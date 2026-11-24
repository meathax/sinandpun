#include "virtual_remote.h"
#include "bluetooth/bt_slot_owner.h"

#include "aim_input.h"
#include "aim_settings.h"
#include "remote_action_buttons.h"
#include "dual_analog.h"
#include "input_bindings.h"
#include "input_map.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wiimote_ext_crypto.h"
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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <vector>
#include "trailer_bot.h"

void AimTelemetryStart();  // hle/aim_telemetry.cpp
namespace VirtualPad { void Start(); }  // wiimote/virtual_pad.cpp

namespace VirtualRemote {
namespace {

using Clock = std::chrono::steady_clock;
using Report = std::vector<uint8_t>;

std::mutex g_dbgMutex;
AimDebug g_dbg;

// SetScheme(): requested extension (-1 = none requested yet; otherwise an InputMap::Scheme value).
std::atomic<int> g_requestedScheme{-1};

std::atomic_bool g_menuNavigation{true};
std::atomic_bool g_partnerControllable{false};

const char* SchemeLabel(AimSettings::Scheme s) { return s == AimSettings::Scheme::Classic ? "classic" : "pointer"; }

// The extension needs time between unplug and plug for the guest's WPAD to see both status reports.
constexpr auto kUnplugHold = std::chrono::milliseconds(500);

// In classic scheme the menu-type keys are also pressed on the Wii Remote core buttons (SIN_VREMOTE_CLASSIC_CORE=0
// turns that off).
bool ClassicCoreMirror() {
    static const bool on = [] {
        const char* v = std::getenv("SIN_VREMOTE_CLASSIC_CORE");
        return !(v && *v == '0');
    }();
    return on;
}

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
    explicit VirtualWiimote(unsigned port) : m_port(port) {
        if (port == 0) {
            AimInput::Install();
            AimTelemetryStart();
            VirtualPad::Start();
        }
        ResetState();
    }

    void ResetState() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settingsLoaded = false;
        RefreshSettings();
        const int requested = RequestedScheme();
        m_scheme = requested >= 0 ? static_cast<AimSettings::Scheme>(requested) : m_settings.scheme;
        m_extPresent = true;
        m_swap = Swap::Idle;
        m_extEncrypted = false;
        m_extKey.fill(0);
        m_extTables.reset();
        m_scriptSeg = -1;
        m_arbiter = InputMap::Arbiter{};
        m_classicFilter = DualAnalog::StickFilter{};
        m_prevRecenter = false;
        m_heldCore = m_heldClassic4 = m_heldClassic5 = 0;
        m_heldJump = m_heldEvade = false;
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
        BuildExtensionRegisters();
        m_pointer = DualAnalog::PointerState{};
        DualAnalog::Recenter(m_pointer, m_stickConfig);
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
        PollScheme();
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
        bool recenter = false;
        bool blocked = false, scripted = false;
        bool padMenu = false;
        bool absPtr = false;  // scripted absolute pointer (screen fractions)
        float au = 0.5f, av = 0.5f;
    };

    void RefreshSettings() {
        const uint32_t generation = AimSettings::Generation();
        if (m_settingsLoaded && generation == m_settingsGeneration) return;
        m_settings = AimSettings::Get();
        m_config = m_settings.aim;
        m_stickConfig = DualAnalog::StickPointerConfig(m_config, RuntimeConfigFile::WiiSensorBarAbove());
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
        r[3] = static_cast<uint8_t>((m_leds << 4) | (m_extPresent ? 0x02 : 0) | (m_speakerEnabled ? 0x04 : 0) |
                                    (m_irEnabled ? 0x08 : 0));
        r[6] = 0xB0; // battery
        static int s_budget = 60;
        if (RtLogBudget(s_budget)) {
            RT_LOGF(RT_TAG_BT, "status 0x20 -> guest: extension=%d (%s) leds=%X\n", m_extPresent ? 1 : 0,
                    m_extPresent ? SchemeLabel(m_scheme) : "none", m_leds);
        }
        m_pending.push_back(std::move(r));
    }

    // ---- Extension hot-swap -----------------------------------------------------------------------
    // Called with the mutex held from Pop(). Idle -> (request differs) unplug + status -> hold for kUnplugHold
    // once the status report was handed to the guest -> plug the requested extension + status -> Idle.
    void PollScheme() {
        const int requested = RequestedScheme();
        if (m_swap == Swap::Idle) {
            if (requested < 0 || static_cast<AimSettings::Scheme>(requested) == m_scheme) return;
            m_swapTarget = static_cast<AimSettings::Scheme>(requested);
            RT_LOGF(RT_TAG_BT, "scheme -> %s (unplug %s)\n", SchemeLabel(m_swapTarget), SchemeLabel(m_scheme));
            m_extPresent = false;
            m_extEncrypted = false;
            m_extKey.fill(0);
            m_extTables.reset();
            m_ext.fill(0);
            QueueStatus();
            m_swap = Swap::Detached;
            m_swapAt = Clock::now();
            return;
        }
        if (m_swap == Swap::Detached) {
            if (!m_pending.empty() || Clock::now() - m_swapAt < kUnplugHold) return;
            // Latest request wins; the guest still sees a full unplug/plug cycle.
            if (requested >= 0) m_swapTarget = static_cast<AimSettings::Scheme>(requested);
            m_scheme = m_swapTarget;
            m_classicFilter = DualAnalog::StickFilter{};
            BuildExtensionRegisters();
            m_extPresent = true;
            RT_LOGF(RT_TAG_BT, "scheme -> %s (plug)\n", SchemeLabel(m_scheme));
            QueueStatus();
            m_swap = Swap::Idle;
        }
    }

    void BuildEeprom() {
        // Accelerometer calibration: zero 0x80, one g 0x9A, checksummed; stored twice.
        uint8_t accel[10] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0x00, 0x00};
        Checksum(accel, sizeof accel, 1);
        std::memcpy(&m_eeprom[0x16], accel, sizeof accel);
        std::memcpy(&m_eeprom[0x20], accel, sizeof accel);
    }

    // Identifier + calibration of the attached extension (register block 0xA40000).
    void BuildExtensionRegisters() {
        m_ext.fill(0);
        if (m_scheme == AimSettings::Scheme::Classic) {
            // Classic Controller calibration, 8-bit precision for every axis (max, min, centre; wiibrew and
            // Dolphin Classic.cpp): 6-bit left stick << 2, 5-bit right stick << 3, trigger neutrals 0.
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
            if (!m_extPresent) return nullptr;
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
                if ((address & 0xFF0000) == kRegExtension) ObserveExtensionWrite(offset, r + 6, length);
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

    // Follows the guest's init conversation with the extension: 0x55 at 0xF0 (plus 0x00 at 0xFB) turns encryption
    // off, 0xAA at 0xF0 turns it on, and the 16-byte key at 0x40..0x4F (three writes: 6+6+4) selects the tables.
    void ObserveExtensionWrite(uint32_t offset, const uint8_t* data, size_t length) {
        static int s_budget = 60;
        if (RtLogBudget(s_budget)) {
            RT_LOGF(RT_TAG_BT, "ext write reg=0x%02X len=%u first=%02X\n", static_cast<unsigned>(offset & 0xFF),
                    static_cast<unsigned>(length), length ? data[0] : 0);
        }
        bool keyChanged = false;
        for (size_t i = 0; i < length; ++i) {
            const uint32_t reg = (offset + static_cast<uint32_t>(i)) & 0xFF;
            const uint8_t value = data[i];
            if (reg >= 0x40 && reg < 0x50) {
                m_extKey[reg - 0x40] = value;
                keyChanged = true;
            } else if (reg == 0xF0) {
                if (value == 0xAA) {
                    m_extEncrypted = true;
                } else if (value == 0x55) {
                    m_extEncrypted = false;
                    m_extKey.fill(0);
                    m_extTables.reset();
                }
            }
        }
        if (keyChanged) {
            m_extTables = Wiimote::ExtCrypto::FromKey(m_extKey);
            if (m_extTables) {
                RT_LOGF(RT_TAG_BT, "ext crypto: key accepted, encryption %s\n", m_extEncrypted ? "ON" : "pending");
            }
        }
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
            if (registers && !error && (address & 0xFF0000) == kRegExtension && m_extEncrypted && m_extTables) {
                // The extension encrypts every register it returns once the guest enabled encryption (0xAA at
                // 0xA400F0); the guest decrypts calibration with the same key schedule.
                m_extTables->Encrypt(&reply[6], address & 0xFF, chunk);
            }
            if (registers && (address & 0xFF0000) == kRegExtension) {
                static int s_budget = 60;
                if (RtLogBudget(s_budget)) {
                    RT_LOGF(RT_TAG_BT, "ext read reg=0x%02X len=%u err=%u enc=%d\n",
                            static_cast<unsigned>(address & 0xFF), static_cast<unsigned>(chunk), error,
                            (m_extEncrypted && m_extTables) ? 1 : 0);
                }
            }
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
        int scheme = -1;       // scheme=classic|pointer: hot-swap the extension when this segment starts
        bool noCore = false;   // core=0: in classic scheme the Wii Remote core buttons stay released
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

    template<bool Partner>
    const std::vector<ScriptSegment>& Script(bool& loop) {
        static bool s_loop = false;
        // SIN_VREMOTE_SCRIPT_FILE=path: the script text is re-read whenever the file changes (QA harness drives
        // phases: menu navigation, then combat) and the script clock restarts.
        auto parse = [](const std::string& envText) {
            std::vector<ScriptSegment> out;
            int pendingScheme = -1;
            s_loop = false;
            const char* env = envText.c_str();
            if (!*env) return out;
            // '|' is accepted for ';' and '/' for ',' so the script survives shells and env lists that split on those.
            std::string text(env);
            for (char& ch : text) {
                if (ch == '|') ch = ';';
                else if (ch == '/') ch = ',';
            }
            for (const std::string& seg : Split(text, ';')) {
                if (seg == "loop") { s_loop = true; continue; }
                if (seg.empty()) continue;
                // A bare "scheme=classic" segment applies to the next timed segment.
                if (seg.rfind("scheme=", 0) == 0) {
                    pendingScheme = seg.compare(7, std::string::npos, "classic") == 0 ? 1 : 0;
                    continue;
                }
                std::vector<std::string> parts = Split(seg, ':');
                ScriptSegment s;
                s.scheme = pendingScheme;
                pendingScheme = -1;
                for (size_t i = 1; i < parts.size(); ++i) {
                    if (parts[i].rfind("scheme=", 0) == 0) {
                        s.scheme = parts[i].compare(7, std::string::npos, "classic") == 0 ? 1 : 0;
                        parts[i].clear();
                    } else if (parts[i] == "core=0") {
                        s.noCore = true;
                        parts[i].clear();
                    }
                }
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
        };
        static std::vector<ScriptSegment> segments;
        static bool s_inited = false;
        static std::filesystem::file_time_type s_stamp{};
        static auto s_lastCheck = Clock::time_point{};
        const char* scriptFile = std::getenv(Partner ? "SIN_P2_SCRIPT_FILE" : "SIN_VREMOTE_SCRIPT_FILE");
        if (!s_inited) {
            s_inited = true;
            const char* e = std::getenv(Partner ? "SIN_P2_SCRIPT" : "SIN_VREMOTE_SCRIPT");
            segments = parse(e ? e : "");
        }
        if (scriptFile && *scriptFile && Clock::now() - s_lastCheck > std::chrono::milliseconds(500)) {
            s_lastCheck = Clock::now();
            std::error_code ec;
            const auto stamp = std::filesystem::last_write_time(scriptFile, ec);
            if (!ec && stamp != s_stamp) {
                s_stamp = stamp;
                std::ifstream in(scriptFile);
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
                segments = parse(text);
                m_scriptReloaded = true;
            }
        }
        loop = s_loop;
        return segments;
    }

    // Returns true and fills `in` when a script is active.
    bool ScriptedInput(Input& in) {
        if (TrailerBot::Output bot; TrailerBot::Sample(m_port, bot)) {
            in.buttons = static_cast<uint16_t>((bot.a ? kBtnA : 0) | (bot.b ? kBtnB : 0) | (bot.plus ? kBtnPlus : 0));
            in.c = bot.c;
            in.z = bot.z;
            const DualAnalog::NunchukState nun = DualAnalog::NunchukFromStick(bot.lx, bot.ly);
            in.sx = nun.x;
            in.sy = nun.y;
            in.lx = bot.lx;
            in.ly = -bot.ly;
            in.scripted = true;
            in.absPtr = bot.pointer;
            in.au = bot.u;
            in.av = bot.v;
            if (bot.a) in.cl5 |= kClA;
            if (bot.b) in.cl5 |= kClB;
            if (bot.c) in.cl4 |= kClL;
            if (bot.z) in.cl5 |= kClZL;
            return true;
        }
        bool loop = false;
        const auto& segments = m_port ? Script<true>(loop) : Script<false>(loop);
        if (m_scriptReloaded) {
            m_scriptReloaded = false;
            m_scriptStart = Clock::now();
            m_scriptSeg = -1;
        }
        if (segments.empty()) return false;
        float total = 0;
        for (const auto& s : segments) total += s.seconds;
        float at = std::chrono::duration<float>(Clock::now() - m_scriptStart).count();
        if (loop && total > 0) at = std::fmod(at, total);
        float acc = 0;
        int index = 0;
        for (const auto& s : segments) {
            if (at < acc + s.seconds) {
                if (index != m_scriptSeg) {
                    m_scriptSeg = index;
                    if (m_port == 0 && s.scheme >= 0) {
                        RT_LOGF(RT_TAG_BT, "script t=%.1fs segment %d: scheme=%s\n", at, index,
                                s.scheme == 1 ? "classic" : "pointer");
                        g_requestedScheme.store(s.scheme == 1 ? static_cast<int>(AimSettings::Scheme::Classic)
                                                              : static_cast<int>(AimSettings::Scheme::Pointer),
                                                std::memory_order_relaxed);
                    }
                }
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
                if (s.noCore && m_scheme == AimSettings::Scheme::Classic) in.buttons = 0;
                return true;
            }
            acc += s.seconds;
            ++index;
        }
        in.scripted = true;
        return true; // script finished: everything released
    }

    // Bindings (gamepad, keyboard, mouse) -> remote state. Called with the mutex held; side-effect free.
    Input ReadInput() {
        Input in = ReadInputBase();
        if (m_port == 1 && RuntimeConfigFile::EnhancedCoopAtLaunch()) {
            if (!g_partnerControllable.load(std::memory_order_relaxed)) {
                m_heldCore = in.buttons;
                m_heldClassic4 = in.cl4;
                m_heldClassic5 = in.cl5;
                m_heldJump = in.c;
                m_heldEvade = in.z;
                Input neutral;
                neutral.blocked = true;
                return neutral;
            }
            m_heldCore &= in.buttons;
            m_heldClassic4 &= in.cl4;
            m_heldClassic5 &= in.cl5;
            m_heldJump &= in.c;
            m_heldEvade &= in.z;
            in.buttons &= ~m_heldCore;
            in.cl4 &= ~m_heldClassic4;
            in.cl5 &= ~m_heldClassic5;
            in.c &= !m_heldJump;
            in.z &= !m_heldEvade;
        }
        if (m_port == 0 && InjectActive()) {
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
        const InputMap::RawSample raw = AimInput::Sample(m_port);
        if (g_menuNavigation.load(std::memory_order_relaxed) &&
            AimInput::LastDevice() == InputMap::ActiveDevice::Gamepad) {
            in.padMenu = true;
            const auto button = [&](SDL_GamepadButton b) { return raw.padButtons[b]; };
            auto menuButton = [&](bool down, uint16_t core, uint8_t cl4, uint8_t cl5) {
                if (down) { in.buttons |= core; in.cl4 |= cl4; in.cl5 |= cl5; }
            };
            menuButton(button(SDL_GAMEPAD_BUTTON_SOUTH), kBtnA, 0, kClA);
            menuButton(button(SDL_GAMEPAD_BUTTON_EAST), kBtnB, 0, kClB);
            menuButton(button(SDL_GAMEPAD_BUTTON_START), kBtnPlus, kClPlus, 0);
            menuButton(button(SDL_GAMEPAD_BUTTON_BACK), kBtnMinus, kClMinus, 0);
            const float x = raw.padAxes[SDL_GAMEPAD_AXIS_LEFTX];
            const float y = raw.padAxes[SDL_GAMEPAD_AXIS_LEFTY];
            menuButton(button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -0.55f, kBtnLeft, 0, kClDLeft);
            menuButton(button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 0.55f, kBtnRight, kClDRight, 0);
            menuButton(button(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -0.55f, kBtnUp, 0, kClDUp);
            menuButton(button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 0.55f, kBtnDown, kClDDown, 0);
            return in;
        }
        auto val = [&](Action a) {
            return AimSettings::ActionValue(m_settings,a,raw);
        };
        auto vec = [&](Action left, Action right, Action up, Action downA, float& x, float& y) {
            x = val(right) - val(left);
            y = val(downA) - val(up);
            const float m = std::sqrt(x * x + y * y);
            if (m > 1.0f) {
                x /= m;
                y /= m;
            }
        };
        const auto buttons = RemoteActions::Resolve(m_settings,raw,m_scheme,ClassicCoreMirror());
        float mx = 0, my = 0;
        vec(Action::MoveLeft, Action::MoveRight, Action::MoveUp, Action::MoveDown, mx, my);
        DualAnalog::ApplyDeadzone(mx, my, m_config.leftDeadzone);
        in.lx = mx;
        in.ly = my;
        vec(Action::AimLeft, Action::AimRight, Action::AimUp, Action::AimDown, in.rx, in.ry);
        in.recenter = buttons.recenter;
        in.buttons = buttons.core;
        in.cl4 = buttons.classic4;
        in.cl5 = buttons.classic5;
        in.c = buttons.c;
        in.z = buttons.z;
        in.lt = buttons.lt;
        in.rt = buttons.rt;
        if (m_scheme == AimSettings::Scheme::Pointer) {
            const DualAnalog::NunchukState nun = DualAnalog::NunchukFromStick(mx, -my, 0.0f);
            in.sx = nun.x;
            in.sy = nun.y;
        }
        return in;
    }

    void BuildIr(uint8_t* out, bool extended, bool padMenu) {
        if (padMenu) {
            std::memset(out, 0xff, extended ? 12 : 10);
            return;
        }
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
        if (OwnsMouse()) AimInput::ConsumeMouseDelta(mdx, mdy);
        float gdx = 0.f, gdy = 0.f;
        AimInput::ConsumeGyroDelta(gdx, gdy, m_port);
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
        const bool mouseOn = OwnsMouse() && mouse.enabled && !in.scripted;
        const bool mouseMoving = mouseOn && (std::fabs(mdx) + std::fabs(mdy)) > 0.f;
        const bool stickMoving = DualAnalog::StickActive(m_config, in.rx, in.ry);
        const InputMap::AimSource source = in.scripted ? InputMap::AimSource::Stick
                                                       : InputMap::Arbitrate(m_arbiter, stickMoving, mouseMoving);
        const bool byMouse = mouseOn && source == InputMap::AimSource::Mouse;
        if (m_scheme == AimSettings::Scheme::Classic) {
            float sx = in.rx, sy = in.ry;
            if (byMouse) DualAnalog::MouseDeltaToStick(mdx, mdy, dt, mouse.stickSpeed, sx, sy);
            DualAnalog::ClassicStick(m_config, m_classicFilter, sx, sy, dt, classicX, classicY);
            return;
        }
        if (in.recenter && !m_prevRecenter) {
            DualAnalog::Recenter(m_pointer, m_stickConfig);
            gdx = gdy = 0.f;
        }
        m_prevRecenter = in.recenter;
        if (byMouse && mouse.raw) {
            if (mouseMoving) DualAnalog::MouseRelativeStep(m_pointer, m_config, mdx, mdy, mouse.sensitivity);
        } else if (byMouse) {
            const AimInput::MouseSample ms = AimInput::MouseNow();
            if (ms.valid && (mouseMoving || !m_settings.gyro.enabled)) {
                const DualAnalog::Rect rect = AimInput::PictureRect(ms);
                float u = 0.5f, v = 0.5f, tx = m_pointer.x, ty = m_pointer.y;
                DualAnalog::WindowToNormalized(rect, ms.x, ms.y, u, v);
                DualAnalog::NormalizedToCamera(m_config, u, v, mouse.rangeX, mouse.rangeY, tx, ty);
                m_pointer.x = DualAnalog::SmoothToward(m_pointer.x, tx, dt, mouse.smoothingMs);
                m_pointer.y = DualAnalog::SmoothToward(m_pointer.y, ty, dt, mouse.smoothingMs);
            }
        } else {
            // Stick right moves the pointer right, which moves the sensor bar left in the camera.
            DualAnalog::Advance(m_pointer, m_stickConfig, -in.rx, in.ry, dt);
        }
        // Gyro: equal screen-pixel motion per degree on both axes (the reticle layer is 810x456).
        if (m_settings.gyro.enabled && !in.scripted)
            DualAnalog::MouseRelativeStep(m_pointer, m_stickConfig, gdx, gdy * DualAnalog::kScreenAspectY, 1.f);
    }

    void PublishDebug(const Input& in, float classicX, float classicY) {
        AimDebug d;
        d.scheme = static_cast<int>(m_scheme);
        d.scripted = in.scripted;
        d.extPresent = m_extPresent;
        d.rx = in.rx;
        d.ry = in.ry;
        d.lx = in.lx;
        d.ly = in.ly;
        d.camX = m_pointer.x;
        d.camY = m_pointer.y;
        d.classicX = classicX;
        d.classicY = classicY;
        d.lxQ = DualAnalog::QuantizeStick(in.lx, 32, 31);
        d.lyQ = DualAnalog::QuantizeStick(-in.ly, 32, 31);
        d.rxQ = DualAnalog::QuantizeStick(classicX, 16, 15);
        d.ryQ = DualAnalog::QuantizeStick(-classicY, 16, 15);
        d.nunX = in.sx;
        d.nunY = in.sy;
        d.nunC = in.c;
        d.nunZ = in.z;
        d.buttons = in.buttons;
        d.cl4 = in.cl4;
        d.cl5 = in.cl5;
        d.lt = in.lt;
        d.rt = in.rt;
        std::lock_guard<std::mutex> lock(g_dbgMutex);
        d.serial = g_dbg.serial + 1;
        g_dbg = d;
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

        if (m_port == 0) PublishDebug(in, classicX, classicY);

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
        if (m_extPresent) {
            BuildExtension(ext, in, classicX, classicY);
            if (m_extEncrypted) {
                // Data reports are encrypted with the table offset restarting at 0 (Dolphin / real extension).
                if (m_extTables) {
                    m_extTables->Encrypt(ext, 0, 6);
                } else {
                    static int s_warn = 3;
                    if (RtLogBudget(s_warn)) {
                        RT_LOGF(RT_TAG_BT, "ext crypto: encryption enabled but the guest key was not accepted; "
                                           "sending plain data\n");
                    }
                }
            }
        }
        switch (m_mode) {
        case 0x31: accel(3); break;
        case 0x32: std::memcpy(&out[3], ext, 8); break;
        case 0x33: accel(3); BuildIr(&out[6], true, in.padMenu); break;
        case 0x35: accel(3); std::memcpy(&out[6], ext, 16); break;
        case 0x36: BuildIr(&out[3], false, in.padMenu); std::memcpy(&out[13], ext, 9); break;
        case 0x37: accel(3); BuildIr(&out[6], false, in.padMenu); std::memcpy(&out[16], ext, 6); break;
        case 0x34: std::memcpy(&out[3], ext, 19); break;
        case 0x3D: std::memcpy(&out[1], ext, 21); break;
        default: break;
        }
    }

    const unsigned m_port;
    bool OwnsMouse() const { return m_port == (RuntimeConfigFile::SecondRemoteAtLaunch() == 2 ? 1u : 0u); }
    uint16_t m_heldCore = 0;
    uint8_t m_heldClassic4 = 0, m_heldClassic5 = 0;
    bool m_heldJump = false, m_heldEvade = false;
    int RequestedScheme() const {
        if (m_port == 0 || RuntimeConfigFile::EnhancedCoopAtLaunch()) return g_requestedScheme.load(std::memory_order_relaxed);
        return static_cast<int>(OwnsMouse() ? AimSettings::Scheme::Pointer : AimSettings::Scheme::Classic);
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
    DualAnalog::Config m_stickConfig;
    DualAnalog::PointerState m_pointer;
    AimSettings::Settings m_settings;
    uint32_t m_settingsGeneration = 0;
    bool m_settingsLoaded = false;
    AimSettings::Scheme m_scheme = AimSettings::Scheme::Pointer;  // the extension currently attached
    enum class Swap : uint8_t { Idle, Detached };
    Swap m_swap = Swap::Idle;
    AimSettings::Scheme m_swapTarget = AimSettings::Scheme::Pointer;
    Clock::time_point m_swapAt;
    bool m_extPresent = true;
    bool m_extEncrypted = false;
    std::array<uint8_t, 16> m_extKey{};
    std::optional<Wiimote::ExtCrypto::Tables> m_extTables;
    bool m_scriptReloaded = false;
    int m_scriptSeg = -1;
    InputMap::Arbiter m_arbiter;
    DualAnalog::StickFilter m_classicFilter;
    bool m_prevRecenter = false;
};

class CompositeHost final : public Bluetooth::RemoteHost {
public:
    bool IsAttached(size_t slot) const override {
        // A link established by one backend must not silently continue on the other: the game would keep the
        // extension type, report mode and IR setup it negotiated with the previous device, and the real host
        // would never see its link come up. Report "detached" once so the link drops and the guest reconnects.
        if (const Bluetooth::SlotBackend now = Backend(slot); !m_owner.Matches(slot, now)) {
            static int s_budget = 8;
            if (RtLogBudget(s_budget)) {
                RT_LOG(RT_TAG_HLE) << "Wii Remote " << (slot + 1) << ": device changed to "
                                   << (now == Bluetooth::SlotBackend::Real ? "a real Wii Remote" : "the virtual remote")
                                   << ", reconnecting" << std::endl;
            }
            return false;
        }
        if (UsesVirtualSlot(slot)) {
            static int s_budget = 8;
            if (RtLogBudget(s_budget)) {
                RT_LOG(RT_TAG_BT) << "IsAttached slot=" << slot << " -> true (virtual)" << std::endl;
            }
            return true;
        }
        return Wiimote::Host().IsAttached(slot);
    }

    bool ConsumeWake(size_t slot) override {
        if (UsesVirtualSlot(slot)) {
            const bool woke = m_wake[slot].exchange(false);
            static int s_budget = 8;
            if (RtLogBudget(s_budget)) {
                RT_LOG(RT_TAG_BT) << "ConsumeWake slot=" << slot << " -> " << (woke ? "true" : "false") << std::endl;
            }
            return woke;
        }
        return Wiimote::Host().ConsumeWake(slot);
    }

    bool PopInputReport(size_t slot, std::vector<uint8_t>& report) override {
        if (UsesVirtualSlot(slot)) {
            const bool got = Remote(slot).Pop(report);
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
        if (UsesVirtualSlot(slot)) {
            static int s_budget = 160;
            if (RtLogBudget(s_budget)) {
                RT_LOGF(RT_TAG_BT, "out id=0x%02X size=%u b1..6=%02X %02X %02X %02X %02X %02X\n",
                        size != 0 ? report[0] : 0, static_cast<unsigned>(size),
                        size > 1 ? report[1] : 0, size > 2 ? report[2] : 0, size > 3 ? report[3] : 0,
                        size > 4 ? report[4] : 0, size > 5 ? report[5] : 0, size > 6 ? report[6] : 0);
            }
            Remote(slot).Output(report, size);
            return;
        }
        Wiimote::Host().SendOutputReport(slot, report, size);
    }

    void OnLinkStateChanged(size_t slot, Bluetooth::LinkState state) override {
        const bool virtualSlot = UsesVirtualSlot(slot);
        if (slot < 2 && state == Bluetooth::LinkState::Down) m_wake[slot] = true;
        const Bluetooth::SlotBackend lost = m_owner.OnStateChanged(slot, state, Backend(slot));
        // The backend that owned the link hears it go down, even if the slot has switched meanwhile.
        if (lost == Bluetooth::SlotBackend::Real) {
            Wiimote::Host().OnLinkStateChanged(slot, state);
            return;
        }
        if (lost == Bluetooth::SlotBackend::Virtual) return;
        if (virtualSlot) {
            if (state == Bluetooth::LinkState::Ready) {
                Remote(slot).ResetState();
                RT_LOG(RT_TAG_HLE) << "Virtual Wii Remote + Nunchuk connected to the game (gamepad)" << std::endl;
            }
            return;
        }
        Wiimote::Host().OnLinkStateChanged(slot, state);
    }

    void OnGuestDisconnect(size_t slot) override {
        if (UsesVirtualSlot(slot)) {
            RT_LOG(RT_TAG_HLE) << "Virtual Wii Remote disconnected by the game" << std::endl;
            return;
        }
        Wiimote::Host().OnGuestDisconnect(slot);
    }

    void OnGuestInquiry(bool active) override { Wiimote::Host().OnGuestInquiry(active); }

    const char* DeviceName(size_t slot) const override {
        if (UsesVirtualSlot(slot)) return "Nintendo RVL-CNT-01";
        return Wiimote::Host().DeviceName(slot);
    }

private:
    VirtualWiimote& Remote(size_t slot) {
        if (slot == 0) return m_primary;
        static VirtualWiimote partner(1);
        return partner;
    }

    static Bluetooth::SlotBackend Backend(size_t slot) {
        return UsesVirtualSlot(slot) ? Bluetooth::SlotBackend::Virtual : Bluetooth::SlotBackend::Real;
    }

    VirtualWiimote m_primary{0};
    std::atomic<bool> m_wake[2]{true, true};
    Bluetooth::SlotOwnership m_owner;  // backend that brought each current link up
};

} // namespace

Bluetooth::RemoteHost& Host() {
    static CompositeHost* host = new CompositeHost();
    return *host;
}

bool UsesVirtualSlot(size_t slot) {
    if (slot > 1) return false;
    if (slot == 1) {
        const int source = RuntimeConfigFile::SecondRemoteAtLaunch();
        if (!source || (source == 1 && !PADGetSDLGamepadForPort(1))) return false;
    }
    static const bool enabled = [] {
        const char* v=std::getenv("SIN_VIRTUAL_REMOTE");
        return !(v && *v=='0');
    }();
    return enabled && !Wiimote::Host().IsAttached(slot);
}

bool UsesVirtualInput() { return UsesVirtualSlot(0); }

bool RemoteOwnsPort(size_t slot) {
    if (slot > 1) return false;
    return UsesVirtualSlot(slot) || Wiimote::Host().IsAttached(slot);
}

void SetPartnerControllable(bool controllable) {
    g_partnerControllable.store(controllable, std::memory_order_relaxed);
}

void SetMenuNavigation(bool enabled) { g_menuNavigation.store(enabled, std::memory_order_relaxed); }

void SetScheme(InputMap::Scheme scheme) {
    static const bool scriptOwns = [] {
        const char* env = std::getenv("SIN_VREMOTE_SCRIPT");
        return env && std::strstr(env, "scheme=") != nullptr;
    }();
    if (scriptOwns) return;
    g_requestedScheme.store(static_cast<int>(scheme), std::memory_order_relaxed);
}

AimDebug GetAimDebug() {
    std::lock_guard<std::mutex> lock(g_dbgMutex);
    return g_dbg;
}

InputMap::Scheme GetScheme() {
    const int requested = g_requestedScheme.load(std::memory_order_relaxed);
    return requested >= 0 ? static_cast<InputMap::Scheme>(requested) : AimSettings::Get().scheme;
}

void InjectCancel(int millis) {
    g_injectCancelUntilMs.store(NowMs() + (millis > 0 ? millis : 0), std::memory_order_relaxed);
}

} // namespace VirtualRemote
