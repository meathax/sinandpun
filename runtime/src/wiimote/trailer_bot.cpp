// Capture-only autoplay for trailers and long visual QA. See trailer_bot.h.
//
// Guest layout (R2VE01, evidence: disassembly of ObjHit::entry 0x800A3C90, ObjHit::convLockOnPos
// 0x800A38BC, MEtc::isHitDamageEnemy 0x8011D0FC, Ng::GfxGetCam 0x80029AAC, Ng::Cam::conv3Dto2D 0x800207C4):
//   GM = *(0x804B19A0). Hit system H = *(*(*(GM+0x60)+0x44)+0x14).
//   u16 H+0x3A = number of entried ObjHits, H+0x44 + 4*i = ObjHit*.
//   ObjHit+0x14 u16 lock-on slot (0xFFFF none); slot record R = H+0x194C0 + 64*slot:
//     R+3 = position converted this frame, R+8 Vec3 world lock-on position.
//   Lockable targets have a slot (ObjHit+0x14 != 0xFFFF) whose record byte R+1 is set while entried.
//   (+0x48/+0x4C/+0x80/+0x84 are per-frame hit results, cleared by entry.) Life: s32 16.16 at ObjHit+0x1C.
//   Camera n = *(*(GM+0x58)+0x1C0) + 0x20C*n; conv3Dto2D projects clip = M44(cam) * [p,1], ndc = clip/w.
//
// Environment:
//   SIN_TRAILER_BOT=1         enable (both seats when enhanced co-op is on)
//   SIN_TRAILER_BOT_GOD=1     keep the shared team health topped up (capture only)
//   SIN_TRAILER_BOT_CAM=n     camera index (default 0); SIN_TRAILER_BOT_LOG=1 logs targets twice a second
//   SIN_TRAILER_BOT_SEED=n    randomness seed
//   SIN_TRAILER_BOT_FLIPY=1   flip projected y if a build's camera convention differs

#include "trailer_bot.h"

#include "memory.h"
#include "guest_abi/guest_region.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

bool SinCoopLastActors(uint32_t& lead, uint32_t& partner);  // hle/sin_coop.cpp

namespace TrailerBot {
namespace {

using Clock = std::chrono::steady_clock;

bool R32(uint32_t a, uint32_t& v) { return Memory::TryRead32(a, v); }
bool RF(uint32_t a, float& f) {
    uint32_t v;
    if (!R32(a, v)) return false;
    f = std::bit_cast<float>(v);
    return std::isfinite(f);
}
bool Valid(uint32_t p) { return p >= 0x80000000u && p < 0x81800000u && (p & 3) == 0; }

int EnvInt(const char* name, int fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : fallback;
}

struct Target {
    uint32_t id = 0;     // ObjHit address: stable for the object's lifetime
    float u = 0, v = 0;  // screen fractions
    float depth = 0;     // clip w
    float life = 0;
};

struct Smooth1D {  // smooth value noise: sum of incommensurate sines with random phases
    float p[3]{}, f[3]{};
    void Init(std::mt19937& rng, float base) {
        std::uniform_real_distribution<float> ph(0.f, 6.2831853f), fr(0.75f, 1.35f);
        for (int i = 0; i < 3; ++i) { p[i] = ph(rng); f[i] = base * fr(rng) * (1.f + 0.9f * i); }
    }
    float At(float t) const {
        return (std::sin(t * f[0] + p[0]) * 0.55f + std::sin(t * f[1] + p[1]) * 0.3f +
                std::sin(t * f[2] + p[2]) * 0.15f);
    }
};

struct Seat {
    bool init = false;
    std::mt19937 rng;
    float u = 0.5f, v = 0.48f, vu = 0, vv = 0;      // pointer state
    float gu = 0.5f, gv = 0.48f;                     // goal the "hand" is currently heading for
    uint32_t target = 0;
    float targetSince = 0, dwell = 1.f;
    float reactUntil = 0;                            // reaction delay after picking a new target
    float lastSeenU = 0.5f, lastSeenV = 0.5f;
    float tu = 0.5f, tv = 0.5f;                      // perceived (lagged) target position
    Smooth1D jitterU, jitterV, wanderU, wanderV, moveX, moveY;
    float fireGapUntil = 0, nextFireGap = 2.f;
    float chargeUntil = 0, nextCharge = 7.f, chargeRelease = 0;
    float lockTapUntil = 0, nextLockTap = 3.f;
    float dodgeUntil = 0, nextDodge = 6.f, dodgeX = 0, dodgeY = 0;
    float jumpUntil = 0, nextJump = 9.f;
    float aimBiasU = 0, aimBiasV = 0;                // per-target hit point offset (not dead centre)
};

std::mutex g_mutex;
std::atomic<uint64_t> g_frame{0};       // guest frames produced
std::atomic<int64_t> g_startFrame{-1};  // g_frame when gameplay first became controllable
uint64_t g_lastFrame[2]{~0ull, ~0ull};
Output g_cached[2];
std::array<Seat, 2> g_seats;
std::vector<Target> g_targets;
float g_scanAt = -1.f;
float g_lastT[2]{};
uint32_t g_playerHit[2]{};
float g_actorPos[2][2]{};   // screen-plane x,y of each player actor (actor+384/+388)
bool g_actorValid[2]{};

float Rand(Seat& s, float a, float b) { return std::uniform_real_distribution<float>(a, b)(s.rng); }

bool CamMatrix(uint32_t gm, float m[16]) {
    static const int index = EnvInt("SIN_TRAILER_BOT_CAM", 0);
    uint32_t gfx, cams;
    if (!R32(gm + 0x58, gfx) || !Valid(gfx) || !R32(gfx + 0x1C0, cams) || !Valid(cams)) return false;
    const uint32_t cam = cams + 0x20Cu * static_cast<uint32_t>(index);
    for (int i = 0; i < 16; ++i)
        if (!RF(cam + 4u * i, m[i])) return false;
    return true;
}

void ResolvePlayerHits(uint32_t gm) {
    // Seat 0 drives the lead actor, seat 1 the partner (hle/sin_coop.cpp resolves both).
    g_playerHit[0] = g_playerHit[1] = 0;
    uint32_t lead, partner;
    if (SinCoopLastActors(lead, partner) && Valid(lead) && Valid(partner)) {
        g_playerHit[0] = lead + 668;
        g_playerHit[1] = partner + 668;
    }
    (void)gm;
    for (int i = 0; i < 2; ++i) {
        g_actorValid[i] = g_playerHit[i] && RF(g_playerHit[i] - 668 + 384, g_actorPos[i][0]) &&
                          RF(g_playerHit[i] - 668 + 388, g_actorPos[i][1]);
    }
}

void KeepTeamAlive(uint32_t gm) {
    static const bool god = EnvInt("SIN_TRAILER_BOT_GOD", 0) != 0;
    if (!god) return;
    for (uint32_t hit : g_playerHit) {
        if (!hit) continue;
        uint32_t cur, max;
        if (!R32(hit + 28, cur) || !R32(hit + 32, max)) continue;
        if (static_cast<int32_t>(max) > 0 && static_cast<int32_t>(cur) > 0 &&
            static_cast<int32_t>(cur) < static_cast<int32_t>(max) * 3 / 4) {
            try { Memory::Write32(hit + 28, max); } catch (...) {}
        }
    }
    (void)gm;
}

void Scan(float t) {
    if (t - g_scanAt < 0.008f) return;
    g_scanAt = t;
    g_targets.clear();
    static const bool flipY = EnvInt("SIN_TRAILER_BOT_FLIPY", 0) != 0;
    static const bool log = EnvInt("SIN_TRAILER_BOT_LOG", 0) != 0;
    static float nextLog = 0;
    uint32_t gm, a, m, h;
    if (!R32(MKW_GADDR(804B19A0), gm) || !Valid(gm)) return;
    ResolvePlayerHits(gm);
    KeepTeamAlive(gm);
    if (!R32(gm + 0x60, a) || !Valid(a) || !R32(a + 0x44, m) || !Valid(m) || !R32(m + 0x14, h) || !Valid(h)) return;
    float c[16];
    if (!CamMatrix(gm, c)) return;
    uint32_t countWord;
    if (!R32(h + 0x38, countWord)) return;
    const uint32_t count = std::min<uint32_t>(countWord & 0xFFFFu, 512);
    const bool doLog = log && t >= nextLog;
    if (doLog) {
        nextLog = t + 0.5f;
        std::fprintf(stderr, "[tbot] t=%.1f hits=%u cam=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",
                     t, count, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13],
                     c[14], c[15]);
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t oh;
        if (!R32(h + 0x44 + 4 * i, oh) || !Valid(oh)) continue;
        if (oh == g_playerHit[0] || oh == g_playerHit[1]) continue;
        uint32_t w14, dmgTable, dmgFlags, lifeRaw;
        if (!R32(oh + 0x14, w14) || !R32(oh + 0x48, dmgTable) || !R32(oh + 0x4C, dmgFlags) || !R32(oh + 0x1C, lifeRaw))
            continue;
        const uint32_t slot = w14 >> 16;  // big-endian u16 at +0x14
        if (doLog) {
            uint32_t rf = 0, r8 = 0;
            if (slot != 0xFFFFu) { R32(h + 0x194C0u + 64u * slot, rf); R32(h + 0x194C8u + 64u * slot, r8); }
            uint32_t q[8]{};
            for (int k = 0; k < 8; ++k) R32(oh + 0x30 + 4 * k, q[k]);
            std::fprintf(stderr, "[tbot]   raw oh=%08x w14=%08x 48=%08x 4c=%08x life=%08x rec=%08x/%08x  30..4c=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                         oh, w14, dmgTable, dmgFlags, lifeRaw, rf, r8, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
        }
        if (slot == 0xFFFFu || slot >= 1024 || static_cast<int32_t>(lifeRaw) <= 0) continue;
        const uint32_t rec = h + 0x194C0u + 64u * slot;
        uint32_t flags;
        float p[3];
        if (!R32(rec, flags) || !RF(rec + 8, p[0]) || !RF(rec + 12, p[1]) || !RF(rec + 16, p[2])) continue;
        if (!(flags & 0x00FF0000u)) continue;  // byte +1: entried (lock-on slot live)
        const float cx = c[0] * p[0] + c[1] * p[1] + c[2] * p[2] + c[3];
        const float cy = c[4] * p[0] + c[5] * p[1] + c[6] * p[2] + c[7];
        const float cw = c[12] * p[0] + c[13] * p[1] + c[14] * p[2] + c[15];
        if (!(cw > 0.05f)) continue;
        Target tg;
        tg.id = oh;
        tg.u = 0.5f + 0.5f * cx / cw;
        tg.v = flipY ? 0.5f + 0.5f * cy / cw : 0.5f - 0.5f * cy / cw;
        tg.depth = cw;
        tg.life = static_cast<int32_t>(lifeRaw) / 65536.f;
        if (doLog)
            std::fprintf(stderr, "[tbot]   hit=%08x slot=%u life=%.1f world=(%.1f %.1f %.1f) uv=(%.3f %.3f) w=%.1f\n", oh,
                         slot, tg.life, p[0], p[1], p[2], tg.u, tg.v, cw);
        if (tg.u < 0.03f || tg.u > 0.97f || tg.v < 0.04f || tg.v > 0.96f) continue;
        g_targets.push_back(tg);
    }
}

const Target* Find(uint32_t id) {
    for (const auto& t : g_targets)
        if (t.id == id) return &t;
    return nullptr;
}

void Think(int port, Seat& s, float t, float dt) {
    if (!s.init) {
        s.init = true;
        s.rng.seed(static_cast<uint32_t>(EnvInt("SIN_TRAILER_BOT_SEED", 7) * 7919 + port * 104729));
        s.jitterU.Init(s.rng, 2.1f);
        s.jitterV.Init(s.rng, 2.4f);
        s.wanderU.Init(s.rng, 0.35f);
        s.wanderV.Init(s.rng, 0.29f);
        s.moveX.Init(s.rng, 0.42f);
        s.moveY.Init(s.rng, 0.37f);
        s.u = port ? 0.58f : 0.42f;
        s.nextFireGap = t + Rand(s, 1.2f, 2.8f);
        s.nextCharge = t + Rand(s, 4.f, 8.f) + port * 2.5f;
        s.nextLockTap = t + Rand(s, 2.f, 4.f);
        s.nextDodge = t + Rand(s, 4.f, 8.f) + port * 1.7f;
        s.nextJump = t + Rand(s, 6.f, 11.f) + port * 3.1f;
    }
    const Seat& other = g_seats[port ^ 1];

    // ---- target choice: keep the current one for a human dwell time, prefer something the partner is not shooting
    const Target* cur = Find(s.target);
    const bool dwellDone = t - s.targetSince > s.dwell;
    if (!cur || dwellDone) {
        const Target* best = nullptr;
        float bestScore = 1e9f;
        for (const auto& tg : g_targets) {
            const float du = tg.u - s.u, dv = (tg.v - s.v) * 0.75f;
            float score = std::sqrt(du * du + dv * dv);
            if (other.init && tg.id == other.target && g_targets.size() > 1) score += 0.35f;
            if (tg.id == s.target) score -= 0.12f;                      // mild loyalty
            score -= std::min(tg.life, 400.f) * 0.0004f;               // bosses and big parts are attractive
            score += (tg.u < 0.1f || tg.u > 0.9f) ? 0.15f : 0.f;       // edge targets are less natural
            if (score < bestScore) { bestScore = score; best = &tg; }
        }
        if (best && best->id != s.target) {
            const float jump = std::hypot(best->u - s.u, best->v - s.v);
            s.target = best->id;
            s.targetSince = t;
            s.dwell = Rand(s, 0.9f, 2.4f);
            s.reactUntil = t + Rand(s, 0.11f, 0.22f) + jump * 0.12f;
            s.aimBiasU = Rand(s, -0.012f, 0.012f);
            s.aimBiasV = Rand(s, -0.010f, 0.016f);
            s.tu = best->u;
            s.tv = best->v;
            cur = best;
        } else if (best) {
            s.targetSince = t - s.dwell * 0.5f;  // same target still best: re-check a little later
        } else {
            s.target = 0;
        }
    }

    // ---- where the hand wants to be
    if (cur) {
        s.lastSeenU = cur->u;
        s.lastSeenV = cur->v;
        // perceived position lags the true one like eye tracking (~70 ms)
        const float k = 1.f - std::exp(-dt / 0.07f);
        s.tu += (cur->u - s.tu) * k;
        s.tv += (cur->v - s.tv) * k;
        if (t >= s.reactUntil) {
            s.gu = s.tu + s.aimBiasU;
            s.gv = s.tv + s.aimBiasV;
        }
    } else {
        // no target: rest near the middle of the action and drift slowly, like someone scanning for the next wave
        const float restU = 0.5f + (port ? 0.06f : -0.06f) + 0.13f * s.wanderU.At(t);
        const float restV = 0.46f + 0.09f * s.wanderV.At(t);
        const float k = 1.f - std::exp(-dt / 0.6f);
        s.gu += (restU - s.gu) * k;
        s.gv += (restV - s.gv) * k;
    }

    // ---- hand dynamics: slightly under-damped spring, faster for big flicks, plus small tremor
    const float du = s.gu - s.u, dv = s.gv - s.v;
    const float dist = std::hypot(du, dv);
    const float omega = 2.f * 3.14159265f * (cur ? (dist > 0.15f ? 3.0f : 2.2f) : 1.1f);
    const float zeta = cur ? 0.78f : 1.0f;
    const int steps = std::max(1, static_cast<int>(std::ceil(dt / 0.004f)));
    const float h = dt / steps;
    for (int i = 0; i < steps; ++i) {
        const float au = omega * omega * (s.gu - s.u) - 2.f * zeta * omega * s.vu;
        const float av = omega * omega * (s.gv - s.v) - 2.f * zeta * omega * s.vv;
        s.vu += au * h;
        s.vv += av * h;
        s.u += s.vu * h;
        s.v += s.vv * h;
    }
    s.u = std::clamp(s.u, 0.07f, 0.93f);
    s.v = std::clamp(s.v, 0.09f, 0.91f);
    if (s.u <= 0.07f || s.u >= 0.93f) s.vu = 0;
    if (s.v <= 0.09f || s.v >= 0.91f) s.vv = 0;
    (void)port;
}

void Act(int port, Seat& s, float t, Output& out) {
    const bool hasTarget = Find(s.target) != nullptr;
    const float tremor = 0.0035f;
    out.u = std::clamp(s.u + tremor * s.jitterU.At(t), 0.06f, 0.94f);
    out.v = std::clamp(s.v + tremor * s.jitterV.At(t), 0.08f, 0.92f);

    // fire: hold while engaged, with short human release gaps; sporadic bursts when nothing is up
    if (t >= s.nextFireGap) {
        s.fireGapUntil = t + Rand(s, 0.08f, 0.22f);
        s.nextFireGap = t + Rand(s, 1.4f, 3.2f);
    }
    bool fire = t >= s.fireGapUntil;
    if (!hasTarget) fire = fire && std::sin(t * 2.3f + port * 1.7f) > -0.2f;

    // charge shot: hold lock/charge, release on a target
    bool lock = false;
    if (t >= s.nextCharge && hasTarget && s.chargeUntil < t) {
        s.chargeUntil = t + Rand(s, 1.15f, 1.5f);
        s.nextCharge = t + Rand(s, 6.f, 10.f);
    }
    if (t < s.chargeUntil) {
        lock = true;
        fire = false;
    } else if (hasTarget && t >= s.nextLockTap) {
        s.lockTapUntil = t + 0.09f;
        s.nextLockTap = t + Rand(s, 3.f, 6.f);
    }
    if (t < s.lockTapUntil) lock = true;
    out.b = fire;
    out.a = lock;

    // movement: smooth wandering, each seat favouring its own side of the screen
    const float side = port ? 0.22f : -0.22f;
    float lx = std::clamp(side * 0.6f + 0.75f * s.moveX.At(t), -1.f, 1.f);
    float ly = std::clamp(0.55f * s.moveY.At(t), -1.f, 1.f);
    // Keep the two actors visibly apart: inside the personal-space radius both get pushed
    // away from each other along x (and a little in y), seat 1 to the right by default.
    static const float minSep = static_cast<float>(EnvInt("SIN_TRAILER_BOT_SEP", 20));
    if (g_actorValid[0] && g_actorValid[1]) {
        const float dx = g_actorPos[port][0] - g_actorPos[port ^ 1][0];
        const float dy = g_actorPos[port][1] - g_actorPos[port ^ 1][1];
        if (std::abs(dx) < minSep && std::abs(dy) < minSep * 0.9f) {
            const float dir = std::abs(dx) > 0.5f ? (dx > 0 ? 1.f : -1.f) : (port ? 1.f : -1.f);
            const float push = 1.f - std::abs(dx) / minSep;
            lx = std::clamp(lx * (1.f - push) + dir * (0.55f + 0.45f * push), -1.f, 1.f);
            ly = std::clamp(ly + (dy >= 0 ? 0.35f : -0.35f) * push, -1.f, 1.f);
        }
    }
    if (t >= s.nextDodge) {
        s.dodgeUntil = t + 0.12f;
        s.nextDodge = t + Rand(s, 4.5f, 9.f);
        s.dodgeX = Rand(s, 0.f, 1.f) < 0.5f ? -1.f : 1.f;
        s.dodgeY = Rand(s, -0.3f, 0.3f);
    }
    if (t < s.dodgeUntil) {
        out.z = true;
        lx = s.dodgeX;
        ly = s.dodgeY;
    }
    if (t >= s.nextJump) {
        s.jumpUntil = t + Rand(s, 0.35f, 0.9f);
        s.nextJump = t + Rand(s, 6.f, 12.f);
    }
    out.c = t < s.jumpUntil;
    out.lx = lx;
    out.ly = ly;
}

} // namespace

bool Enabled() {
    static const bool on = EnvInt("SIN_TRAILER_BOT", 0) != 0;
    return on;
}

bool Sample(int port, Output& out) {
    if (!Enabled() || port < 0 || port > 1) return false;
    std::lock_guard lock(g_mutex);
    const uint64_t frame = g_frame.load(std::memory_order_acquire);
    if (frame == g_lastFrame[port]) { out = g_cached[port]; return true; }  // one decision per guest frame
    const uint64_t prevFrame = g_lastFrame[port];
    g_lastFrame[port] = frame;
    const int64_t start = g_startFrame.load(std::memory_order_acquire);
    const float t = start >= 0 ? static_cast<float>(static_cast<int64_t>(frame) - start) / 60.f
                               : static_cast<float>(frame) / 60.f;
    float dt = prevFrame == ~0ull ? 1.f / 60.f : static_cast<float>(frame - prevFrame) / 60.f;
    g_lastT[port] = t;
    dt = std::clamp(dt, 1.f / 60.f, 0.05f);
    Scan(t);
    if (g_playerHit[0] && start < 0) {
        int64_t expected = -1;
        g_startFrame.compare_exchange_strong(expected, static_cast<int64_t>(frame));
    }
    if (!g_playerHit[0]) {
        // Boot, strap screens and menus: P1 taps A so autostage can take over; nobody aims.
        out = Output{};
        out.u = 0.795f;  // File Select: "Do Not Save" (keeps the player's save files untouched)
        out.v = 0.70f;
        out.a = port == 0 && (frame % 108) < 7;
        g_cached[port] = out;
        return true;
    }
    Seat& s = g_seats[port];
    Think(port, s, t, dt);
    Act(port, s, t, out);
    g_cached[port] = out;
    static FILE* trace = [] {
        const char* path = std::getenv("SIN_TRAILER_BOT_TRACE");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    if (trace) {
        std::fprintf(trace, "f=%llu s=%lld p=%d tid=%lu u=%.4f v=%.4f a=%d b=%d c=%d z=%d lx=%.3f ly=%.3f n=%zu act=%.3f,%.3f\n",
                     static_cast<unsigned long long>(frame), static_cast<long long>(start), port,
                     static_cast<unsigned long>(GetCurrentThreadId()), out.u, out.v, out.a, out.b, out.c, out.z, out.lx,
                     out.ly, g_targets.size(), g_actorPos[port][0], g_actorPos[port][1]);
        if ((frame & 63) == 0) std::fflush(trace);
    }
    return true;
}

void OnFramePresented() { g_frame.fetch_add(1, std::memory_order_acq_rel); }

unsigned FrameTag() {
    const int64_t start = g_startFrame.load(std::memory_order_acquire);
    if (start < 0) return 0;
    return static_cast<unsigned>((static_cast<int64_t>(g_frame.load(std::memory_order_acquire)) - start + 1) & 0xFFFF);
}

bool TagEnabled() {
    static const bool on = EnvInt("SIN_TRAILER_TAG", 0) != 0;
    return on;
}

} // namespace TrailerBot
