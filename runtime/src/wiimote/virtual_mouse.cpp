#include "virtual_mouse.h"

#include "aim_input.h"
#include "aim_settings.h"
#include "runtime_log.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace VirtualMouse {
namespace {

struct Segment {
    float seconds = 1.f;
    bool hasAt = false;
    float ax = 0.5f, ay = 0.5f;
    bool ramp = false;
    bool jit = false;
    bool hasVel = false;
    float vx = 0.f, vy = 0.f;
    uint8_t buttons = 0;  // bit n = SDL button n+1
    std::vector<int> keys;
    bool rep = false;
    int wheel = 0;
    int focus = 0;  // 1 lost, 2 gain
    bool leave = false;
    std::string mark;
    // Live settings applied at the segment start (same path as the overlay: AimSettings::Set).
    float sm = -1.f, sens = -1.f, rngX = -1.f, rngY = -1.f;
    int raw = -1, en = -1;
    std::string bind;  // action key name (e.g. "move_left"); the key given in k= is captured for it
};

std::vector<std::string> Split(const std::string& s, const char* seps) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::strchr(seps, c) != nullptr) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

bool Pair(const std::string& v, float& a, float& b) {
    const size_t slash = v.find('/');
    if (slash == std::string::npos) return false;
    a = static_cast<float>(std::atof(v.substr(0, slash).c_str()));
    b = static_cast<float>(std::atof(v.substr(slash + 1).c_str()));
    return true;
}

std::vector<Segment> Parse(const std::string& text) {
    std::vector<Segment> out;
    for (const std::string& raw : Split(text, "|;")) {
        if (raw.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        const std::vector<std::string> fields = Split(raw, ":");
        Segment s;
        s.seconds = std::max(0.01f, static_cast<float>(std::atof(fields[0].c_str())));
        for (size_t i = 1; i < fields.size(); ++i) {
            const std::string& f = fields[i];
            const size_t eq = f.find('=');
            const std::string key = eq == std::string::npos ? f : f.substr(0, eq);
            const std::string val = eq == std::string::npos ? "" : f.substr(eq + 1);
            if (key == "at") {
                s.hasAt = Pair(val, s.ax, s.ay);
            } else if (key == "ramp") {
                s.ramp = true;
            } else if (key == "jit") {
                s.jit = true;
            } else if (key == "v") {
                s.hasVel = Pair(val, s.vx, s.vy);
            } else if (key == "b") {
                for (const std::string& n : Split(val, "+")) {
                    if (n == "L") s.buttons |= 1;
                    else if (n == "M") s.buttons |= 2;
                    else if (n == "R") s.buttons |= 4;
                    else if (n == "X1") s.buttons |= 8;
                    else if (n == "X2") s.buttons |= 16;
                }
            } else if (key == "k") {
                for (const std::string& n : Split(val, "+")) {
                    const SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
                    if (sc != SDL_SCANCODE_UNKNOWN) s.keys.push_back(static_cast<int>(sc));
                    else if (!n.empty()) RT_LOGF(RT_TAG_BT, "[vmouse] unknown key name '%s'\n", n.c_str());
                }
            } else if (key == "rep") {
                s.rep = true;
            } else if (key == "wheel") {
                s.wheel = std::atoi(val.c_str());
            } else if (key == "focus") {
                s.focus = val == "lost" ? 1 : val == "gain" ? 2 : 0;
            } else if (key == "leave") {
                s.leave = true;
            } else if (key == "mark") {
                s.mark = val;
            } else if (key == "sm") {
                s.sm = static_cast<float>(std::atof(val.c_str()));
            } else if (key == "sens") {
                s.sens = static_cast<float>(std::atof(val.c_str()));
            } else if (key == "rng") {
                Pair(val, s.rngX, s.rngY);
            } else if (key == "raw") {
                s.raw = std::atoi(val.c_str());
            } else if (key == "en") {
                s.en = std::atoi(val.c_str());
            } else if (key == "bind") {
                s.bind = val;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

struct State {
    bool init = false;
    bool enabled = false;
    bool warp = false;
    int logMs = 100;
    std::vector<Segment> segs;
    int cur = -1;
    float segStart = 0.f;
    float fx = 0.5f, fy = 0.5f;      // current cursor, window fractions
    float fromX = 0.5f, fromY = 0.5f;
    float lastPx = -1.f, lastPy = -1.f;
    uint8_t buttons = 0;
    std::vector<int> keys;
    Uint64 lastRepeat = 0;
    Uint64 lastLog = 0;
    bool jitFlip = false;
    bool finished = false;
    int bindAction = -1;
};

State& S() {
    static State s;
    return s;
}

std::string FileText(const char* path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void Init(State& s) {
    s.init = true;
    const char* file = std::getenv("SIN_VMOUSE_SCRIPT_FILE");
    const char* env = std::getenv("SIN_VMOUSE_SCRIPT");
    std::string text;
    if (file != nullptr && *file) text = FileText(file);
    else if (env != nullptr) text = env;
    if (text.empty()) return;
    s.segs = Parse(text);
    s.enabled = !s.segs.empty();
    const char* warp = std::getenv("SIN_VMOUSE_WARP");
    s.warp = warp != nullptr && std::atoi(warp) != 0;
    if (const char* l = std::getenv("SIN_VMOUSE_LOG")) s.logMs = std::atoi(l);
    RT_LOGF(RT_TAG_BT, "[vmouse] script active: %u segment(s), warp=%d\n", static_cast<unsigned>(s.segs.size()), s.warp ? 1 : 0);
}

void PushMotion(SDL_WindowID id, float x, float y, float xrel, float yrel, uint8_t buttons) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.timestamp = SDL_GetTicksNS();
    e.motion.windowID = id;
    e.motion.which = 1;
    e.motion.state = static_cast<SDL_MouseButtonFlags>(buttons);
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = xrel;
    e.motion.yrel = yrel;
    SDL_PushEvent(&e);
}

void PushButton(SDL_WindowID id, int button, bool down, float x, float y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.timestamp = SDL_GetTicksNS();
    e.button.windowID = id;
    e.button.which = 1;
    e.button.button = static_cast<Uint8>(button);
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

void PushKey(SDL_WindowID id, int scancode, bool down, bool repeat) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = id;
    e.key.which = 1;
    e.key.scancode = static_cast<SDL_Scancode>(scancode);
    e.key.key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), SDL_KMOD_NONE, false);
    e.key.mod = SDL_KMOD_NONE;
    e.key.down = down;
    e.key.repeat = repeat;
    SDL_PushEvent(&e);
}

void PushWindow(SDL_WindowID id, SDL_EventType type) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.window.timestamp = SDL_GetTicksNS();
    e.window.windowID = id;
    SDL_PushEvent(&e);
}

void ApplySettings(State& s, const Segment& seg) {
    if (seg.sm < 0.f && seg.sens < 0.f && seg.rngX < 0.f && seg.raw < 0 && seg.en < 0 && seg.bind.empty()) return;
    if (seg.bind.empty()) {
        AimSettings::Settings cfg = AimSettings::Get();
        if (seg.sm >= 0.f) cfg.mouse.smoothingMs = seg.sm;
        if (seg.sens >= 0.f) cfg.mouse.sensitivity = seg.sens;
        if (seg.rngX >= 0.f) {
            cfg.mouse.rangeX = seg.rngX;
            cfg.mouse.rangeY = seg.rngY;
        }
        if (seg.raw >= 0) cfg.mouse.raw = seg.raw != 0;
        if (seg.en >= 0) cfg.mouse.enabled = seg.en != 0;
        AimSettings::Sanitize(cfg);
        AimSettings::Set(cfg);
        RT_LOGF(RT_TAG_BT, "[vmouse] live settings: smoothing=%.0f sens=%.2f range=%.2f,%.2f raw=%d enabled=%d\n",
                cfg.mouse.smoothingMs, cfg.mouse.sensitivity, cfg.mouse.rangeX, cfg.mouse.rangeY, cfg.mouse.raw ? 1 : 0,
                cfg.mouse.enabled ? 1 : 0);
        return;
    }
    for (size_t a = 0; a < AimSettings::kActionCount; ++a) {
        if (seg.bind == AimSettings::kActions[a].key) {
            s.bindAction = static_cast<int>(a);
            AimInput::BeginCapture(InputMap::Profile::KeyboardMouse);
            RT_LOGF(RT_TAG_BT, "[vmouse] bind: capturing for %s\n", seg.bind.c_str());
        }
    }
}

void PollBind(State& s) {
    if (s.bindAction < 0) return;
    const AimInput::CaptureResult r = AimInput::PollCapture(false);
    if (r.state == AimInput::CaptureState::Idle || r.state == AimInput::CaptureState::Pending) return;
    if (r.state == AimInput::CaptureState::Done || r.state == AimInput::CaptureState::Cleared) {
        AimSettings::Settings cfg = AimSettings::Get();
        const InputMap::Binding b = r.state == AimInput::CaptureState::Done ? r.binding : InputMap::Binding{};
        const auto swapped = InputMap::AssignSwap(cfg.bindings.kbm, static_cast<size_t>(s.bindAction), 0, b);
        AimSettings::Set(cfg);
        const bool saved = AimSettings::Save();
        RT_LOGF(RT_TAG_BT, "[vmouse] bind: %s slot0 -> '%s' swapped=%s saved=%d\n",
                AimSettings::kActions[static_cast<size_t>(s.bindAction)].key, InputMap::Format(b).c_str(),
                swapped ? AimSettings::kActions[*swapped].key : "-", saved ? 1 : 0);
    }
    s.bindAction = -1;
}

void Log(State& s, SDL_Window* window, float t, float winW, float winH) {
    const AimInput::MouseSample ms = AimInput::MouseNow();
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    float vl = 0.f, vt = 0.f, vw = 0.f, vh = 0.f;
    const bool vp = aurora_get_present_viewport(&vl, &vt, &vw, &vh);
    float gx = 0.f, gy = 0.f;
    SDL_GetGlobalMouseState(&gx, &gy);
    int wx = 0, wy = 0;
    SDL_GetWindowPosition(window, &wx, &wy);
    const InputMap::Scheme scheme = AimInput::EffectiveScheme();
    const InputMap::ActiveDevice dev = AimInput::LastDevice();
    RT_LOGF(RT_TAG_BT,
            "[vmouse] t=%.3f seg=%d want=%.4f,%.4f mouse=%.1f,%.1f valid=%d win=%.0fx%.0f pix=%dx%d vp=%d:%.4f,%.4f,%.4f,%.4f "
            "os=%.1f,%.1f winpos=%d,%d cursor=%d rel=%d scheme=%s dev=%s foc=%d btn=%u\n",
            t, s.cur, s.fx, s.fy, ms.x, ms.y, ms.valid ? 1 : 0, winW, winH, pw, ph, vp ? 1 : 0, vl, vt, vw, vh, gx, gy, wx, wy,
            SDL_CursorVisible() ? 1 : 0, SDL_GetWindowRelativeMouseMode(window) ? 1 : 0,
            scheme == InputMap::Scheme::Classic ? "classic" : "pointer",
            dev == InputMap::ActiveDevice::Gamepad ? "pad" : dev == InputMap::ActiveDevice::KeyboardMouse ? "kbm" : "none",
            AimInput::Focused() ? 1 : 0, static_cast<unsigned>(s.buttons));
}

} // namespace

bool Active() {
    State& s = S();
    if (!s.init) Init(s);
    return s.enabled;
}

void Tick() {
    State& s = S();
    if (!s.init) Init(s);
    if (!s.enabled) return;
    SDL_Window* window = aurora_get_window();
    if (window == nullptr) return;
    const SDL_WindowID id = SDL_GetWindowID(window);
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    const float t = static_cast<float>(SDL_GetTicks()) / 1000.f;
    PollBind(s);

    // Which segment are we in?
    float acc = 0.f;
    int index = -1;
    for (size_t i = 0; i < s.segs.size(); ++i) {
        if (t < acc + s.segs[i].seconds) {
            index = static_cast<int>(i);
            break;
        }
        acc += s.segs[i].seconds;
    }
    const bool done = index < 0;
    if (done && !s.finished) {
        index = static_cast<int>(s.segs.size());
    }
    if (done && s.finished) return;

    // Segment change: release what the old segment held, apply the new one.
    auto px = [&](float f, int n) { return f * static_cast<float>(n); };
    while (s.cur < index) {
        ++s.cur;
        const bool real = s.cur < static_cast<int>(s.segs.size());
        const Segment empty;
        const Segment& seg = real ? s.segs[static_cast<size_t>(s.cur)] : empty;
        float segStart = 0.f;
        for (int i = 0; i < s.cur; ++i) segStart += s.segs[static_cast<size_t>(i)].seconds;
        s.segStart = segStart;
        if (real) {
            RT_LOGF(RT_TAG_BT, "[vmouse] t=%.3f segment %d start%s%s at=%s b=0x%02X keys=%u\n", t, s.cur,
                    seg.mark.empty() ? "" : " mark=", seg.mark.c_str(), seg.hasAt ? "yes" : "hold", seg.buttons,
                    static_cast<unsigned>(seg.keys.size()));
        } else {
            RT_LOGF(RT_TAG_BT, "[vmouse] t=%.3f script finished: everything released\n", t);
            s.finished = true;
        }
        if (real) ApplySettings(s, seg);
        if (seg.focus == 1) PushWindow(id, SDL_EVENT_WINDOW_FOCUS_LOST);
        if (seg.focus == 2) PushWindow(id, SDL_EVENT_WINDOW_FOCUS_GAINED);
        if (seg.leave) PushWindow(id, SDL_EVENT_WINDOW_MOUSE_LEAVE);
        // Buttons
        for (int b = 0; b < 5; ++b) {
            const bool was = (s.buttons >> b) & 1, now = (seg.buttons >> b) & 1;
            if (was != now) PushButton(id, b + 1, now, px(s.fx, ww), px(s.fy, wh));
        }
        s.buttons = seg.buttons;
        // Keys
        for (int k : s.keys) {
            if (std::find(seg.keys.begin(), seg.keys.end(), k) == seg.keys.end()) PushKey(id, k, false, false);
        }
        for (int k : seg.keys) {
            if (std::find(s.keys.begin(), s.keys.end(), k) == s.keys.end()) PushKey(id, k, true, false);
        }
        s.keys = seg.keys;
        if (seg.wheel != 0) {
            SDL_Event e;
            SDL_zero(e);
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.timestamp = SDL_GetTicksNS();
            e.wheel.windowID = id;
            e.wheel.which = 1;
            e.wheel.y = static_cast<float>(seg.wheel);
            e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            SDL_PushEvent(&e);
        }
        s.fromX = s.fx;
        s.fromY = s.fy;
        if (seg.hasAt && !seg.ramp) {
            s.fx = seg.ax;
            s.fy = seg.ay;
        }
    }

    if (s.cur >= 0 && s.cur < static_cast<int>(s.segs.size())) {
        const Segment& seg = s.segs[static_cast<size_t>(s.cur)];
        const float local = t - s.segStart;
        const float dt = 1.f / 60.f;
        float x = s.fx, y = s.fy;
        float xrel = 0.f, yrel = 0.f;
        if (seg.hasAt && seg.ramp) {
            const float k = std::clamp(local / seg.seconds, 0.f, 1.f);
            x = s.fromX + (seg.ax - s.fromX) * k;
            y = s.fromY + (seg.ay - s.fromY) * k;
        }
        float xp = px(x, ww), yp = px(y, wh);
        if (seg.jit) {
            s.jitFlip = !s.jitFlip;
            xp += s.jitFlip ? 1.f : -1.f;
        }
        if (seg.hasVel) {
            xrel = seg.vx * dt;
            yrel = seg.vy * dt;
        } else if (s.lastPx >= 0.f) {
            xrel = xp - s.lastPx;
            yrel = yp - s.lastPy;
        }
        const bool moved = seg.hasVel || std::fabs(xrel) > 0.001f || std::fabs(yrel) > 0.001f || s.lastPx < 0.f;
        s.fx = x;
        s.fy = y;
        if (moved && (seg.hasAt || seg.hasVel || seg.jit)) {
            if (s.warp && !seg.hasVel) {
                SDL_WarpMouseInWindow(window, xp, yp);
            } else {
                PushMotion(id, xp, yp, xrel, yrel, s.buttons);
            }
            s.lastPx = xp;
            s.lastPy = yp;
        }
        if (seg.rep && !s.keys.empty() && SDL_GetTicks() - s.lastRepeat >= 33) {
            s.lastRepeat = SDL_GetTicks();
            for (int k : s.keys) PushKey(id, k, true, true);
        }
    }

    if (s.logMs > 0 && SDL_GetTicks() - s.lastLog >= static_cast<Uint64>(s.logMs)) {
        s.lastLog = SDL_GetTicks();
        Log(s, window, t, static_cast<float>(ww), static_cast<float>(wh));
    }
}

} // namespace VirtualMouse
