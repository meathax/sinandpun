#include "button_prompts.h"
#include "button_prompt_atlas.h"
#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include <memory>
#include <limits>

namespace ButtonPrompts {
bool Update() {
    // GX-worker-owned storage: the raw registration borrows the pixel bytes.
    static uint32_t generation=std::numeric_limits<uint32_t>::max();
    static std::unique_ptr<Atlas> pixels;
    static aurora::texture::ReplacementRegistration registration;
    const uint32_t next=AimSettings::Generation();
    if (next==generation) return false;
    auto candidate=std::make_unique<Atlas>();
    BuildAtlas(AimSettings::Get(), *candidate);
    generation=next;
    if (pixels && *pixels==*candidate) return false;
    // Exact R2VE01 texture.arc/BTN/TEX0 base-level identity. No pointer or
    // dimensions-only matching: unrelated 256x256 textures remain untouched.
    const aurora::texture::TextureSourceKey key{
        .textureHash=0x7d9cbe2a23379258ull,.width=256,.height=256,.format=6
    };
    const auto fresh=aurora::texture::register_replacement(key,
        {.bytes=*candidate,.width=256,.height=256,.mipCount=1,.gxFormat=6,.label="Sin modern button prompts"},
        {.priority=100});
    aurora::texture::unregister_replacement(registration);
    registration=fresh;
    pixels=std::move(candidate);
    return true;
}
}
