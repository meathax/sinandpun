// Scripted VIRTUAL SDL gamepad for the aim tests (SIN_VPAD_SCRIPT / SIN_VPAD_SCRIPT_FILE).
//
// Unlike SIN_VREMOTE_SCRIPT (which replaces the remote's input and so bypasses the gamepad code entirely) this attaches
// a real SDL3 virtual joystick of type "gamepad". aurora opens it like a physical Xbox pad, so the whole real path is
// exercised: SDL gamepad -> PADGetSDLGamepadForIndex -> AimInput::Sample -> bindings -> deadzone/curve ->
// VirtualRemote -> guest KPAD. The pad also reaches the game's own PAD HLE exactly like hardware.
//
// Script syntax (segments joined by '|' or ';', pairs with '/' or ','; later fields optional):
//   seconds[:buttons[:rx/ry[:lx/ly[:lt/rt]]]]
//   buttons joined by '+': A B X Y (Xbox names = south/east/west/north; also SOUTH EAST WEST NORTH; the game's
//                          Confirm default is pad:east = B, Cancel is pad:south = A), LB RB, LT RT (full trigger pull), START BACK GUIDE,
//                          UP DOWN LEFT RIGHT (d-pad), L3 R3
//   sticks use the SDL convention: +x right, +y DOWN (so "up" is -1); range -1..1; triggers 0..1
//   a segment "loop" restarts the script at its end
// The clock starts when the pad is attached. SIN_VPAD_SCRIPT_FILE=<path> re-reads the file whenever it changes and
// restarts the clock (the test harness writes phases into it). With no script the pad is attached idle.
//
// Environment:
//   SIN_VPAD=1               attach the pad even without a script
//   SIN_VPAD_SCRIPT=...      script text
//   SIN_VPAD_SCRIPT_FILE=... live script file

#include "runtime_log.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace VirtualPad {
namespace {

struct Segment {
    float seconds = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    float rx = 0, ry = 0, lx = 0, ly = 0, lt = 0, rt = 0;
};

std::vector<std::string> Split(const std::string& text, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find(sep, a);
        if (b == std::string::npos) b = text.size();
        out.push_back(text.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

int ButtonByName(const std::string& n) {
    struct Entry { const char* name; int button; };
    static const Entry table[] = {
        {"A", SDL_GAMEPAD_BUTTON_SOUTH}, {"B", SDL_GAMEPAD_BUTTON_EAST}, {"X", SDL_GAMEPAD_BUTTON_WEST},
        {"Y", SDL_GAMEPAD_BUTTON_NORTH}, {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"START", SDL_GAMEPAD_BUTTON_START},
        {"BACK", SDL_GAMEPAD_BUTTON_BACK}, {"GUIDE", SDL_GAMEPAD_BUTTON_GUIDE}, {"UP", SDL_GAMEPAD_BUTTON_DPAD_UP},
        {"DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"L3", SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {"R3", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"SOUTH", SDL_GAMEPAD_BUTTON_SOUTH},
        {"EAST", SDL_GAMEPAD_BUTTON_EAST}, {"WEST", SDL_GAMEPAD_BUTTON_WEST}, {"NORTH", SDL_GAMEPAD_BUTTON_NORTH},
    };
    for (const Entry& e : table) {
        if (n == e.name) return e.button;
    }
    return -1;
}

std::vector<Segment> Parse(const std::string& raw, bool& loop) {
    std::vector<Segment> out;
    loop = false;
    std::string text = raw;
    for (char& c : text) {
        if (c == '|') c = ';';
        else if (c == '/') c = ',';
    }
    for (const std::string& seg : Split(text, ';')) {
        if (seg.empty()) continue;
        if (seg == "loop") {
            loop = true;
            continue;
        }
        const std::vector<std::string> parts = Split(seg, ':');
        Segment s;
        s.seconds = static_cast<float>(std::atof(parts[0].c_str()));
        auto pair = [](const std::string& v, float& x, float& y) {
            const std::vector<std::string> xy = Split(v, ',');
            if (xy.size() == 2) {
                x = static_cast<float>(std::atof(xy[0].c_str()));
                y = static_cast<float>(std::atof(xy[1].c_str()));
            }
        };
        if (parts.size() > 1) {
            for (const std::string& name : Split(parts[1], '+')) {
                if (name == "LT") s.lt = 1.0f;
                else if (name == "RT") s.rt = 1.0f;
                else if (const int b = ButtonByName(name); b >= 0) s.buttons[b] = true;
            }
        }
        if (parts.size() > 2) pair(parts[2], s.rx, s.ry);
        if (parts.size() > 3) pair(parts[3], s.lx, s.ly);
        if (parts.size() > 4) {
            float lt = s.lt, rt = s.rt;
            pair(parts[4], lt, rt);
            s.lt = std::max(s.lt, lt);
            s.rt = std::max(s.rt, rt);
        }
        out.push_back(s);
    }
    return out;
}

Sint16 StickValue(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<Sint16>(std::lround(v < 0 ? v * 32768.0f : v * 32767.0f));
}

Sint16 TriggerValue(float v) { return static_cast<Sint16>(std::lround(std::clamp(v, 0.0f, 1.0f) * 32767.0f)); }

void Run() {
    using Clock = std::chrono::steady_clock;
    // The hint must allow input while the (test) window is in the background; SDL ignores it otherwise.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_Joystick* joystick = nullptr;
    SDL_JoystickID id = 0;
    while (!joystick) {
        if (SDL_WasInit(SDL_INIT_GAMEPAD) != 0) {
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1;
            desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
            desc.name = "Sin Virtual Test Pad";
            id = SDL_AttachVirtualJoystick(&desc);
            if (id != 0) joystick = SDL_OpenJoystick(id);
            if (!joystick) {
                RT_LOGF(RT_TAG_BT, "vpad: attach failed: %s\n", SDL_GetError());
            }
        }
        if (!joystick) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    RT_LOGF(RT_TAG_BT, "vpad: virtual SDL gamepad attached (instance %u)\n", static_cast<unsigned>(id));

    bool loop = false;
    std::vector<Segment> segments;
    if (const char* e = std::getenv("SIN_VPAD_SCRIPT")) segments = Parse(e, loop);
    const char* file = std::getenv("SIN_VPAD_SCRIPT_FILE");
    std::filesystem::file_time_type stamp{};
    auto t0 = Clock::now();
    auto lastCheck = Clock::time_point{};
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> shownB{};
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> shownA{};
    int lastSeg = -2;
    bool first = true;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (file && *file && Clock::now() - lastCheck > std::chrono::milliseconds(200)) {
            lastCheck = Clock::now();
            std::error_code ec;
            const auto st = std::filesystem::last_write_time(file, ec);
            if (!ec && st != stamp) {
                stamp = st;
                std::ifstream in(file);
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
                segments = Parse(text, loop);
                t0 = Clock::now();
                lastSeg = -2;
                RT_LOGF(RT_TAG_BT, "vpad: script file reloaded, %u segment(s)\n", static_cast<unsigned>(segments.size()));
            }
        }
        float total = 0;
        for (const Segment& s : segments) total += s.seconds;
        float at = std::chrono::duration<float>(Clock::now() - t0).count();
        if (loop && total > 0) at = std::fmod(at, total);
        Segment cur;
        int index = -1, i = 0;
        float acc = 0;
        for (const Segment& s : segments) {
            if (at < acc + s.seconds) {
                cur = s;
                index = i;
                break;
            }
            acc += s.seconds;
            ++i;
        }
        if (index != lastSeg) {
            lastSeg = index;
            if (index >= 0) {
                RT_LOGF(RT_TAG_BT, "vpad: t=%.2fs segment %d R=(%.2f,%.2f) L=(%.2f,%.2f) LT/RT=(%.2f,%.2f)\n", at, index,
                        cur.rx, cur.ry, cur.lx, cur.ly, cur.lt, cur.rt);
            }
        }
        std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> axes{};
        axes[SDL_GAMEPAD_AXIS_LEFTX] = StickValue(cur.lx);
        axes[SDL_GAMEPAD_AXIS_LEFTY] = StickValue(cur.ly);
        axes[SDL_GAMEPAD_AXIS_RIGHTX] = StickValue(cur.rx);
        axes[SDL_GAMEPAD_AXIS_RIGHTY] = StickValue(cur.ry);
        axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = TriggerValue(cur.lt);
        axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = TriggerValue(cur.rt);
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            if (first || axes[a] != shownA[a]) {
                SDL_SetJoystickVirtualAxis(joystick, a, axes[a]);
                shownA[a] = axes[a];
            }
        }
        for (int b = 0; b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT; ++b) {
            if (first || cur.buttons[b] != shownB[b]) {
                SDL_SetJoystickVirtualButton(joystick, b, cur.buttons[b]);
                shownB[b] = cur.buttons[b];
            }
        }
        first = false;
    }
}

}  // namespace

// Called once from the virtual remote's constructor (keeps this object file linked from the static library).
void Start() {
    static std::atomic<bool> started{false};
    const char* script = std::getenv("SIN_VPAD_SCRIPT");
    const char* file = std::getenv("SIN_VPAD_SCRIPT_FILE");
    const char* on = std::getenv("SIN_VPAD");
    const bool want = (script && *script) || (file && *file) || (on && *on && *on != '0');
    if (want && !started.exchange(true)) std::thread(Run).detach();
}

}  // namespace VirtualPad
