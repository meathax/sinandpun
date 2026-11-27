// Debug aid: SIN_AUTOSTAGE=<0..7> steers the game's own stage-flow task onto stage n.
//
// Nothing here replaces translated code. A host watcher thread polls guest memory and pokes the
// same fields the game's flow task reads, so the original code does the module load (fn_8001F478
// -> stgNN.rso), exactly as a normal stage change would.
//
// Evidence (tools/ + generated/functions, snapshot build/native/UserData/Logs/base_*/mem1.bin):
//   r13 (SDA) = 0x804B7700. GM = *(0x804B19A0) (r13-23904) is the global game-state object.
//   GM+100 = GameData: u32 flags at +0 (bit 0x40000000 = "start stage from select" consumed by
//   fn_800C2704 state 5), bytes +8/+9/+10 = (stage,a,b) "current/continue" (setter fn_8008F0A4),
//   bytes +144/+145/+146 = (stage,a,b) "requested next" (read at 0x800C2A74 via fn_8008F0D0).
//   Flow task F (class with vptr 0x802FD608 at +0 and 0x802FD620 at +0x30, 72 bytes, created by
//   fn_800C2FC8): byte +54 = state (fn_800C2704 jump table @0x802FD580: 3 = load stage, 5 = pick
//   next stage), +55/+56/+57 = (stage,a,b). State 3 calls fn_800F9190 -> fn_8001F478(stage,a,b).
//   Loaded stage id byte = *(0x804B1968) (r13-23960), module object = *(0x804B196C).
//
// Environment:
//   SIN_AUTOSTAGE=n          enable, stage 0..7
//   SIN_AUTOSTAGE_A / _B     the two sub-parameters (bytes +56/+57, default 0)
//   SIN_AUTOSTAGE_DELAY=sec  do not touch anything before this many seconds after boot (default 0)
//   SIN_AUTOSTAGE_FORCE=0    never redirect a stage that is already loading/loaded (default 1)
//   SIN_AUTOSTAGE_SEED=0     do not pre-seed the GameData stage bytes (default 1)
//   SIN_AUTOSTAGE_TRACE=1    log scene / game-over jingle slot / sound-manager entry changes (see TraceSoundSlot)
//   SIN_AUTOSTAGE_SEQ=a,b    select each stage through fn_800C2704; active scenes are closed through
//                            fn_8003D35C's normal object flags before the next module loads
//   SIN_QA_DEATH_EVERY=sec  While the stage runs, request native player death every <sec> seconds.
//   SIN_AUTOSTAGE_PROBE=1    only log flow-task/GameData changes (use during a normal menu run)
//   SIN_AUTOSTAGE_FIXED_ADDR=0xADDR  skip the heap scan, use this address for F

#include "memory.h"
#include "guest_abi/guest_region.h"
#include "runtime_log.h"

#include <aurora/aurora.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

namespace {

const uint32_t kGmPtrAddr = MKW_GADDR(804B19A0);
const uint32_t kLoadedStageAddr = MKW_GADDR(804B1968);
const uint32_t kFlowVptr0 = MKW_GADDR(802FD608);
const uint32_t kFlowVptr30 = MKW_GADDR(802FD620);
constexpr uint32_t kScanLo = 0x80400000u;
constexpr uint32_t kScanHi = 0x81800000u;

constexpr uint32_t kGameData = 100;      // GM+100
constexpr uint32_t kGdCurrent = 8;       // +8..10
constexpr uint32_t kGdNext = 144;        // +144..146
constexpr uint32_t kFlagStartSelected = 0x40000000u;
constexpr uint32_t kFlagPlayerDead = 1u << 3; // Player::_dead_chk: rotl32(flags, 29) & 1
constexpr uint32_t kFlowState = 54;
constexpr uint32_t kFlowStage = 55;

std::atomic<bool> g_stop{false};

bool Rd32(uint32_t a, uint32_t& v) {
    return Memory::TryRead32(a, v);
}

bool Rd8(uint32_t a, uint8_t& v) {
    try {
        v = Memory::Read8(a);
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

bool Wr8(uint32_t a, uint8_t v) {
    try {
        Memory::Write8(a, v);
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

bool Wr32(uint32_t a, uint32_t v) {
    return Memory::TryWrite32(a, v);
}

bool IsFlow(uint32_t f) {
    uint32_t a = 0, b = 0;
    if (!Rd32(f, a) || a != kFlowVptr0 || !Rd32(f + 0x30, b) || b != kFlowVptr30) {
        return false;
    }
    uint8_t st = 0xFF;
    return Rd8(f + kFlowState, st) && st <= 10;
}

uint32_t ScanForFlow() {
    for (uint32_t a = kScanLo; a < kScanHi; a += 8) {
        uint32_t v = 0;
        if (Rd32(a, v) && v == kFlowVptr0 && IsFlow(a)) {
            return a;
        }
    }
    return 0;
}

int EnvInt(const char* name, int def) {
    const char* e = std::getenv(name);
    return (e && *e) ? static_cast<int>(std::strtol(e, nullptr, 0)) : def;
}

// SIN_AUTOSTAGE_TRACE=1: log every change of the state that decides the game-over crash (docs/SWAP-CRASH-NOTES.md):
// current scene (GM+96 -> scene object, label string at +4), game-over jingle slot variable 0x804B1CD0, sound
// manager entry for that slot (mgr = *(GM+92), entries = *(mgr+1004), 68 bytes each: +40 state (5 = unloaded),
// +44 handle h, +48 data), *h, and the game-data flags. "DANGLING" = slot var names an unloaded entry whose
// handle no longer points at a MEM2 sound object.
void TraceSoundSlot(uint32_t gm, double now) {
    static uint32_t last[10];
    static bool first = true;
    uint32_t scene = 0, vp = 0, l0 = 0, l1 = 0, slot = 0xFFFFFFFFu, mgr = 0, arr = 0, flags = 0;
    Rd32(gm + 96, scene);
    if (scene >= 0x80000000u) {
        Rd32(scene, vp);
        Rd32(scene + 4, l0);
        Rd32(scene + 8, l1);
    }
    Rd32(MKW_GADDR(804B1CD0), slot);
    Rd32(gm + 92, mgr);
    Rd32(gm + 100, flags);
    uint32_t st = 0, h = 0, ph = 0, d48 = 0;
    if (mgr >= 0x80000000u && slot < 32 && Rd32(mgr + 1004, arr) && arr >= 0x80000000u) {
        const uint32_t e = arr + slot * 68u;
        Rd32(e + 40, st);
        Rd32(e + 44, h);
        Rd32(e + 48, d48);
        if (h >= 0x80000000u) {
            Rd32(h, ph);
        }
    }
    const uint32_t cur[10] = {vp, l0, l1, slot, st, h, ph, d48, flags, 0};
    if (!first && std::equal(std::begin(cur), std::end(cur), std::begin(last))) {
        return;
    }
    first = false;
    std::copy(std::begin(cur), std::end(cur), std::begin(last));
    char label[9] = {0};
    for (int i = 0; i < 4; ++i) {
        label[i] = static_cast<char>(l0 >> (24 - 8 * i));
        label[4 + i] = static_cast<char>(l1 >> (24 - 8 * i));
    }
    for (char& c : label) {
        if (c < 32 || c > 126) c = c ? '.' : 0;
    }
    const bool dangling = slot < 32 && st == 5 && d48 == 0 && ph < 0x90000000u;
    RT_LOGF("autostage", "t=%.2f scene=%08X vptr=%08X '%s' slotvar=%d entry.state=%u h=%08X *h=%08X data=%08X flags=%08X%s\n",
            now, scene, vp, label, static_cast<int>(slot), st, h, ph, d48, flags, dangling ? " DANGLING" : "");
}

// fn_8003D35C(scene, 0) requests a normal scene-manager exit by setting the close bit and target 0 on
// the current +0x40 chain, from child to root. cbResume__7TcStage uses this exact path when its stage
// is allowed to finish. Reproduce the state transition here instead of invoking translated guest code
// from the watcher thread.
bool RequestSceneExit(uint32_t gm) {
    uint32_t root = 0;
    if (!Rd32(gm + 96, root) || root < 0x80000000u || root >= 0x81800000u) {
        return false;
    }

    struct SceneExit {
        uint32_t address;
        uint8_t flags;
    };
    std::vector<SceneExit> scenes;
    for (uint32_t scene = root; scene >= 0x80000000u && scene < 0x81800000u && scenes.size() < 64;) {
        const auto duplicate =
            std::find_if(scenes.begin(), scenes.end(), [scene](const SceneExit& item) { return item.address == scene; });
        if (duplicate != scenes.end()) {
            return false;
        }
        uint8_t objectFlags = 0;
        if (!Rd8(scene + 46, objectFlags)) {
            return false;
        }
        scenes.push_back({scene, objectFlags});
        uint32_t child = 0;
        if (!Rd32(scene + 40, child)) {
            return false;
        }
        if (child == 0) {
            break;
        }
        scene = child;
    }

    for (auto it = scenes.rbegin(); it != scenes.rend(); ++it) {
        if (!Wr8(it->address + 46, it->flags | 2u) || !Wr32(it->address + 76, 0)) {
            return false;
        }
    }
    return !scenes.empty();
}

void Watcher() {
    const char* stageEnv = std::getenv("SIN_AUTOSTAGE");
    int stage = static_cast<int>(std::strtol(stageEnv, nullptr, 0));
    // SIN_AUTOSTAGE_SEQ="a,b,c": after the first load is reached, keep switching stages (swap test) every GAP seconds.
    std::vector<int> seq;
    if (const char* s = std::getenv("SIN_AUTOSTAGE_SEQ")) {
        for (const char* c = s; *c;) {
            char* end = nullptr;
            const long v = std::strtol(c, &end, 0);
            if (end == c) break;
            seq.push_back(static_cast<int>(v));
            c = (*end == ',' || *end == '/' || *end == ';') ? end + 1 : end;
        }
        if (!seq.empty()) stage = seq[0];
    }
    size_t seqIndex = 0;
    const double seqGap = std::getenv("SIN_AUTOSTAGE_GAP") ? std::atof(std::getenv("SIN_AUTOSTAGE_GAP")) : 25.0;
    double nextSwapAt = 0.0;
    const char* captureDir = std::getenv("SIN_AUTOSTAGE_CAPTURE_DIR");
    int captureStage = -1;
    double captureAt = 0.0;
    const bool probe = EnvInt("SIN_AUTOSTAGE_PROBE", 0) != 0;
    if (!probe && (stage < 0 || stage > 7)) {
        RT_LOGF("autostage", "SIN_AUTOSTAGE=%s invalid (0..7); disabled\n", stageEnv);
        return;
    }
    const uint8_t pa = static_cast<uint8_t>(EnvInt("SIN_AUTOSTAGE_A", 0));
    const uint8_t pb = static_cast<uint8_t>(EnvInt("SIN_AUTOSTAGE_B", 0));
    const bool force = EnvInt("SIN_AUTOSTAGE_FORCE", 1) != 0;
    const bool seed = EnvInt("SIN_AUTOSTAGE_SEED", 1) != 0;
    const double delaySec = std::getenv("SIN_AUTOSTAGE_DELAY") ? std::atof(std::getenv("SIN_AUTOSTAGE_DELAY")) : 0.0;
    uint32_t fixedF = 0;
    if (const char* e = std::getenv("SIN_AUTOSTAGE_FIXED_ADDR")) {
        fixedF = static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
    }

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    auto since = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
    // Guest memory is not mapped for the first moments of the process.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    RT_LOGF("autostage", "armed: stage=%d a=%u b=%u force=%d seed=%d probe=%d delay=%.1fs\n", stage, pa, pb, force,
            seed, probe, delaySec);

    uint32_t f = fixedF;
    uint32_t gm = 0;
    int lastState = -1;
    uint32_t lastFlags = 0xFFFFFFFFu;
    uint32_t lastLoaded = 0xFFFFFFFFu;
    double nextScan = 0.0, nextStatus = 5.0, nextTrace = 0.0;
    const bool trace = EnvInt("SIN_AUTOSTAGE_TRACE", 0) != 0;
    const double deathEvery = std::getenv("SIN_QA_DEATH_EVERY") ? std::atof(std::getenv("SIN_QA_DEATH_EVERY")) : 0.0;
    double nextDeath = deathEvery;
    bool patched = false, forced = false, announcedFlow = false, done = false;

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const double now = since();
        if (!Rd32(kGmPtrAddr, gm) || gm < 0x80000000u || gm >= 0x81800000u) {
            continue;
        }
        uint32_t flags = 0;
        Rd32(gm + kGameData, flags);
        uint8_t loaded = 0xFF;
        Rd8(kLoadedStageAddr, loaded);
        if (captureStage >= 0 && now >= captureAt) {
            if (loaded == captureStage) {
                const auto path = std::filesystem::path(captureDir) /
                                  ("stage-" + std::to_string(captureStage) + ".png");
                aurora_capture_present_image(path.string().c_str());
            }
            captureStage = -1;
            if (done && !trace) return;
        }

        if (probe) {
            uint8_t c0 = 0, c1 = 0, c2 = 0, n0 = 0, n1 = 0, n2 = 0;
            Rd8(gm + kGameData + kGdCurrent, c0);
            Rd8(gm + kGameData + kGdCurrent + 1, c1);
            Rd8(gm + kGameData + kGdCurrent + 2, c2);
            Rd8(gm + kGameData + kGdNext, n0);
            Rd8(gm + kGameData + kGdNext + 1, n1);
            Rd8(gm + kGameData + kGdNext + 2, n2);
            const uint32_t sig = flags ^ (loaded << 24) ^ (c0 | c1 << 8 | c2 << 16) ^ (n0 << 3 | n1 << 11 | n2 << 19);
            if (sig != lastFlags) {
                lastFlags = sig;
                RT_LOGF("autostage", "t=%.2f GM=%08X flags=%08X cur=%u,%u,%u next=%u,%u,%u loadedId=%u\n", now, gm,
                        flags, c0, c1, c2, n0, n1, n2, loaded);
            }
        }

        if (trace && now >= nextTrace) {
            nextTrace = now + 0.05;
            TraceSoundSlot(gm, now);
        }

        if (deathEvery > 0 && now >= nextDeath && loaded <= 7) {
            uint32_t fl = 0;
            uint8_t fst = 0;
            if (f && IsFlow(f) && Rd8(f + kFlowState, fst) && fst == 4 && Rd32(gm + kGameData, fl) && !(fl & kFlagPlayerDead)) {
                if (Memory::TryWrite32(gm + kGameData, fl | kFlagPlayerDead)) {
                    RT_LOGF("autostage", "t=%.2f QA death: GM flags %08X -> %08X\n", now, fl, fl | kFlagPlayerDead);
                }
                nextDeath = now + deathEvery;
            }
        }

        if (now < delaySec || now < nextSwapAt) {
            continue;
        }

        if (!f || !IsFlow(f)) {
            f = 0;
            announcedFlow = false;
            if (now >= nextScan && !fixedF) {
                f = ScanForFlow();
                nextScan = now + 0.05;
            }
            if (!f) {
                if (now >= nextStatus) {
                    nextStatus = now + 10.0;
                    RT_LOGF("autostage", "t=%.0fs waiting for flow task (GM=%08X flags=%08X loadedId=%u)\n", now, gm,
                            flags, loaded);
                }
                continue;
            }
        }
        uint8_t st = 0;
        Rd8(f + kFlowState, st);
        uint8_t s0 = 0, s1 = 0, s2 = 0;
        Rd8(f + kFlowStage, s0);
        Rd8(f + kFlowStage + 1, s1);
        Rd8(f + kFlowStage + 2, s2);
        if (!announcedFlow) {
            announcedFlow = true;
            RT_LOGF("autostage", "t=%.2f flow task F=%08X state=%u stage=%u,%u,%u loadedId=%u\n", now, f, st, s0, s1,
                    s2, loaded);
        }
        if (st != lastState || loaded != lastLoaded) {
            if (probe || st != lastState) {
                RT_LOGF("autostage", "t=%.2f F state %d -> %u stage=%u,%u,%u loadedId=%u\n", now, lastState, st, s0,
                        s1, s2, loaded);
            }
            lastState = st;
            lastLoaded = loaded;
        }
        if (probe || done) {
            continue;
        }

        if (seed && !patched && !forced) {
            // Cheap and harmless until the first load: anything that derives the starting stage
            // from GameData sees the requested stage.
            Wr8(gm + kGameData + kGdCurrent, static_cast<uint8_t>(stage));
            Wr8(gm + kGameData + kGdNext, static_cast<uint8_t>(stage));
        }

        const bool checkpointRequested = std::getenv("SIN_AUTOSTAGE_A") || std::getenv("SIN_AUTOSTAGE_B");
        const bool targetMatches = static_cast<int>(s0) == stage &&
                (!checkpointRequested || (s1 == pa && s2 == pb));
        if (st <= 2) {
            // Before the load state: the loader will read F+55..57 on the next state-3 tick.
            if (s0 != stage || s1 != pa || s2 != pb) {
                Wr8(f + kFlowStage, static_cast<uint8_t>(stage));
                Wr8(f + kFlowStage + 1, pa);
                Wr8(f + kFlowStage + 2, pb);
                if (!patched) {
                    RT_LOGF("autostage", "t=%.2f patched F=%08X stage %u,%u,%u -> %d,%u,%u (state %u)\n", now, f, s0,
                            s1, s2, stage, pa, pb, st);
                }
                patched = true;
            }
        } else if (force && !forced && !targetMatches) {
            // Flow already past the load: use the game's own "next stage" path (state 5 consumes
            // the 0x40000000 flag and GameData+144..146, then re-enters state 3).
            if (st == 4 || st == 6) {
                Wr8(gm + kGameData + kGdNext, static_cast<uint8_t>(stage));
                Wr8(gm + kGameData + kGdNext + 1, pa);
                Wr8(gm + kGameData + kGdNext + 2, pb);
                uint32_t fl = 0;
                if (Rd32(gm + kGameData, fl)) {
                    fl = (fl | kFlagStartSelected) & ~0x80000000u;
                    Wr32(gm + kGameData, fl);
                }
                const bool sceneExit = RequestSceneExit(gm);
                if (!sceneExit) {
                    continue;
                }
                RT_LOGF("autostage", "t=%.2f requested guest scene exit before stage %d\n", now, stage);
                Wr8(f + kFlowState, 5);
                forced = true;
                RT_LOGF("autostage", "t=%.2f forced stage change F=%08X state %u -> 5, next=%d,%u,%u sceneExit=%d\n",
                        now, f, st, stage, pa, pb, sceneExit ? 1 : 0);
            }
        }

        if ((patched || forced || !seq.empty()) && st >= 4 && targetMatches) {
            RT_LOGF("autostage", "t=%.2f stage %d load reached (F state %u, loadedId=%u)\n", now, stage, st, loaded);
            if (captureDir && *captureDir) {
                captureStage = stage;
                captureAt = now + std::max(seqGap - 3.0, 1.0);
            }
            if (seqIndex + 1 >= seq.size()) {
                RT_LOGF("autostage", "watcher done%s\n", trace ? " (tracing continues)" : "");
                if (!trace && captureStage < 0) return;
                done = true;
                continue;
            }
            ++seqIndex;
            stage = seq[seqIndex];
            nextSwapAt = now + seqGap;
            patched = false;
            forced = false;
            RT_LOGF("autostage", "sequence: next stage %d in %.0fs\n", stage, seqGap);
        }
    }
}

struct AutoStageStarter {
    AutoStageStarter() {
        if (!std::getenv("SIN_AUTOSTAGE") && !std::getenv("SIN_AUTOSTAGE_PROBE")) {
            return;
        }
        if (!std::getenv("SIN_AUTOSTAGE")) {
#if defined(_WIN32)
            _putenv_s("SIN_AUTOSTAGE", "0");
#else
            setenv("SIN_AUTOSTAGE", "0", 1);
#endif
        }
        std::atexit([] { g_stop.store(true); });
        std::thread(Watcher).detach();
    }
};

AutoStageStarter g_autoStageStarter;

} // namespace
