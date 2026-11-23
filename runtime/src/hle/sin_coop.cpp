#include "sin_coop.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "guest_abi/guest_abi.h"
#include "memory.h"
#include "runtime_config.h"
#include "virtual_remote.h"

namespace {
struct Players {
    uint32_t lead{};
    uint32_t partner{};
};

Players ResolvePlayers(uint32_t root, uint32_t stage) {
    if (!stage || !Memory::Contains(stage, 764)) return {};
    const uint32_t scene = Memory::Read32(root + 96);
    if (!scene || !Memory::Contains(scene, 72)) return {};
    const uint32_t game = Memory::Read32(scene + 36);
    const uint32_t owner = game ? game : scene;
    if (!Memory::Contains(owner, 72)) return {};
    const uint32_t globals = Memory::Read32(owner + 68);
    if (!globals || globals == stage || !Memory::Contains(globals, 10)) return {};
    const auto choice = Memory::Read8(globals + 9);
    if (choice > 1) return {};
    return {Memory::Read32(stage + 8 + 4 * choice), Memory::Read32(stage + 8 + 4 * (1 - choice))};
}
// Player::recoverLife and the HUD both use the embedded ObjHit at +668.
// ObjHit stores current and maximum life as signed fixed-point words at +28/+32.
struct TeamLife {
    Players bodies{};
    int32_t previous{};
};
thread_local TeamLife life;
thread_local Players borrowed;
thread_local uint32_t finaleConstruction;

void ShareLife(const Players& players, bool entering) {
    const auto leadHP = static_cast<int32_t>(Memory::Read32(players.lead + 696));
    const auto otherHP = static_cast<int32_t>(Memory::Read32(players.partner + 696));
    const auto maximum = static_cast<int32_t>(Memory::Read32(players.lead + 700));
    if (maximum <= 0) return;
    int64_t total = leadHP;
    if (!entering && life.bodies.lead == players.lead && life.bodies.partner == players.partner) {
        // Hits on both bodies during one update charge the larger loss once.
        const int64_t a = static_cast<int64_t>(leadHP) - life.previous;
        const int64_t b = static_cast<int64_t>(otherHP) - life.previous;
        total = life.previous + std::min({a, b, int64_t{0}}) + std::max({a, b, int64_t{0}});
    }
    life = {players, static_cast<int32_t>(std::clamp<int64_t>(total, 0, maximum))};
    Memory::Write32(players.lead + 696, life.previous);
    Memory::Write32(players.partner + 700, maximum);
    Memory::Write32(players.partner + 696, life.previous);
}

}

double SinCoopBaseLife(double base) {
    const bool enhanced = RuntimeConfigFile::EnhancedCoopAtLaunch();
    const double maximum = enhanced ? base * 1.5 : base;
    if (std::getenv("SIN_COOP_TRACE"))
        std::fprintf(stderr, "[coop-health] enhanced=%d base=%g maximum=%g\n", int(enhanced), base, maximum);
    return maximum;
}

uint32_t SinCoopChange(uint32_t sda, uint32_t stage, uint32_t actor, uint32_t original) {
    if (!RuntimeConfigFile::EnhancedCoopAtLaunch()) return original;
    const auto root = Memory::Read32(sda - 23904);
    const auto players = ResolvePlayers(root, stage);
    static const bool trace = std::getenv("SIN_COOP_TRACE") != nullptr;
    static unsigned samples[2]{};
    const unsigned seat = actor == players.partner ? 1 : 0;
    if (trace && (++samples[seat] <= 3 || samples[seat] % 60 == 0)) {
        const auto hub = Memory::Read32(root + 4);
        const auto pad = hub + 1924 + seat * 1464;
        std::fprintf(stderr, "[coop] seat=%u actor=%08x lead=%08x partner=%08x mode=%u demo=%u control=%x port=%u stick=%.2f,%.2f pos=%.3f,%.3f aim=%.2f,%.2f buttons=%08x hp=%u/%u\n",
            seat + 1, actor, players.lead, players.partner, Memory::Read8(actor + 61), Memory::Read32(stage + 760) & 1,
            Memory::Read32(actor + 352), Memory::Read8(hub + 813 + seat * 88),
            Memory::ReadFloat32(pad + 12), Memory::ReadFloat32(pad + 16),
            Memory::ReadFloat32(actor + 384), Memory::ReadFloat32(actor + 388),
            Memory::ReadFloat32(pad + 20), Memory::ReadFloat32(pad + 24), Memory::Read32(pad + 36),
            Memory::Read32(actor + 696), Memory::Read32(actor + 700));
    }
    if (actor != players.partner || !Memory::Contains(players.lead, 64)) return original;
    const auto input = Memory::Read32(root + 4);
    const bool playable = Memory::Read8(players.lead + 61) == 2 && Memory::Read8(input + 901) != 255 &&
        !(Memory::Read32(stage + 760) & 1) && !(Memory::Read32(players.lead + 352) & 1);
    const bool reserve = Memory::Read8(actor + 61) == 3;
    if (playable && !reserve && borrowed.lead == players.lead && borrowed.partner == actor)
        ShareLife(players, false);
    // ObjHit can report signed overkill. Also honor Player::_dead_chk during
    // scripted deaths that precede a health update. The lead owns the retry.
    const bool controllable = playable && static_cast<int32_t>(Memory::Read32(players.lead + 696)) > 0 &&
        !(Memory::Read32(root + 100) & 8u);
    if (controllable) borrowed = players;
    if (borrowed.lead != players.lead || borrowed.partner != actor) return original;
    VirtualRemote::SetPartnerControllable(controllable && !reserve);
    if (controllable && reserve) ShareLife(players, true);
    if (!controllable) {
        life = {};
        if (reserve) {
            borrowed = {};
            return original;
        }
    }
    // Ask the retail state machine to run its existing change-in or change-out.
    return reserve == controllable ? 128u : 0u;
}

uint32_t SinCoopPad(uint32_t sda, uint32_t actor, uint32_t firstPad) {
    if (!RuntimeConfigFile::EnhancedCoopAtLaunch()) return firstPad;
    const auto root = Memory::Read32(sda - 23904);
    const auto scene = Memory::Read32(root + 96);
    if (!scene || !Memory::Contains(scene, 72)) return firstPad;
    const auto players = ResolvePlayers(root, Memory::Read32(scene + 68));
    return actor && actor == players.partner ? firstPad + 1464 : firstPad;
}

bool SinCoopDecodePad(CpuContext* ctx, uint32_t stack, uint32_t input, uint32_t device) {
    if (!RuntimeConfigFile::EnhancedCoopAtLaunch()) return false;
    CpuContext call = *ctx;
    call.gpr[1] = stack;
    call.gpr[3] = input + 3388;
    call.gpr[4] = device;
    call.gpr[5] = input + 112;
    call.gpr[6] = Memory::Read8(input + 1920);
    InvokeDirectCpu<0x800C5B0Cu>(&call);
    return true;
}

uint32_t SinCoopPollMask(uint32_t original) {
    return RuntimeConfigFile::EnhancedCoopAtLaunch() ? original | 2u : original;
}

uint32_t SinCoopSightCharacter(uint32_t sda, uint32_t owner, uint32_t original) {
    if (!owner || !RuntimeConfigFile::EnhancedCoopAtLaunch()) return original;
    return SinCoopPad(sda, owner, 0) != 0 && Memory::Read8(owner + 61) == 2
        ? Memory::Read8(owner + 60) : original;
}

float SinCoopMoveAxis(uint32_t sda, uint32_t actor, uint32_t pad, uint32_t offset) {
    const float stick = Memory::ReadFloat32(pad + offset);
    if (!RuntimeConfigFile::EnhancedCoopAtLaunch()) return stick;
    const auto root = Memory::Read32(sda - 23904);
    const auto scene = Memory::Read32(root + 96);
    if (!scene || !Memory::Contains(scene, 72)) return stick;
    const auto stage = Memory::Read32(scene + 68);
    const auto players = ResolvePlayers(root, stage);
    const auto other = actor == players.lead ? players.partner : players.lead;
    if (!actor || (actor != players.lead && actor != players.partner) || !other ||
        !Memory::Contains(other, 392) || Memory::Read8(other + 61) != 2 ||
        Memory::Read8(actor + 61) != 2 || (Memory::Read32(stage + 760) & 1)) return stick;
    // A steering bias preserves native bounds and can be overcome by either player.
    const float dx = Memory::ReadFloat32(actor + 384) - Memory::ReadFloat32(other + 384);
    const float dy = Memory::ReadFloat32(actor + 388) - Memory::ReadFloat32(other + 388);
    constexpr float radius = 4.0f;
    const float distance = std::hypot(dx, dy);
    if (!(distance < radius)) return stick;
    const float side = actor == players.lead ? -1.f : 1.f;
    const float direction = distance < 0.001f ? (offset == 12 ? side : 0.f) :
        (offset == 12 ? dx : dy) / distance;
    return std::clamp(stick + direction * 0.14f * (1.f - distance / radius), -1.f, 1.f);
}

void SinCoopFinishStageSeven(CpuContext* ctx) {
    if (!RuntimeConfigFile::EnhancedCoopAtLaunch()) return;
    const auto root = Memory::Read32(ctx->gpr[13] - 23904);
    const auto scene = Memory::Read32(root + 96);
    const auto stage = Memory::Read32(scene + 68);
    const auto players = ResolvePlayers(root, stage);
    if (!players.lead || players.partner) return;
    CpuContext call = *ctx;
    call.gpr[3] = 1264; // Player025, as allocated by TcStg07::cbCreatePlayer
    InvokeDirectCpu<0x8002C7BCu>(&call);
    if (!call.gpr[3]) return;
    struct ConstructionScope {
        ~ConstructionScope() { finaleConstruction = 0; }
    } scope;
    finaleConstruction = call.gpr[3];
    InvokeDirectCpu<0x97E32D3Cu>(&call);
}

uint32_t SinCoopFinaleCharacter(uint32_t actor, uint32_t storyCharacter) {
    return actor && actor == finaleConstruction ? storyCharacter ^ 1u : storyCharacter;
}

void SinCoopEndActor(uint32_t actor) {
    if (actor && (actor == borrowed.partner || actor == borrowed.lead)) {
        life = {};
        borrowed = {};
        VirtualRemote::SetPartnerControllable(false);
    }
}

// Seed only the isolated autostage fixture, on the guest thread before the
// factory chooses a character. A stage task's own +68 is StgGlobals.
void SinCoopSeedTestCharacter(CpuContext* ctx) {
    static const int choice = [] {
        const char* c = std::getenv("SIN_AUTOSTAGE_CHARACTER");
        if (!std::getenv("SIN_AUTOSTAGE") || !c || !c[0] || c[1]) return -1;
        return c[0] == '1' ? 1 : c[0] == '0' ? 0 : -1;
    }();
    if (choice < 0) return;
    const auto root = Memory::Read32(ctx->gpr[13] - 23904);
    const auto scene = Memory::Read32(root + 96);
    const auto parent = Memory::Read32(scene + 36);
    if (!parent || parent == scene) return;
    const auto globals = Memory::Read32(parent + 68);
    if (globals && globals != Memory::Read32(scene + 68) && Memory::Contains(globals, 10)) {
        Memory::Write8(globals + 9, static_cast<uint8_t>(choice));
    }
}
