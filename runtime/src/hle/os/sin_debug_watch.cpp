// Debug aid: SIN_DEBUG_WATCH=1 polls guest memory from a host thread and logs "[watch]" lines to stderr.
// Read-only. Nothing in the guest is changed.
//
// Stage 2 (stg02.rso): Submarine00 objects (vtable 0x97EA0F20, eachUpdate 0x97E2A720) and live WaterMines
// (vtable 0x97EA1418). A Submarine00 launches a WaterMine only in ActionMode mode 2 (byte +0x12C), step 1
// (byte +0x12F), when its step counter (+0x138) is a multiple of 60 and while the route distance
// StgGlobals+0x268 lies in (3300, 4500). Step 0 -> 1 happens once the distance exceeds 3000.
// StgGlobals = *(*(GM+0x60)+0x44), GM = *(0x804B19A0); distance += speed(+0x26C) * scale(+0x270) per frame.
//
// Stage 0 (stg00.rso): RigidBoxManager = *(StgGlobals+0x494+0x10). Dynamic boxes: count +0x238, RigidBox* at
// +0x38[i]. Static walls/floor: count +0x23C, RigidBox* at +0x1B8[i]. RigidBox: pos +0x0, half extents +0xC,
// quat +0xCC, mass +0xDC, linear velocity +0x140, angular velocity +0x14C, asleep byte +0x174.
//
//   SIN_DEBUG_WATCH=1          enable
//   SIN_DEBUG_WATCH_HZ=n       poll rate (default 20)

#include "memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kGmPtrAddr = 0x804B19A0u;
constexpr uint32_t kLoadedStageAddr = 0x804B1968u;
constexpr uint32_t kSubmarine00Vt = 0x97EA0F20u;
constexpr uint32_t kWaterMineVt = 0x97EA1418u;
constexpr uint32_t kTorpedoVt = 0x97EA1120u;
constexpr uint32_t kWaterMine2Vt = 0x97EA1310u;
constexpr uint32_t kSubmarine01Vt = 0x97EA0E38u;  // fires setEShotBullet_eh05 orbs (eachUpdate 0x97E2BCC4)

std::atomic<bool> g_stop{false};

bool R32(uint32_t a, uint32_t& v) { return Memory::TryRead32(a, v); }
bool R8(uint32_t a, uint8_t& v) {
    uint32_t w;
    if (!R32(a & ~3u, w)) return false;
    v = static_cast<uint8_t>(w >> (8 * (3 - (a & 3))));
    return true;
}
float RF(uint32_t a) {
    uint32_t v = 0;
    return R32(a, v) ? std::bit_cast<float>(v) : NAN;
}
bool Ptr(uint32_t p) {
    return (p & 3) == 0 && ((p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u));
}

uint32_t StgGlobals() {
    uint32_t gm = 0, a = 0, sg = 0;
    if (!R32(kGmPtrAddr, gm) || !Ptr(gm) || !R32(gm + 0x60, a) || !Ptr(a) || !R32(a + 0x44, sg) || !Ptr(sg)) return 0;
    return sg;
}

double Now() {
    using C = std::chrono::steady_clock;
    static const auto t0 = C::now();
    return std::chrono::duration<double>(C::now() - t0).count();
}

std::vector<uint32_t> mines2, torpedoes;
std::map<uint32_t, double> old01Status;

void Scan(std::vector<uint32_t>& subs, std::vector<uint32_t>& mines) {
    subs.clear();
    mines.clear();
    mines2.clear();
    torpedoes.clear();
    const auto scan = [&](uint32_t lo, uint32_t hi) {
        for (uint32_t a = lo; a < hi; a += 4) {
            uint32_t v = 0, w = 0;
            if (!R32(a, v)) { a = (a | 0xFFFu); continue; }
            if ((v == kSubmarine00Vt || v == kSubmarine01Vt) && R32(a + 0x28, w) && w == v + 0xC) subs.push_back(a);
            else if (v == kWaterMineVt) mines.push_back(a);
            else if (v == kWaterMine2Vt) mines2.push_back(a);
            else if (v == kTorpedoVt) torpedoes.push_back(a);
        }
    };
    scan(0x80400000u, 0x81800000u);
    scan(0x90000000u, 0x94000000u);
}

struct SubState {
    uint8_t mode = 0xFF, step = 0xFF;
};

void Stage2(double now, double& nextScan, std::vector<uint32_t>& subs, std::vector<uint32_t>& mines,
            std::map<uint32_t, SubState>& seen, double& nextStatus) {
    const uint32_t sg = StgGlobals();
    const float dist = sg ? RF(sg + 0x268) : NAN;
    if (now >= nextScan) {
        nextScan = now + 1.0;
        const size_t oldMines = mines.size();
        Scan(subs, mines);
        if (mines.size() != oldMines)
            std::fprintf(stderr, "[watch] t=%.1f dist=%.1f WaterMines live=%zu\n", now, dist, mines.size());
    }
    for (uint32_t s : subs) {
        uint32_t vt = 0;
        if (!R32(s, vt) || (vt != kSubmarine00Vt && vt != kSubmarine01Vt)) continue;
        SubState cur;
        R8(s + 0x12C, cur.mode);
        R8(s + 0x12F, cur.step);
        if (vt == kSubmarine01Vt && now >= old01Status[s]) {
            old01Status[s] = now + 1.0;
            uint32_t type = 0, stepTimer = 0, counter = 0, cam = 0;
            R32(s + 0x3AC, type);
            R32(s + 0x138, stepTimer);
            if (sg) R32(sg + 0x4B4, counter);
            float plane = NAN;
            if (sg && R32(sg + 4, cam) && Ptr(cam)) {
                plane = (RF(s + 0x38) - RF(cam + 0x178)) * RF(cam + 0x120) +
                        (RF(s + 0x3C) - RF(cam + 0x17C)) * RF(cam + 0x124) +
                        (RF(s + 0x40) - RF(cam + 0x180)) * RF(cam + 0x128);
            }
            std::fprintf(stderr, "[watch] t=%.1f sub01=%08X type=%u step=%u stimer=%u plane=%.1f s2ctr=%u mines2=%zu torpedoes=%zu\n",
                         now, s, type, cur.step, stepTimer, plane, counter, mines2.size(), torpedoes.size());
        }
        SubState& old = seen[s];
        if (cur.mode != old.mode || cur.step != old.step) {
            uint32_t ctr = 0, mtimer = 0;
            R32(s + 0x138, ctr);
            R32(s + 0x134, mtimer);
            std::fprintf(stderr,
                         "[watch] t=%.1f sub%s=%08X mode=%u step=%u ctr=%u mtimer=%u pos=(%.1f %.1f %.1f) dist=%.1f speed=%.3f scale=%.3f\n",
                         now, vt == kSubmarine01Vt ? "01" : "00", s, cur.mode, cur.step, ctr, mtimer, RF(s + 0x38), RF(s + 0x3C), RF(s + 0x40), dist,
                         sg ? RF(sg + 0x26C) : NAN, sg ? RF(sg + 0x270) : NAN);
            old = cur;
        }
    }
    if (now >= nextStatus) {
        nextStatus = now + 2.0;
        std::fprintf(stderr, "[watch] t=%.1f stage2 dist=%.1f subs=%zu mines=%zu\n", now, dist, subs.size(),
                     mines.size());
    }
}

void Stage0(double now, std::map<uint32_t, float>& lastY, double& nextStatus, bool& wallsLogged) {
    const uint32_t sg = StgGlobals();
    uint32_t mgr = 0;
    if (!sg || !R32(sg + 0x494 + 0x10, mgr) || !Ptr(mgr)) return;
    uint32_t n = 0, nw = 0;
    R32(mgr + 0x238, n);
    R32(mgr + 0x23C, nw);
    if (n > 32 || nw > 32) return;
    if (!wallsLogged && nw) {
        wallsLogged = true;
        for (uint32_t i = 0; i < nw; ++i) {
            uint32_t w = 0;
            if (!R32(mgr + 0x1B8 + 4 * i, w) || !Ptr(w)) continue;
            std::fprintf(stderr, "[watch] wall%u=%08X pos=(%.2f %.2f %.2f) half=(%.2f %.2f %.2f) q=(%.3f %.3f %.3f %.3f) mass=%g\n",
                         i, w, RF(w), RF(w + 4), RF(w + 8), RF(w + 0xC), RF(w + 0x10), RF(w + 0x14), RF(w + 0xCC),
                         RF(w + 0xD0), RF(w + 0xD4), RF(w + 0xD8), RF(w + 0xDC));
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t b = 0;
        if (!R32(mgr + 0x38 + 4 * i, b) || !Ptr(b)) continue;
        const float y = RF(b + 4);
        auto it = lastY.find(b);
        const bool moved = it == lastY.end() || std::fabs(it->second - y) > 0.05f;
        if (moved) {
            uint8_t sleep = 0;
            R8(b + 0x174, sleep);
            std::fprintf(stderr,
                         "[watch] t=%.2f box=%08X pos=(%.2f %.2f %.2f) half=(%.2f %.2f %.2f) v=(%.2f %.2f %.2f) w=(%.2f %.2f %.2f) sleep=%u\n",
                         now, b, RF(b), y, RF(b + 8), RF(b + 0xC), RF(b + 0x10), RF(b + 0x14), RF(b + 0x140),
                         RF(b + 0x144), RF(b + 0x148), RF(b + 0x14C), RF(b + 0x150), RF(b + 0x154), sleep);
            lastY[b] = y;
        }
    }
    if (now >= nextStatus) {
        nextStatus = now + 5.0;
        std::fprintf(stderr, "[watch] t=%.1f stage0 boxes=%u walls=%u\n", now, n, nw);
    }
}

void Watcher() {
    const char* hzText = std::getenv("SIN_DEBUG_WATCH_HZ");
    const int hz = hzText && *hzText ? std::max(1, std::atoi(hzText)) : 20;
    std::vector<uint32_t> subs, mines;
    std::map<uint32_t, SubState> seen;
    std::map<uint32_t, float> lastY;
    double nextScan = 0, nextStatus = 0;
    bool wallsLogged = false;
    uint8_t lastStage = 0xFE;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / hz));
        uint8_t stage = 0xFF;
        R8(kLoadedStageAddr, stage);
        const double now = Now();
        if (stage != lastStage) {
            std::fprintf(stderr, "[watch] t=%.1f loaded stage %u\n", now, stage);
            lastStage = stage;
            subs.clear();
            mines.clear();
            seen.clear();
            lastY.clear();
            wallsLogged = false;
        }
        if (stage == 2) Stage2(now, nextScan, subs, mines, seen, nextStatus);
        else if (stage == 0) Stage0(now, lastY, nextStatus, wallsLogged);
        std::fflush(stderr);
    }
}

struct DebugWatchStarter {
    DebugWatchStarter() {
        const char* v = std::getenv("SIN_DEBUG_WATCH");
        if (!v || !*v || *v == '0') return;
        std::atexit([] { g_stop.store(true); });
        std::thread(Watcher).detach();
    }
};

DebugWatchStarter g_debugWatchStarter;

} // namespace
