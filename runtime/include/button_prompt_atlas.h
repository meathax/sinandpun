#pragma once
#include "aim_settings.h"
#include "button_prompt_pixels.h"
#include <array>
#include <cstdint>

namespace ButtonPrompts {
using Atlas = std::array<uint8_t, 256 * 256 * 4>;
inline int BindingGlyph(const InputMap::Binding& b) {
    if (b.device != InputMap::Device::Pad) return 20;
    if (b.code >= 0 && b.code <= 14) return b.code;
    if (b.code == 20) return 21;
    if (b.code >= 15 && b.code < InputMap::kPadAxisBase) return 22;
    switch (b.code - InputMap::kPadAxisBase) {
    case 0: case 1: return 17;
    case 2: case 3: return 18;
    case 4: return 15;
    case 5: return 16;
    default: return 20;
    }
}

inline int ActionGlyph(const AimSettings::Settings& s, AimSettings::Action a) {
    for (const auto& b : s.bindings.pad[static_cast<size_t>(a)]) {
        if (b.Valid()) return BindingGlyph(b);
    }
    return 20;
}

// BTN SPRX has 32 frames: indices 0..24 occupy five columns, with
// Classic L/R/ZL/ZR/L-stick/R-stick/Start in the two rightmost columns.
inline constexpr std::array<int, 32> kCells = {
    0,1,2,3,4,8,9,10,11,12,16,17,18,19,20,24,25,26,27,28,
    32,33,34,35,36,5,13,6,14,29,21,22
};

inline void BuildAtlas(const AimSettings::Settings& s, Atlas& out) {
    using A = AimSettings::Action;
    const auto action = [&](A a) { return ActionGlyph(s,a); };
    const std::array<int,32> icons = {
        action(A::Confirm),action(A::Cancel),action(A::Jump),action(A::Evade),17,
        action(A::Pause),action(A::Minus),action(A::ButtonX),action(A::ButtonY),action(A::Confirm),
        action(A::Cancel),action(A::ButtonX),action(A::ButtonY),action(A::Evade),action(A::Jump),
        action(A::Fire),17,18,19,12,
        action(A::Pause),action(A::ButtonY),action(A::ButtonX),action(A::Confirm),action(A::Cancel),
        action(A::Jump),action(A::Fire),action(A::Evade),action(A::LockOn),17,18,action(A::Pause)
    };
    out.fill(0);
    for (int frame=0;frame<32;++frame) {
        const int cell=kCells[frame], ox=(cell%8)*32, oy=(cell/8)*32;
        // Remote A/B each have two host actions (menu and combat). Show both
        // bindings rather than incorrectly claiming that one button does both.
        const int second=frame==0?action(A::LockOn):frame==1?action(A::Fire):-1;
        const bool pair=second>=0 && second!=icons[frame];
        for (int y=0;y<32;++y) for (int x=0;x<32;++x) {
            int icon=icons[frame],sx=x,sy=y;
            if (pair) {
                if (y<8 || y>=24) continue;
                if (x>=16) icon=second;
                sx=(x%16)*2;sy=(y-8)*2;
            }
            const auto* rgba=&kGlyphPixels[s.playStationPrompts?1:0][icon][(sy*32+sx)*4];
            const int px=ox+x,py=oy+y;
            const size_t tile=((py/4)*64+px/4)*64;
            const size_t sample=((py%4)*4+px%4)*2;
            out[tile+sample]=rgba[3];out[tile+sample+1]=rgba[0];
            out[tile+32+sample]=rgba[1];out[tile+32+sample+1]=rgba[2];
        }
    }
}
}
