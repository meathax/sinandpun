#include "virtual_remote.h"

#include "aim_input.h"
#include "aim_settings.h"
#include "dual_analog.h"
#include "input_bindings.h"
#include "input_map.h"
#include "runtime_config.h"
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

// InjectCancel(): press B until this deadline (steady-clock milliseconds).
std::atomic<long long> g_injectCancelUntilMs{0};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

bool InjectActive() { return NowMs() < g_injectCancelUntilMs.load(std::memory_order_relaxed); }

// Wii Remote button bits (the two core-button bytes of every input report).
constexpr uint16_t kBtnLeft = 0x0100, kBtnRight = 0x0200, kBtnDown = 0x0400, kBtnUp = 0x0800, kBtnPlus = 0x1000;
constexpr uint16_t kBtnTwo = 0x0001, kBtnOne = 0x0002, kBtnB = 0x0004, kBtnA = 0x0008, kBtnMinus = 0x0010,
                   kBtnHome = 0x0080;

// Classic Controller button bytes (active low on the wire): byte 4 then byte 5 of the 6-byte report.
constexpr uint8_t kClDRight = 0x80, kClDDown = 0x40, kClL = 0x20, kClMinus = 0x10, kClHome = 0x08, kClPlus = 0x04,
                  kClR = 0x02;
constexpr uint8_t kClZL = 0x80, kClB = 0x40, kClY = 0x20, kClA = 0x10, kClX = 0x08, kClZR = 0x04, kClDLeft = 0x02,
                  kClDUp = 0x01;

constexpr uint32_t kSpaceEeprom = 0;
constexpr uint32_t kRegSpeaker = 0xA20000, kRegExtension = 0xA40000, kRegIr = 0xB00000;
constexpr size_t kEepromSize = 0x1700;
constexpr auto kReportInterval = std::chrono::milliseconds(10);

// Sum-based calibration checksum used by the Wii Remote EEPROM and Nunchuk.
void Checksum(uint8_t* data, size_t length, size_t checksumBytes) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length - checksumBytes; ++i) sum = static_cast<uint8_t>(sum + data[i]);
    data[length - checksumBytes] = static_cast<uint8_t>(sum + 0x55);
    if (checksumBytes > 1) data[length - checksumBytes + 1] = static_cast<uint8_t>(sum + 0xAA);
}

class VirtualWiimote {
public:
    VirtualWiimote() {
        AimInput::Install();
        ResetState();
    }

    void ResetState() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settingsLoaded = false;
        RefreshSettings();
        m_scheme = m_settings.scheme;
        m_arbiter = InputMap::Arbiter{};
        m_classicFilter = DualAnalog::StickFilter{};
        m_prevRecenter = false;
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
        DualAnalog::Recenter(m_pointer, m_config);
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
    // Everything one poll of the controls produces. Sticks use the SDL convention (+y down).
    struct Input {
        uint16_t buttons = 0;          // Wii Remote core buttons (pointer scheme)
        bool c = false, z = false;     // Nunchuk buttons (pointer scheme)
        uint8_t sx = 0x80, sy = 0x80;  // Nunchuk stick (pointer scheme)
        uint8_t cl4 = 0, cl5 = 0;      // Classic Controller pressed masks (classic scheme)
        float lt = 0, rt = 0;          // Classic analog L / R
        float lx = 0, ly = 0;          // move stick after deadzone
        float rx = 0, ry = 0;          // aim stick, raw
        bool precision = false, recenter = false;
        bool blocked = false, scripted = false;
        bool absPtr = false;  // scripted absolute pointer (screen fractions)
        float au = 0.5f, av = 0.5f;
    };

    void RefreshSettings() {
        const uint32_t generation = AimSettings::Generation();
        if (m_settingsLoaded && generation == m_settingsGeneration) return;
        m_settings = AimSettings::Get();
        m_config = m_settings.aim;
        m_settingsGeneration = generation;
        m_settingsLoaded = true;
    }

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
        if (m_scheme == AimSettings::Scheme::Classic) {
            // Classic Controller calibration (8-bit scale: 6-bit left stick << 2, 5-bit right stick << 3).
            // UNVERIFIED against the game's WPAD/KPAD calibration handling.
            uint8_t classic[16] = {0xFC, 0x04, 0x80,  // left X max/min/center
                                   0xFC, 0x04, 0x80,  // left Y
                                   0xF8, 0x08, 0x80,  // right X
                                   0xF8, 0x08, 0x80,  // right Y
                                   0x00, 0x00, 0x00, 0x00};
            Checksum(classic, sizeof classic, 2);
            std::memcpy(&m_ext[0x20], classic, sizeof classic);
            std::memcpy(&m_ext[0x30], classic, sizeof classic);
            static const uint8_t id[6] = {0x00, 0x00, 0xA4, 0x20, 0x01, 0x01};
            std::memcpy(&m_ext[0xFA], id, sizeof id);
            return;
        }
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
        bool absPtr = false;  // third field written as @u/v: pointer placed at screen fractions u,v
        float au = 0.5f, av = 0.5f;
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
                if (parts.size() > 2 && !parts[2].empty() && parts[2][0] == '@') {
                    pair(parts[2].substr(1), s.au, s.av);
                    s.absPtr = true;
                } else if (parts.size() > 2) {
                    pair(parts[2], s.rx, s.ry);
                }
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
                in.lx = s.lx;
                in.ly = -s.ly;
                in.rx = s.rx;
                in.ry = s.ry;
                in.scripted = true;
                in.absPtr = s.absPtr;
                in.au = s.au;
                in.av = s.av;
                const auto cl = [&](uint16_t wii, uint8_t& mask, uint8_t bit) { if (s.buttons & wii) mask |= bit; };
                cl(kBtnA, in.cl5, kClA);
                cl(kBtnB, in.cl5, kClB);
                cl(kBtnOne, in.cl5, kClX);
                cl(kBtnTwo, in.cl5, kClY);
                cl(kBtnUp, in.cl5, kClDUp);
                cl(kBtnLeft, in.cl5, kClDLeft);
                cl(kBtnDown, in.cl4, kClDDown);
                cl(kBtnRight, in.cl4, kClDRight);
                cl(kBtnPlus, in.cl4, kClPlus);
                cl(kBtnMinus, in.cl4, kClMinus);
                cl(kBtnHome, in.cl4, kClHome);
                if (s.c) in.cl4 |= kClL;
                if (s.z) in.cl5 |= kClZL;
                return true;
            }
            acc += s.seconds;
        }
        in.scripted = true;
        return true; // script finished: everything released
    }

    // Bindings (gamepad, keyboard, mouse) -> remote state. Called with the mutex held; side-effect free.
    Input ReadInput() {
        Input in = ReadInputBase();
        if (InjectActive()) {
            in.buttons |= kBtnB;
            in.cl5 |= kClB;
        }
        return in;
    }

    Input ReadInputBase() {
        using AimSettings::Action;
        Input in;
        if (ScriptedInput(in)) return in;
        if (InputBindings::InputBlocked() || !AimInput::Focused()) {
            in.blocked = true;
            return in;
        }
        RefreshSettings();
        const InputMap::RawSample raw = AimInput::Sample();
        auto val = [&](Action a) {
            const size_t i = static_cast<size_t>(a);
            return std::max(InputMap::SlotsValue(m_settings.bindings.pad[i], raw),
                            InputMap::SlotsValue(m_settings.bindings.kbm[i], raw));
        };
        auto down = [&](Action a) { return val(a) >= 0.5f; };
        auto vec = [&](Action left, Action right, Action up, Action downA, float& x, float& y) {
            x = val(right) - val(left);
            y = val(downA) - val(up);
            const float m = std::sqrt(x * x + y * y);
            if (m > 1.0f) {
                x /= m;
                y /= m;
            }
        };
        const bool fire = down(Action::Fire), lock = down(Action::LockOn), jump = down(Action::Jump),
                   evade = down(Action::Evade), confirm = down(Action::Confirm), cancel = down(Action::Cancel),
                   pause = down(Action::Pause), minus = down(Action::Minus), home = down(Action::Home),
                   bx = down(Action::ButtonX), by = down(Action::ButtonY), du = down(Action::DpadUp),
                   dd = down(Action::DpadDown), dl = down(Action::DpadLeft), dr = down(Action::DpadRight);
        float mx = 0, my = 0;
        vec(Action::MoveLeft, Action::MoveRight, Action::MoveUp, Action::MoveDown, mx, my);
        DualAnalog::ApplyDeadzone(mx, my, m_config.leftDeadzone);
        in.lx = mx;
        in.ly = my;
        vec(Action::AimLeft, Action::AimRight, Action::AimUp, Action::AimDown, in.rx, in.ry);
        in.precision = down(Action::Precision);
        in.recenter = down(Action::Recenter);
        if (m_scheme == AimSettings::Scheme::Pointer) {
            auto set = [&](bool on, uint16_t bit) { if (on) in.buttons |= bit; };
            set(fire || cancel, kBtnB);
            set(lock || confirm, kBtnA);
            set(pause, kBtnPlus);
            set(minus, kBtnMinus);
            set(home, kBtnHome);
            set(bx, kBtnOne);
            set(by, kBtnTwo);
            set(du, kBtnUp);
            set(dd, kBtnDown);
            set(dl, kBtnLeft);
            set(dr, kBtnRight);
            in.c = jump;
            in.z = evade;
            const DualAnalog::NunchukState nun = DualAnalog::NunchukFromStick(mx, -my, 0.0f);
            in.sx = nun.x;
            in.sy = nun.y;
        } else {
            auto set = [&](bool on, uint8_t& mask, uint8_t bit) { if (on) mask |= bit; };
            set(jump, in.cl4, kClL);
            set(fire, in.cl4, kClR);
            set(evade, in.cl5, kClZL);
            set(lock, in.cl5, kClZR);
            set(confirm, in.cl5, kClA);
            set(cancel, in.cl5, kClB);
            set(bx, in.cl5, kClX);
            set(by, in.cl5, kClY);
            set(pause, in.cl4, kClPlus);
            set(minus, in.cl4, kClMinus);
            set(home, in.cl4, kClHome);
            set(du, in.cl5, kClDUp);
            set(dl, in.cl5, kClDLeft);
            set(dd, in.cl4, kClDDown);
            set(dr, in.cl4, kClDRight);
            in.lt = val(Action::Jump);
            in.rt = val(Action::Fire);
        }
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

    void BuildExtension(uint8_t* out, const Input& in, float aimX, float aimYDown) {
        if (m_scheme == AimSettings::Scheme::Classic) {
            // 6-byte Classic Controller report: 6-bit left stick, 5-bit right stick and analog L/R.
            const uint8_t lx = DualAnalog::QuantizeStick(in.lx, 32, 31);
            const uint8_t ly = DualAnalog::QuantizeStick(-in.ly, 32, 31);
            const uint8_t rx = DualAnalog::QuantizeStick(aimX, 16, 15);
            const uint8_t ry = DualAnalog::QuantizeStick(-aimYDown, 16, 15);
            const uint8_t lt = static_cast<uint8_t>(std::lround(std::clamp(in.lt, 0.f, 1.f) * 31.f));
            const uint8_t rt = static_cast<uint8_t>(std::lround(std::clamp(in.rt, 0.f, 1.f) * 31.f));
            out[0] = static_cast<uint8_t>(((rx >> 3) & 3) << 6 | (lx & 0x3F));
            out[1] = static_cast<uint8_t>(((rx >> 1) & 3) << 6 | (ly & 0x3F));
            out[2] = static_cast<uint8_t>(((rx & 1) << 7) | (((lt >> 3) & 3) << 5) | (ry & 0x1F));
            out[3] = static_cast<uint8_t>(((lt & 7) << 5) | (rt & 0x1F));
            out[4] = static_cast<uint8_t>(~in.cl4);
            out[5] = static_cast<uint8_t>(~in.cl5);
            return;
        }
        out[0] = in.sx;
        out[1] = in.sy;
        out[2] = 0x80; // accel X
        out[3] = 0x80; // accel Y
        out[4] = 0xB3; // accel Z (one g while held level)
        out[5] = static_cast<uint8_t>(0x03 & ~((in.z ? 1 : 0) | (in.c ? 2 : 0)));
    }

    // Decides which device aims and updates the pointer (pointer scheme) or shapes the Classic right stick.
    // Called once per data report: it consumes the accumulated mouse motion.
    void UpdateAim(const Input& in, float dt, float& classicX, float& classicY) {
        classicX = classicY = 0.f;
        float mdx = 0.f, mdy = 0.f;
        AimInput::ConsumeMouseDelta(mdx, mdy);
        if (in.blocked) {
            m_arbiter.prevStick = m_arbiter.prevMouse = false;
            m_classicFilter = DualAnalog::StickFilter{};
            return;
        }
        if (in.scripted && in.absPtr && m_scheme != AimSettings::Scheme::Classic) {
            DualAnalog::NormalizedToCamera(m_config, in.au, in.av, 1.0f, 1.0f, m_pointer.x, m_pointer.y);
            return;
        }
        const AimSettings::MouseConfig& mouse = m_settings.mouse;
        const bool mouseOn = mouse.enabled && !in.scripted;
        const bool mouseMoving = mouseOn && (std::fabs(mdx) + std::fabs(mdy)) > 0.f;
        const bool stickMoving = DualAnalog::StickActive(m_config, in.rx, in.ry);
        const InputMap::AimSource source = in.scripted ? InputMap::AimSource::Stick
                                                       : InputMap::Arbitrate(m_arbiter, stickMoving, mouseMoving);
        const bool byMouse = mouseOn && source == InputMap::AimSource::Mouse;
        if (m_scheme == AimSettings::Scheme::Classic) {
            float sx = in.rx, sy = in.ry;
            if (byMouse) DualAnalog::MouseDeltaToStick(mdx, mdy, dt, mouse.stickSpeed, sx, sy);
            DualAnalog::ClassicStick(m_config, m_classicFilter, sx, sy, dt, in.precision, classicX, classicY);
            return;
        }
        if (in.recenter && !m_prevRecenter) DualAnalog::Recenter(m_pointer, m_config);
        m_prevRecenter = in.recenter;
        if (byMouse && mouse.raw) {
            if (mouseMoving) DualAnalog::MouseRelativeStep(m_pointer, m_config, mdx, mdy, mouse.sensitivity);
        } else if (byMouse) {
            const AimInput::MouseSample ms = AimInput::MouseNow();
            if (ms.valid) {
                const bool letterbox = RuntimeConfigFile::ForceAspectEnabled();
                const float aspect = RuntimeConfigFile::WidescreenEnabled() ? 16.0f / 9.0f : 4.0f / 3.0f;
                const DualAnalog::Rect rect = DualAnalog::ContentRect(ms.winW, ms.winH, letterbox, aspect);
                float u = 0.5f, v = 0.5f, tx = m_pointer.x, ty = m_pointer.y;
                DualAnalog::WindowToNormalized(rect, ms.x, ms.y, u, v);
                DualAnalog::NormalizedToCamera(m_config, u, v, mouse.rangeX, mouse.rangeY, tx, ty);
                m_pointer.x = DualAnalog::SmoothToward(m_pointer.x, tx, dt, mouse.smoothingMs);
                m_pointer.y = DualAnalog::SmoothToward(m_pointer.y, ty, dt, mouse.smoothingMs);
            }
        } else {
            // Stick right moves the pointer right, which moves the sensor bar left in the camera.
            DualAnalog::Advance(m_pointer, m_config, -in.rx, in.ry, dt, in.precision);
        }
    }

    void BuildDataReport(Report& out) {
        const Input in = ReadInput();
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - m_pointerStamp).count();
        m_pointerStamp = now;
        float classicX = 0.f, classicY = 0.f;
        UpdateAim(in, dt, classicX, classicY);
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
        BuildExtension(ext, in, classicX, classicY);
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
    AimSettings::Settings m_settings;
    uint32_t m_settingsGeneration = 0;
    bool m_settingsLoaded = false;
    AimSettings::Scheme m_scheme = AimSettings::Scheme::Classic;
    InputMap::Arbiter m_arbiter;
    DualAnalog::StickFilter m_classicFilter;
    bool m_prevRecenter = false;
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

void InjectCancel(int millis) {
    g_injectCancelUntilMs.store(NowMs() + (millis > 0 ? millis : 0), std::memory_order_relaxed);
}

} // namespace VirtualRemote
