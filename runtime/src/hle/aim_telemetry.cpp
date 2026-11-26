// Aim telemetry: SIN_AIM_LOG=1 prints one "[aim] ..." line at 10 Hz with what the GAME received next to what the
// host emitted, so reticle/stick behaviour can be measured instead of guessed.
//
// Guest side (nothing is hooked, a host thread only reads guest memory; evidence: tools/dis_dol.py 0x8002D208):
//   GM = *(0x804B19A0);  input = *(GM + 4)                    (main loop: 0x8003ECEC..F4 -> fn_8002D208(input))
//   fn_8002D208 calls KPADRead (0x8019DE10) per channel and copies the newest KPADStatus to
//     input + 0x490 + chan * 0xB0        hold +0, trig +4, release +8, pos.x +0x20, pos.y +0x24,
//                                        dev_type +0x5C (0 core, 1 Nunchuk, 2 Classic), dpd_valid_fg +0x5E,
//                                        ex_status +0x60: Nunchuk stick x/y +0x60/+0x64; Classic hold +0x60,
//                                        lstick +0x6C/+0x70, rstick +0x74/+0x78, ltrig +0x7C, rtrig +0x80
//   and (0x8002DDEC..) the pointer used by the game's own code: input + 0x48 + chan * 100 (pos.x, pos.y floats).
//   KPAD pos is -1..1 normalised (+x right, +y DOWN is NOT assumed: the log prints it as read).
//
// Host side: VirtualRemote::GetAimDebug() (stick/pointer/scheme the remote last emitted) and AimInput state.
//
// Environment:
//   SIN_AIM_LOG=1              enable (10 Hz by default)
//   SIN_AIM_LOG_HZ=<n>         sample rate
//   SIN_AIM_LOG_FILE=<path>    write the lines to a file instead of stderr
//   SIN_AIM_LOG_RETICLE=0xADDR optional: guest address of two floats (reticle x, y) to add as "ret=" (see docs)
//
// Line format (key=value, space separated, stable for tools/aim_test.ps1):
//   [aim] t=<s> scene=<label> stage=<id> dev=<0/1/2> dpd=<n> kpos=(x,y) gpos=(x,y) nun=(x,y) cl.l=(x,y) cl.r=(x,y)
//         cl.lt=<f> cl.rt=<f> hold=<hex> clhold=<hex> | host scheme=<p|c> script=<0/1> raw.r=(x,y) raw.l=(x,y)
//         cam=(x,y) cls=(x,y) q.r=(a,b) q.l=(a,b) nunq=(a,b) btn=<hex> cl4=<hex> cl5=<hex> n=<reports>

#include "memory.h"
#include "guest_abi/guest_region.h"
#include "runtime_log.h"
#include "virtual_remote.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

const uint32_t kGmPtrAddr = MKW_GADDR(804B19A0);
const uint32_t kLoadedStageAddr = MKW_GADDR(804B1968);
constexpr uint32_t kStatusBase = 0x490u;   // input + this = KPADStatus copy, channel 0
constexpr uint32_t kPointerBase = 0x48u;   // input + this = pointer x,y floats, channel 0
// GunSight (the reticle object; Ng names from projects/r2ve01/MAP.txt): GunSight1P vtable 0x802F9270 (ctor 0x8006934C).
// GunSight::draw (0x80068CF4) translates by (405 * x, 228 * y) around the screen centre of a 810 x 456 layer using
// x = *(sight + 0x44), y = *(sight + 0x48): normalised -1..1, 0 = centre. setPos2D (0x80066138) stores +0x3C/+0x40 and
// copies them to +0x44/+0x48. Byte +0x73 = visible/active, +0x76 = mode.
const uint32_t kSightVptr1P = MKW_GADDR(802F9270);  // R2VJ01: 0x802DFF10
constexpr uint32_t kScanLo = 0x80400000u;
constexpr uint32_t kScanHi = 0x81800000u;

bool Rd32(uint32_t a, uint32_t& v) {
    try {
        return Memory::TryRead32(a, v);
    } catch (...) {
        return false;
    }
}

float AsFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

bool RdF(uint32_t a, float& f) {
    uint32_t v = 0;
    if (!Rd32(a, v)) return false;
    f = AsFloat(v);
    return true;
}

bool Rd8(uint32_t a, uint8_t& v) {
    try {
        v = Memory::Read8(a);
        return true;
    } catch (...) {
        return false;
    }
}

bool Valid(uint32_t p) { return p >= 0x80000000u && p < 0x81800000u; }

uint32_t ScanForSight() {
    for (uint32_t a = kScanLo; a < kScanHi; a += 8) {
        uint32_t v = 0;
        if (Rd32(a, v) && v == kSightVptr1P) return a;
    }
    return 0;
}

void Run() {
    const char* hzEnv = std::getenv("SIN_AIM_LOG_HZ");
    const double hz = hzEnv ? std::atof(hzEnv) : 10.0;
    const auto period = std::chrono::duration<double>(1.0 / (hz > 0.5 ? hz : 10.0));
    FILE* out = stderr;
    if (const char* path = std::getenv("SIN_AIM_LOG_FILE")) {
        if (*path) {
            if (FILE* f = std::fopen(path, "w")) out = f;
        }
    }
    uint32_t reticleAddr = 0;
    if (const char* r = std::getenv("SIN_AIM_LOG_RETICLE")) reticleAddr = static_cast<uint32_t>(std::strtoul(r, nullptr, 0));

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // guest memory is not mapped at first
    auto next = Clock::now();
    uint32_t sight = 0;
    double nextScan = 0.0;
    for (;;) {
        next += std::chrono::duration_cast<Clock::duration>(period);
        std::this_thread::sleep_until(next);
        const double t = std::chrono::duration<double>(Clock::now() - t0).count();

        uint32_t gm = 0, input = 0, scene = 0, l0 = 0, l1 = 0;
        uint8_t stage = 0xFF;
        Rd32(kGmPtrAddr, gm);
        Rd8(kLoadedStageAddr, stage);
        char label[9] = "-";
        if (Valid(gm)) {
            Rd32(gm + 4, input);
            if (Rd32(gm + 96, scene) && Valid(scene)) {
                Rd32(scene + 4, l0);
                Rd32(scene + 8, l1);
                for (int i = 0; i < 4; ++i) {
                    label[i] = static_cast<char>(l0 >> (24 - 8 * i));
                    label[4 + i] = static_cast<char>(l1 >> (24 - 8 * i));
                }
                label[8] = 0;
                for (char& c : label) {
                    if (c && (c < 32 || c > 126)) c = '.';
                }
            }
        }

        char guest[400] = "guest=none";
        if (Valid(input)) {
            const uint32_t s = input + kStatusBase;
            uint32_t hold = 0, clhold = 0;
            uint8_t dev = 0xFF, dpd = 0xFF;
            float kx = 0, ky = 0, gx = 0, gy = 0, nx = 0, ny = 0, llx = 0, lly = 0, rlx = 0, rly = 0, lt = 0, rt = 0;
            Rd32(s, hold);
            Rd8(s + 0x5C, dev);
            Rd8(s + 0x5E, dpd);
            RdF(s + 0x20, kx);
            RdF(s + 0x24, ky);
            RdF(input + kPointerBase, gx);
            RdF(input + kPointerBase + 4, gy);
            RdF(s + 0x60, nx);
            RdF(s + 0x64, ny);
            Rd32(s + 0x60, clhold);
            RdF(s + 0x6C, llx);
            RdF(s + 0x70, lly);
            RdF(s + 0x74, rlx);
            RdF(s + 0x78, rly);
            RdF(s + 0x7C, lt);
            RdF(s + 0x80, rt);
            std::snprintf(guest, sizeof guest,
                          "dev=%u dpd=%u kpos=(%.3f,%.3f) gpos=(%.3f,%.3f) nun=(%.3f,%.3f) cl.l=(%.3f,%.3f) "
                          "cl.r=(%.3f,%.3f) cl.lt=%.2f cl.rt=%.2f hold=%08X clhold=%08X",
                          dev, dpd, kx, ky, gx, gy, nx, ny, llx, lly, rlx, rly, lt, rt, hold, clhold);
        }
        char ret[160] = "";
        if (reticleAddr != 0) {
            float rx = 0, ry = 0;
            RdF(reticleAddr, rx);
            RdF(reticleAddr + 4, ry);
            std::snprintf(ret, sizeof ret, " ret=(%.3f,%.3f)", rx, ry);
        } else {
            uint32_t vp = 0;
            if (sight == 0 || !Rd32(sight, vp) || vp != kSightVptr1P) {
                sight = 0;
                if (t >= nextScan) {
                    sight = ScanForSight();
                    nextScan = t + 1.0;
                    if (sight) std::fprintf(out, "[aim] sight found at %08X (t=%.2f)\n", sight, t);
                }
            }
            if (sight != 0) {
                float tx = 0, ty = 0, sx = 0, sy = 0;
                uint8_t vis = 0, mode = 0, st = 0;
                RdF(sight + 0x3C, tx);
                RdF(sight + 0x40, ty);
                RdF(sight + 0x44, sx);
                RdF(sight + 0x48, sy);
                Rd8(sight + 0x73, vis);
                Rd8(sight + 0x76, mode);
                Rd8(sight + 0x71, st);
                std::snprintf(ret, sizeof ret, " sight=(%.3f,%.3f) sight.tgt=(%.3f,%.3f) sight.vis=%u mode=%u st=%u",
                              sx, sy, tx, ty, vis, mode, st);
            } else {
                std::snprintf(ret, sizeof ret, " sight=none");
            }
        }

        const VirtualRemote::AimDebug d = VirtualRemote::GetAimDebug();
        std::fprintf(out,
                     "[aim] t=%.2f scene=%s stage=%u %s%s | scheme=%c script=%d ext=%d raw.r=(%.3f,%.3f) "
                     "raw.l=(%.3f,%.3f) cam=(%.1f,%.1f) cls=(%.3f,%.3f) q.r=(%u,%u) q.l=(%u,%u) nunq=(%u,%u) "
                     "nunCZ=%d%d btn=%04X cl4=%02X cl5=%02X lt=%.2f rt=%.2f n=%u\n",
                     t, label, stage, guest, ret, d.scheme == 0 ? 'c' : 'p', d.scripted ? 1 : 0, d.extPresent ? 1 : 0,
                     d.rx, d.ry, d.lx, d.ly, d.camX, d.camY, d.classicX, d.classicY, d.rxQ, d.ryQ, d.lxQ, d.lyQ,
                     d.nunX, d.nunY, d.nunC ? 1 : 0, d.nunZ ? 1 : 0, d.buttons, d.cl4, d.cl5, d.lt, d.rt, d.serial);
        std::fflush(out);
    }
}

}  // namespace

// Called once from the virtual remote's constructor (keeps this object file linked from the static library).
void AimTelemetryStart() {
    static std::atomic<bool> started{false};
    const char* v = std::getenv("SIN_AIM_LOG");
    if (v && *v && *v != '0' && !started.exchange(true)) std::thread(Run).detach();
}
