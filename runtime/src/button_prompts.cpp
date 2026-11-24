#include "button_prompts.h"
#include "button_prompt_atlas.h"
#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <limits>

namespace ButtonPrompts {
static std::atomic_bool combatPrompts{false};
void SetCombat(bool active) { combatPrompts.store(active, std::memory_order_relaxed); }

bool Update() {
    // GX-worker-owned storage: the raw registration borrows the pixel bytes.
    static uint32_t generation=std::numeric_limits<uint32_t>::max();
    static std::unique_ptr<Atlas> pixels;
    static std::vector<uint8_t> hdPixels;  // borrowed by the registration; replaced only after re-registering
    static aurora::texture::ReplacementRegistration registration;
    const uint32_t next=AimSettings::Generation();
    static bool previousCombat = false;
    const bool combat = combatPrompts.load(std::memory_order_relaxed);
    if (next==generation && previousCombat==combat) return false;
    previousCombat = combat;
    auto candidate=std::make_unique<Atlas>();
    BuildAtlas(AimSettings::Get(), *candidate, combat);
    static const bool trace = std::getenv("SIN_PROMPT_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "[prompts] context=%s generation=%u\n", combat ? "combat" : "menu", next);
    generation=next;
    if (pixels && *pixels==*candidate) return false;
    // Exact R2VE01 texture.arc/BTN/TEX0 base-level identity. No pointer or
    // dimensions-only matching: unrelated 256x256 textures remain untouched.
    const aurora::texture::TextureSourceKey key{
        .textureHash=0x7d9cbe2a23379258ull,.width=256,.height=256,.format=6
    };
    // 4x atlas (1024x1024 + mips) so prompts stay sharp at high output resolutions; same frames and UVs.
    std::vector<uint8_t> hd;
    BuildAtlasHD(AimSettings::Get(), hd, combat);
    const auto fresh=aurora::texture::register_replacement(key,
        {.bytes=hd,.width=kHdAtlasSize,.height=kHdAtlasSize,.mipCount=kHdAtlasMips,.gxFormat=GX_TF_RGBA8_PC,
         .label="Sin modern button prompts (4x)"},
        {.priority=100});
    aurora::texture::unregister_replacement(registration);
    registration=fresh;
    hdPixels=std::move(hd);
    pixels=std::move(candidate);
    return true;
}
}
