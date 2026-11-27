#include "hle/scene_lighting.h"
#include "guest_abi/cpu_context.h"
#include "memory.h"
#include "guest_abi/guest_region.h"
#include <aurora/gfx.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

// Guest events and VI run on the emulation thread. Aurora snapshots a synchronized
// view-space packet for draws, so render workers never dereference guest memory.
namespace {
using ScenePoint = std::array<float, 3>;
struct SceneAnchor {
    ScenePoint position{}, normal{};
    float radius = 0;
    unsigned life = 0;
};
SceneAnchor explosion, enemyShadow;
ScenePoint settledFlash{};
float flashLevel = 0.f;
float guestFloat(uint32_t addr) { return std::bit_cast<float>(Memory::Read32(addr)); }
bool validPoint(const ScenePoint& point) {
    return std::all_of(point.begin(), point.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 1e7f; });
}
} // namespace

bool ObserveSceneAnchor(uint32_t address, CpuContext* ctx) {
    // Hook addresses and caller return sites are R2VE01 addresses.
    if (address == MKW_GADDR(80258A60)) {  // nw4r::g3d::ScnObj::SetMtx
        // Return sites of the SetMtx calls in NWSM_Shadow::put and the two putShadow pairs.
        const uint32_t lr = ctx->lr;
        if (!(lr == MKW_GADDR(8009C2B4) || lr == MKW_GADDR(8009C3E8) ||
              lr == MKW_GADDR(8009D7F0) || lr == MKW_GADDR(8009D924) ||
              lr == MKW_GADDR(8009F018) || lr == MKW_GADDR(8009F14C)) || !(aurora_scene_effect_mask() & 4)) return true;
        const uint32_t m = ctx->gpr[5];
        if (!Memory::Contains(m, 48)) return true;
        ScenePoint pos{}, up{}, x{};
        for (unsigned i = 0; i < 3; ++i) {
            pos[i] = guestFloat(m + 16*i + 12);
            up[i] = guestFloat(m + 16*i + 4);
            x[i] = guestFloat(m + 16*i);
        }
        const float len = std::hypot(up[0],up[1],up[2]);
        const float radius = std::clamp(std::hypot(x[0],x[1],x[2])*8.f, 2.f, 65.f);
        if (!validPoint(pos) || !validPoint(up) || !validPoint(x) || len < 0.0001f) return true;
        if (enemyShadow.life < 2 || radius > enemyShadow.radius) {
            for (auto& v : up) v /= len;
            enemyShadow = {pos, up, radius, 2};
        }
        return true;
    }
    unsigned positionArg = 3, lifetime = 12;
    if (address == MKW_GADDR(8006D35C)) {  // PlShot::cbHitBg
        lifetime = 5;
        positionArg = 4;
    } else if (address == MKW_GADDR(8006DA00) ||  // PlChargeShot::cbHitBg
               address == MKW_GADDR(8006E4D4) ||  // PlCounterAttack2::cbHitBg
               address == MKW_GADDR(80123220)) {  // setMBomb
        positionArg = 4;
    } else if (address == MKW_GADDR(8013C148) ||  // BombMSet
               address == MKW_GADDR(8013C358) ||  // BombMAlphaSet
               address == MKW_GADDR(8013C5FC)) {  // MechaBombMSet
    } else {
        return false;
    }
    const uint32_t p = ctx->gpr[positionArg];
    if ((aurora_scene_effect_mask() & 2) && lifetime >= explosion.life && Memory::Contains(p, 12)) {
        ScenePoint pos{guestFloat(p),guestFloat(p+4),guestFloat(p+8)};
        if (validPoint(pos)) explosion = {pos, {}, 0, lifetime};
    }
    return true;
}

namespace {
uint32_t activeCamera() {
    auto pointerAt = [](uint32_t base, unsigned offset) -> uint32_t {
        return base && Memory::Contains(base, offset + 4) ? Memory::Read32(base + offset) : 0;
    };
    return pointerAt(pointerAt(Memory::Read32(MKW_GADDR(804B19A0)),88),448);
}

// The ray tracer asks for the camera while GX commands are recorded, which
// runs on the GX worker. Guest memory is only read on the emulation thread
// (UpdateSceneLighting at retrace); the worker reads this snapshot.
std::mutex rtCameraMutex;
std::array<float, 12> rtCameraView{};
uint32_t rtCameraScene = 0;
bool rtCameraValid = false;

void snapshotRtCamera() {
    std::array<float, 12> view{};
    bool valid = false;
    const auto camera = activeCamera();
    if (camera && Memory::Contains(camera, 176)) {
        valid = true;
        for (unsigned i = 0; i < 12; ++i) {
            view[i] = guestFloat(camera + 128 + 4*i);
            valid &= std::isfinite(view[i]) && std::abs(view[i]) <= 1.e7f;
        }
    }
    const uint32_t scene = Memory::Read8(MKW_GADDR(804B1968));
    std::lock_guard lock(rtCameraMutex);
    rtCameraView = view;
    rtCameraScene = scene;
    rtCameraValid = valid;
}

bool readRtCamera(float* view, uint32_t* scene) {
    std::lock_guard lock(rtCameraMutex);
    if (!rtCameraValid) return false;
    std::copy(rtCameraView.begin(), rtCameraView.end(), view);
    *scene = rtCameraScene;
    return true;
}
} // namespace

void UpdateSceneLighting(uint8_t stage) {
    static const bool registered = [] { aurora_set_rt_camera_provider(readRtCamera); return true; }();
    (void)registered;
    snapshotRtCamera();
    static int lastStage = -1;
    static unsigned previousMask = 0;
    const unsigned mask = aurora_scene_effect_mask();
    if (lastStage != stage || !(mask & 2)) {
        explosion = {};
        flashLevel = 0.f;
    }
    if (lastStage != stage || !(mask & 4)) enemyShadow = {};
    lastStage = stage;
    if (!mask && !previousMask) return;
    previousMask = mask;
    const float target = 0.036f * explosion.life;
    if (flashLevel < 0.012f) settledFlash = explosion.position;
    for (unsigned i = 0; i < 3; ++i) {
        if (explosion.life)
            settledFlash[i] += 0.22f * (explosion.position[i] - settledFlash[i]);
    }
    flashLevel += (target > flashLevel ? 0.42f : 0.17f) * (target - flashLevel);
    if (flashLevel < 0.002f) flashLevel = 0.f;
    AuroraSceneLight packet{};
    const auto camera = activeCamera();
    if (mask && camera && Memory::Contains(camera,176)) {
        bool valid = true;
        for (unsigned i = 0; i < 3; ++i) {
            const uint32_t row = camera + 128 + 16*i;
            ScenePoint axis{guestFloat(row), guestFloat(row+4), guestFloat(row+8)};
            const float offset = guestFloat(row+12);
            const float norm = std::hypot(axis[0],axis[1],axis[2]);
            valid &= validPoint(axis) && std::isfinite(offset) && norm > 0.9f && norm < 1.1f;
            const auto dot = [&](const ScenePoint& p) { return axis[0]*p[0]+axis[1]*p[1]+axis[2]*p[2]; };
            packet.cameraUp[i] = axis[1];
            packet.combat[i] = dot(settledFlash) + offset;
            packet.shadow[i] = dot(enemyShadow.position) + offset;
            packet.shadowNormal[i] = dot(enemyShadow.normal);
        }
        packet.cameraUp[3] = stage == 2 ? 1.f : 0.f;
        packet.combat[3] = flashLevel;
        packet.shadow[3] = enemyShadow.life ? 0.27f : 0.f;
        packet.shadowNormal[3] = 1.f / std::max(1.f,enemyShadow.radius * enemyShadow.radius);
        if (!valid) packet = {};
        static const bool trace = std::getenv("SIN_RENDER_PROF") != nullptr;
        static unsigned reported = 0;
        const unsigned active = valid ? (1u | (packet.combat[3] > 0 ? 2u : 0u) | (packet.shadow[3] > 0 ? 4u : 0u)) : 0u;
        if (trace && (active & ~reported)) {
            std::fprintf(stderr, "[scene-light] stage=%u camera=%d explosion=%.2f shadow=%.2f\n",
                         unsigned(stage), int(valid), packet.combat[3], packet.shadow[3]);
            reported |= active;
        }
    }
    aurora_update_scene_light(&packet);
    if (explosion.life) --explosion.life;
    if (enemyShadow.life) --enemyShadow.life;
}
