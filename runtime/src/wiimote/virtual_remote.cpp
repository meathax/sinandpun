#include "virtual_remote.h"

#include "dual_analog.h"
#include "input_bindings.h"
#include "runtime_log.h"
#include "wiimote_host.h"

#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace VirtualRemote {
namespace {

using Clock = std::chrono::steady_clock;
using Report = std::vector<uint8_t>;

// Wii Remote button bits (the two core-button bytes of every input report).
constexpr uint16_t kBtnLeft = 0x0100, kBtnRight = 0x0200, kBtnDown = 0x0400, kBtnUp = 0x0800, kBtnPlus = 0x1000;
constexpr uint16_t kBtnTwo = 0x0001, kBtnOne = 0x0002, kBtnB = 0x0004, kBtnA = 0x0008, kBtnMinus = 0x0010,
                   kBtnHome = 0x0080;

constexpr uint32_t kSpaceEeprom = 0;
constexpr uint32_t kRegSpeaker = 0xA20000, kRegExtension = 0xA40000, kRegIr = 0xB00000;
constexpr size_t kEepromSize = 0x1700;
constexpr auto kReportInterval = std::chrono::milliseconds(10);

float Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis) {
    const int16_t raw = SDL_GetGamepadAxis(pad, axis);
    return raw < 0 ? static_cast<float>(raw) / 32768.0f : static_cast<float>(raw) / 32767.0f;
}

bool Held(SDL_Gamepad* pad, SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); }

// Sum-based calibration checksum used by the Wii Remote EEPROM and Nunchuk.
void Checksum(uint8_t* data, size_t length, size_t checksumBytes) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length - checksumBytes; ++i) sum = static_cast<uint8_t>(sum + data[i]);
    data[length - checksumBytes] = static_cast<uint8_t>(sum + 0x55);
    if (checksumBytes > 1) data[length - checksumBytes + 1] = static_cast<uint8_t>(sum + 0xAA);
}

class VirtualWiimote {
public:
    VirtualWiimote() { ResetState(); }

    void ResetState() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_leds = 0;
        m_mode = 0;
        m_continuous = false;
        m_irEnabled = false;
        m_speakerEnabled = false;
        m_irMode = 1;
        m_pending.clear();
        m_nextReport = Clock::now();
        m_ext.fill(0);
        m_ir.fill(0);
        m_eeprom.fill(0);
        BuildEeprom();
        m_pointer = DualAnalog::PointerState{};
        m_pointerStamp = Clock::now();
        m_scriptStart = Clock::now();
    }

    void Output(const uint8_t* report, size_t size) {
        if (size < 2) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint8_t id = report[0];
        switch (id) {
        case 0x10: break; // rumble only
        case 0x11: m_leds = static_cast<uint8_t>(report[1] >> 4); Ack(id); break;
        case 0x12:
            if (size >= 3) {
                m_continuous = (report[1] & 0x04) != 0;
                m_mode = report[2];
                m_nextReport = Clock::now();
            }
            Ack(id);
            break;
        case 0x13: m_irEnabled = (report[1] & 0x04) != 0; Ack(id); break;
        case 0x14: m_speakerEnabled = (report[1] & 0x04) != 0; Ack(id); break;
        case 0x15: QueueStatus(); break;
        case 0x16: if (size >= 7) WriteMemory(report, size); break;
        case 0x17: if (size >= 7) ReadMemory(report); break;
        case 0x18: Ack(id); break;
        case 0x19: Ack(id); break;
        case 0x1A: m_irEnabled = (report[1] & 0x04) != 0; Ack(id); break;
        default: Ack(id); break;
        }
    }

    bool Pop(Report& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pending.empty()) {
            out = std::move(m_pending.front());
            m_pending.pop_front();
            return true;
        }
        if (m_mode == 0) return false;
        const auto now = Clock::now();
        if (now < m_nextReport) return false;
        m_nextReport += kReportInterval;
        if (m_nextReport < now) m_nextReport = now + kReportInterval;
        BuildDataReport(out);
        return true;
    }

private:
    struct Input {
        uint16_t buttons = 0;
        bool c = false, z = false;
        uint8_t sx = 0x80, sy = 0x80;
        float rx = 0, ry = 0;
    };

    void Ack(uint8_t id, uint8_t error = 0) {
        Report r{0x22, 0, 0, id, error};
        FillButtons(r, 1);
        m_pending.push_back(std::move(r));
    }

    void FillButtons(Report& r, size_t at) {
        const Input in = ReadInput();
        r[at] = static_cast<uint8_t>(in.buttons >> 8);
        r[at + 1] = static_cast<uint8_t>(in.buttons & 0xFF);
    }

    void QueueStatus() {
        Report r(7, 0);
        r[0] = 0x20;
        FillButtons(r, 1);
        r[3] = static_cast<uint8_t>((m_leds << 4) | 0x02 /* extension */ | (m_speakerEnabled ? 0x04 : 0) |
                                    (m_irEnabled ? 0x08 : 0));
        r[6] = 0xB0; // battery
        m_pending.push_back(std::move(r));
    }

    void BuildEeprom() {
        // Accelerometer calibration: zero 0x80, one g 0x9A, checksummed; stored twice.
        uint8_t accel[10] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0x00, 0x00};
        Checksum(accel, sizeof accel, 1);
        std::memcpy(&m_eeprom[0x16], accel, sizeof accel);
        std::memcpy(&m_eeprom[0x20], accel, sizeof accel);
        // Nunchuk calibration lives in the extension register block.
        uint8_t nunchuk[16] = {0x80, 0x80, 0x80, 0x00, // accel zero
                               0xB3, 0xB3, 0xB3, 0x00, // accel one g
                               0xE0, 0x20, 0x80,       // stick X max/min/center
                               0xE0, 0x20, 0x80,       // stick Y max/min/center
                               0x00, 0x00};
        Checksum(nunchuk, sizeof nunchuk, 2);
        std::memcpy(&m_ext[0x20], nunchuk, sizeof nunchuk);
        std::memcpy(&m_ext[0x30], nunchuk, sizeof nunchuk);
        static const uint8_t id[6] = {0x00, 0x00, 0xA4, 0x20, 0x00, 0x00};
        std::memcpy(&m_ext[0xFA], id, sizeof id);
    }

    uint8_t* Region(uint32_t address, size_t length, uint32_t& offset) {
        const uint32_t base = address & 0xFF0000;
        if (base == kRegExtension) {
            offset = address & 0xFFFF;
            return offset + length <= m_ext.size() ? m_ext.data() : nullptr;
        }
        if (base == kRegIr) {
            offset = address & 0xFFFF;
            return offset + length <= m_ir.size() ? m_ir.data() : nullptr;
        }
        return nullptr;
    }

    void WriteMemory(const uint8_t* r, size_t size) {
        const bool registers = (r[1] & 0x04) != 0;
        const uint32_t address = (r[2] << 16) | (r[3] << 8) | r[4];
        const size_t length = std::min<size_t>(r[5], size - 6);
        uint8_t error = 0;
        if (registers) {
            uint32_t offset = 0;
            if (uint8_t* region = Region(address, length, offset)) {
                std::memcpy(region + offset, r + 6, length);
                if ((address & 0xFF0000) == kRegIr && address == 0xB00033 && length >= 1) m_irMode = r[6];
            } else if ((address & 0xFF0000) != kRegSpeaker) {
                error = 7;
            }
        } else if (address + length <= kEepromSize) {
            std::memcpy(&m_eeprom[address], r + 6, length);
        } else {
            error = 7;
        }
        Ack(0x16, error);
    }

    void ReadMemory(const uint8_t* r) {
        const bool registers = (r[1] & 0x04) != 0;
        uint32_t address = (r[2] << 16) | (r[3] << 8) | r[4];
        size_t remaining = (r[5] << 8) | r[6];
        while (remaining > 0) {
            const size_t chunk = std::min<size_t>(remaining, 16);
            Report reply(21, 0);
            reply[0] = 0x21;
            FillButtons(reply, 1);
            uint8_t error = 0;
            const uint8_t* src = nullptr;
            if (registers) {
                uint32_t offset = 0;
                if (const uint8_t* region = Region(address, chunk, offset)) src = region + offset;
                else if ((address & 0xFF0000) == kRegSpeaker) src = m_zero.data();
            } else if ((address & 0xFFFFFF) + chunk <= kEepromSize) {
                src = &m_eeprom[address & 0xFFFFFF];
            }
            if (!src) error = 8;
            reply[3] = static_cast<uint8_t>(((chunk - 1) << 4) | error);
            reply[4] = static_cast<uint8_t>((address >> 8) & 0xFF);
            reply[5] = static_cast<uint8_t>(address & 0xFF);
            if (src && !error) std::memcpy(&reply[6], src, chunk);
            m_pending.push_back(std::move(reply));
            address += static_cast<uint32_t>(chunk);
            remaining -= chunk;
        }
    }

    // ---- Deterministic test input (SIN_VREMOTE_SCRIPT) -------------------------------------------
    // Segments separated by ';', each "seconds[:buttons[:rx,ry[:lx,ly]]]" with buttons joined by '+'
    // (A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT C Z) and stick values in -1..1 (right stick = pointer,
    // left stick = Nunchuk). Time runs from when the virtual remote connects to the game. When set, the
    // gamepad is ignored so a run needs no human. A "loop" segment makes the script restart at the end.
    struct ScriptSegment {
        float seconds = 0;
        uint16_t buttons = 0;
        bool c = false, z = false;
        float rx = 0, ry = 0, lx = 0, ly = 0;
    };

    static std::vector<std::string> Split(const std::string& text, char sep) {
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

    static const std::vector<ScriptSegment>& Script(bool& loop) {
        static bool s_loop = false;
        static const std::vector<ScriptSegment> segments = [] {
            std::vector<ScriptSegment> out;
            const char* env = std::getenv("SIN_VREMOTE_SCRIPT");
            if (!env || !*env) return out;
            // '|' is accepted for ';' and '/' for ',' so the script survives shells and env lists that split on those.
            std::string text(env);
            for (char& ch : text) {
                if (ch == '|') ch = ';';
                else if (ch == '/') ch = ',';
            }
            for (const std::string& seg : Split(text, ';')) {
                if (seg == "loop") { s_loop = true; continue; }
                if (seg.empty()) continue;
                const std::vector<std::string> parts = Split(seg, ':');
                ScriptSegment s;
                s.seconds = static_cast<float>(std::atof(parts[0].c_str()));
                if (parts.size() > 1) {
                    for (const std::string& name : Split(parts[1], '+')) {
                        if (name == "A") s.buttons |= kBtnA;
                        else if (name == "B") s.buttons |= kBtnB;
                        else if (name == "1") s.buttons |= kBtnOne;
                        else if (name == "2") s.buttons |= kBtnTwo;
                        else if (name == "PLUS") s.buttons |= kBtnPlus;
                        else if (name == "MINUS") s.buttons |= kBtnMinus;
                        else if (name == "HOME") s.buttons |= kBtnHome;
                        else if (name == "UP") s.buttons |= kBtnUp;
                        else if (name == "DOWN") s.buttons |= kBtnDown;
                        else if (name == "LEFT") s.buttons |= kBtnLeft;
                        else if (name == "RIGHT") s.buttons |= kBtnRight;
                        else if (name == "C") s.c = true;
                        else if (name == "Z") s.z = true;
                    }
                }
                auto pair = [](const std::string& v, float& x, float& y) {
                    const std::vector<std::string> xy = Split(v, ',');
                    if (xy.size() == 2) {
                        x = static_cast<float>(std::atof(xy[0].c_str()));
                        y = static_cast<float>(std::atof(xy[1].c_str()));
                    }
                };
                if (parts.size() > 2) pair(parts[2], s.rx, s.ry);
                if (parts.size() > 3) pair(parts[3], s.lx, s.ly);
                out.push_back(s);
            }
            if (!out.empty()) {
                RT_LOGF(RT_TAG_BT, "SIN_VREMOTE_SCRIPT active: %u segment(s), loop=%d (gamepad ignored)\n",
                        static_cast<unsigned>(out.size()), s_loop ? 1 : 0);
            }
            return out;
        }();
        loop = s_loop;
        return segments;
    }

    // Returns true and fills `in` when a script is active.
    bool ScriptedInput(Input& in) {
        bool loop = false;
        const auto& segments = Script(loop);
        if (segments.empty()) return false;
        float total = 0;
        for (const auto& s : segments) total += s.seconds;
        float at = std::chrono::duration<float>(Clock::now() - m_scriptStart).count();
        if (loop && total > 0) at = std::fmod(at, total);
        float acc = 0;
        for (const auto& s : segments) {
            if (at < acc + s.seconds) {
                in.buttons = s.buttons;
                in.c = s.c;
                in.z = s.z;
                const DualAnalog::NunchukState nun = DualAnalog::NunchukFromStick(s.lx, s.ly);
                in.sx = nun.x;
                in.sy = nun.y;
                in.rx = s.rx;
                in.ry = s.ry;
                return true;
            }
            acc += s.seconds;
        }
        return true; // script finished: everything released
    }

    // Gamepad -> remote/nunchuk state. Called with the mutex held.
    Input ReadInput() {
        Input in;
        if (ScriptedInput(in)) return in;
        if (InputBindings::InputBlocked()) return in;
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
        if (!pad) return in;
        auto set = [&](bool down, uint16_t bit) { if (down) in.buttons |= bit; };
        set(Held(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT), kBtnLeft);
        set(Held(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT), kBtnRight);
        set(Held(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN), kBtnDown);
        set(Held(pad, SDL_GAMEPAD_BUTTON_DPAD_UP), kBtnUp);
        set(Held(pad, SDL_GAMEPAD_BUTTON_START), kBtnPlus);
        set(Held(pad, SDL_GAMEPAD_BUTTON_BACK), kBtnMinus);
        set(Held(pad, SDL_GAMEPAD_BUTTON_GUIDE), kBtnHome);
        set(Held(pad, SDL_GAMEPAD_BUTTON_WEST), kBtnOne);
        set(Held(pad, SDL_GAMEPAD_BUTTON_NORTH), kBtnTwo);
        set(Held(pad, SDL_GAMEPAD_BUTTON_SOUTH), kBtnA);
        set(Held(pad, SDL_GAMEPAD_BUTTON_EAST), kBtnA);
        set(Axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.4f, kBtnB);
        set(Held(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), kBtnB);
        in.c = Held(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        in.z = Axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.4f;
        const DualAnalog::NunchukState nun =
            DualAnalog::NunchukFromStick(Axis(pad, SDL_GAMEPAD_AXIS_LEFTX), -Axis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        in.sx = nun.x;
        in.sy = nun.y;
        in.rx = Axis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        in.ry = Axis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
        return in;
    }

    void BuildIr(uint8_t* out, bool extended) {
        uint16_t x0, y0, x1, y1;
        DualAnalog::EmitDots(m_pointer, m_config, x0, y0, x1, y1);
        if (extended) {
            const uint16_t xs[2] = {x0, x1}, ys[2] = {y0, y1};
            for (int i = 0; i < 4; ++i) {
                uint8_t* p = out + i * 3;
                if (i < 2) {
                    p[0] = xs[i] & 0xFF;
                    p[1] = ys[i] & 0xFF;
                    p[2] = static_cast<uint8_t>(((ys[i] >> 8) << 6) | ((xs[i] >> 8) << 4) | 4);
                } else {
                    p[0] = p[1] = p[2] = 0xFF;
                }
            }
        } else {
            out[0] = x0 & 0xFF;
            out[1] = y0 & 0xFF;
            out[2] = static_cast<uint8_t>(((y0 >> 8) << 6) | ((x0 >> 8) << 4) | ((y1 >> 8) << 2) | (x1 >> 8));
            out[3] = x1 & 0xFF;
            out[4] = y1 & 0xFF;
            std::memset(out + 5, 0xFF, 5);
        }
    }

    void BuildNunchuk(uint8_t* out, const Input& in) {
        out[0] = in.sx;
        out[1] = in.sy;
        out[2] = 0x80; // accel X
        out[3] = 0x80; // accel Y
        out[4] = 0xB3; // accel Z (one g while held level)
        out[5] = static_cast<uint8_t>(0x03 & ~((in.z ? 1 : 0) | (in.c ? 2 : 0)));
    }

    void BuildDataReport(Report& out) {
        const Input in = ReadInput();
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - m_pointerStamp).count();
        m_pointerStamp = now;
        // Stick right moves the pointer right, which moves the sensor bar left in the camera.
        DualAnalog::Advance(m_pointer, m_config, -in.rx, -in.ry, dt);
        {
            static int s_tick = 0;
            static int s_budget = 2000;
            static float s_lastRx = 9, s_lastRy = 9;
            static uint16_t s_lastButtons = 0xFFFF;
            const bool changed = std::fabs(in.rx - s_lastRx) > 0.05f || std::fabs(in.ry - s_lastRy) > 0.05f ||
                                 in.buttons != s_lastButtons;
            if (changed) {
                s_lastRx = in.rx;
                s_lastRy = in.ry;
                s_lastButtons = in.buttons;
            }
            if ((changed || (++s_tick % 100) == 0 || (changed && false)) && RtLogBudget(s_budget)) {
                RT_LOGF(RT_TAG_BT, "pointer cam=(%.0f,%.0f) stick R=(%.2f,%.2f) L=(0x%02X,0x%02X) buttons=0x%04X\n",
                        m_pointer.x, m_pointer.y, in.rx, in.ry, in.sx, in.sy, in.buttons);
            }
        }

        static const struct { uint8_t id; size_t size; } kSizes[] = {
            {0x30, 3}, {0x31, 6}, {0x32, 11}, {0x33, 18}, {0x34, 22}, {0x35, 22}, {0x36, 22}, {0x37, 22}, {0x3D, 22}};
        size_t size = 3;
        for (const auto& s : kSizes) if (s.id == m_mode) size = s.size;
        out.assign(size, 0);
        out[0] = m_mode;
        out[1] = static_cast<uint8_t>(in.buttons >> 8);
        out[2] = static_cast<uint8_t>(in.buttons & 0xFF);
        auto accel = [&](size_t at) { out[at] = 0x80; out[at + 1] = 0x80; out[at + 2] = 0x9A; };
        uint8_t ext[21] = {};
        BuildNunchuk(ext, in);
        switch (m_mode) {
        case 0x31: accel(3); break;
        case 0x32: std::memcpy(&out[3], ext, 8); break;
        case 0x33: accel(3); BuildIr(&out[6], true); break;
        case 0x35: accel(3); std::memcpy(&out[6], ext, 16); break;
        case 0x36: BuildIr(&out[3], false); std::memcpy(&out[13], ext, 9); break;
        case 0x37: accel(3); BuildIr(&out[6], false); std::memcpy(&out[16], ext, 6); break;
        case 0x34: std::memcpy(&out[3], ext, 19); break;
        case 0x3D: std::memcpy(&out[1], ext, 21); break;
        default: break;
        }
    }

    std::mutex m_mutex;
    uint8_t m_leds = 0;
    uint8_t m_mode = 0;
    bool m_continuous = false;
    bool m_irEnabled = false;
    bool m_speakerEnabled = false;
    uint8_t m_irMode = 1;
    std::deque<Report> m_pending;
    Clock::time_point m_nextReport;
    Clock::time_point m_pointerStamp;
    Clock::time_point m_scriptStart = Clock::now();
    std::array<uint8_t, 0x100> m_ext{};
    std::array<uint8_t, 0x100> m_ir{};
    std::array<uint8_t, 0x20> m_zero{};
    std::array<uint8_t, kEepromSize> m_eeprom{};
    DualAnalog::Config m_config;
    DualAnalog::PointerState m_pointer;
};

class CompositeHost final : public Bluetooth::RemoteHost {
public:
    bool IsAttached(size_t slot) const override {
        if (slot == 0 && UseVirtual()) {
            static int s_budget = 8;
            if (RtLogBudget(s_budget)) {
                RT_LOG(RT_TAG_BT) << "IsAttached slot=0 -> true (virtual)" << std::endl;
            }
            return true;
        }
        return Wiimote::Host().IsAttached(slot);
    }

    bool ConsumeWake(size_t slot) override {
        if (slot == 0 && UseVirtual()) {
            const bool woke = m_wake.exchange(false);
            static int s_budget = 8;
            if (RtLogBudget(s_budget)) {
                RT_LOG(RT_TAG_BT) << "ConsumeWake slot=0 -> " << (woke ? "true" : "false") << std::endl;
            }
            return woke;
        }
        return Wiimote::Host().ConsumeWake(slot);
    }

    bool PopInputReport(size_t slot, std::vector<uint8_t>& report) override {
        if (slot == 0 && UseVirtual()) {
            const bool got = m_remote.Pop(report);
            static int s_budget = 100;
            if (got && RtLogBudget(s_budget)) {
                RT_LOGF(RT_TAG_BT, "in  id=0x%02X size=%u\n", report.empty() ? 0 : report[0],
                        static_cast<unsigned>(report.size()));
            }
            return got;
        }
        return Wiimote::Host().PopInputReport(slot, report);
    }

    void SendOutputReport(size_t slot, const uint8_t* report, size_t size) override {
        if (slot == 0 && UseVirtual()) {
            static int s_budget = 160;
            if (RtLogBudget(s_budget)) {
                RT_LOGF(RT_TAG_BT, "out id=0x%02X size=%u b1..6=%02X %02X %02X %02X %02X %02X\n",
                        size != 0 ? report[0] : 0, static_cast<unsigned>(size),
                        size > 1 ? report[1] : 0, size > 2 ? report[2] : 0, size > 3 ? report[3] : 0,
                        size > 4 ? report[4] : 0, size > 5 ? report[5] : 0, size > 6 ? report[6] : 0);
            }
            m_remote.Output(report, size);
            return;
        }
        Wiimote::Host().SendOutputReport(slot, report, size);
    }

    void OnLinkStateChanged(size_t slot, Bluetooth::LinkState state) override {
        if (slot == 0 && UseVirtual()) {
            if (state == Bluetooth::LinkState::Ready) {
                m_remote.ResetState();
                RT_LOG(RT_TAG_HLE) << "Virtual Wii Remote + Nunchuk connected to the game (gamepad)" << std::endl;
            } else if (state == Bluetooth::LinkState::Down) {
                m_wake = true;
            }
            return;
        }
        Wiimote::Host().OnLinkStateChanged(slot, state);
    }

    void OnGuestDisconnect(size_t slot) override {
        if (slot == 0 && UseVirtual()) {
            RT_LOG(RT_TAG_HLE) << "Virtual Wii Remote disconnected by the game" << std::endl;
            return;
        }
        Wiimote::Host().OnGuestDisconnect(slot);
    }

    void OnGuestInquiry(bool active) override { Wiimote::Host().OnGuestInquiry(active); }

    const char* DeviceName(size_t slot) const override {
        if (slot == 0 && UseVirtual()) return "Nintendo RVL-CNT-01";
        return Wiimote::Host().DeviceName(slot);
    }

private:
    bool UseVirtual() const {
        static const bool forced = [] {
            const char* v = std::getenv("SIN_VIRTUAL_REMOTE");
            return v && *v && *v != '0';
        }();
        if (Wiimote::Host().IsAttached(0)) return false;
        return forced || PADGetSDLGamepadForIndex(0) != nullptr;
    }

    VirtualWiimote m_remote;
    std::atomic<bool> m_wake{true};
};

} // namespace

Bluetooth::RemoteHost& Host() {
    static CompositeHost* host = new CompositeHost();
    return *host;
}

} // namespace VirtualRemote
