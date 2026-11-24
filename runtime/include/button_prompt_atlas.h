#pragma once
#include "aim_settings.h"
#include "button_prompt_pixels.h"
#include "button_prompt_pixels_hd.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

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

inline void BuildAtlas(const AimSettings::Settings& s, Atlas& out, bool combat = false) {
    using A = AimSettings::Action;
    const auto action = [&](A a) { return ActionGlyph(s,a); };
    const std::array<int,32> icons = {
        action(combat ? A::LockOn : A::Confirm),action(combat ? A::Fire : A::Cancel),action(A::Jump),action(A::Evade),17,
        action(A::Pause),action(A::Minus),action(A::ButtonX),action(A::ButtonY),action(A::Confirm),
        action(A::Cancel),action(A::ButtonX),action(A::ButtonY),action(A::Evade),action(A::Jump),
        action(A::Fire),17,18,19,12,
        action(A::Pause),action(A::ButtonY),action(A::ButtonX),action(A::Confirm),action(A::Cancel),
        action(A::Jump),action(A::Fire),action(A::Evade),action(A::LockOn),17,18,action(A::Pause)
    };
    out.fill(0);
    for (int frame=0;frame<32;++frame) {
        const int cell=kCells[frame], ox=(cell%8)*32, oy=(cell/8)*32;
        const int icon = icons[frame];
        for (int y=0;y<32;++y) for (int x=0;x<32;++x) {
            const auto* rgba=&kGlyphPixels[s.playStationPrompts?1:0][icon][(y*32+x)*4];
            const int px=ox+x,py=oy+y;
            const size_t tile=((py/4)*64+px/4)*64;
            const size_t sample=((py%4)*4+px%4)*2;
            out[tile+sample]=rgba[3];out[tile+sample+1]=rgba[0];
            out[tile+32+sample]=rgba[1];out[tile+32+sample+1]=rgba[2];
        }
    }
}

// High-resolution variant: the same 32 frames at 4x (128 px glyphs from kGlyphHdRuns) in a linear
// 1024x1024 RGBA8 image followed by its box-filtered mip chain down to 64x64 (5 levels).
inline constexpr int kHdAtlasSize = 1024;
inline constexpr int kHdAtlasMips = 5;
inline void DecodeHdGlyph(int style, int icon, uint8_t* out /* 128*128*4 */) {
    const int g = style * 23 + icon;
    const uint8_t* p = kGlyphHdRuns + kGlyphHdOffsets[g];
    const uint8_t* end = kGlyphHdRuns + kGlyphHdOffsets[g + 1];
    size_t o = 0;
    while (p < end && o < size_t(kGlyphHdSize) * kGlyphHdSize * 4) {
        const int n = p[0];
        for (int k = 0; k < n; ++k) { out[o] = p[1]; out[o + 1] = p[2]; out[o + 2] = p[3]; out[o + 3] = p[4]; o += 4; }
        p += 5;
    }
}
inline void BuildAtlasHD(const AimSettings::Settings& s, std::vector<uint8_t>& out, bool combat = false) {
    using A = AimSettings::Action;
    const auto action = [&](A a) { return ActionGlyph(s,a); };
    const std::array<int,32> icons = {
        action(combat ? A::LockOn : A::Confirm),action(combat ? A::Fire : A::Cancel),action(A::Jump),action(A::Evade),17,
        action(A::Pause),action(A::Minus),action(A::ButtonX),action(A::ButtonY),action(A::Confirm),
        action(A::Cancel),action(A::ButtonX),action(A::ButtonY),action(A::Evade),action(A::Jump),
        action(A::Fire),17,18,19,12,
        action(A::Pause),action(A::ButtonY),action(A::ButtonX),action(A::Confirm),action(A::Cancel),
        action(A::Jump),action(A::Fire),action(A::Evade),action(A::LockOn),17,18,action(A::Pause)
    };
    constexpr int N = kHdAtlasSize, G = kGlyphHdSize;
    size_t total = 0;
    for (int m = 0; m < kHdAtlasMips; ++m) total += size_t(N >> m) * (N >> m) * 4;
    out.assign(total, 0);
    std::vector<uint8_t> glyph(size_t(G) * G * 4);
    const int style = s.playStationPrompts ? 1 : 0;
    for (int frame = 0; frame < 32; ++frame) {
        const int cell = kCells[frame], ox = (cell % 8) * G, oy = (cell / 8) * G;
        DecodeHdGlyph(style, icons[frame], glyph.data());
        for (int y = 0; y < G; ++y)
            for (int x = 0; x < G; ++x)
                for (int c = 0; c < 4; ++c)
                    out[(size_t(oy + y) * N + ox + x) * 4 + c] = glyph[(size_t(y) * G + x) * 4 + c];
    }
    // Mips: premultiplied 2x2 box filter so transparent texels never darken edges.
    size_t src = 0;
    for (int m = 1; m < kHdAtlasMips; ++m) {
        const int sw = N >> (m - 1), dw = N >> m;
        const size_t dst = src + size_t(sw) * sw * 4;
        for (int y = 0; y < dw; ++y)
            for (int x = 0; x < dw; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const uint8_t* q = &out[src + (size_t(2 * y + dy) * sw + 2 * x + dx) * 4];
                        const float a = q[3] / 255.f;
                        acc[0] += q[0] * a; acc[1] += q[1] * a; acc[2] += q[2] * a; acc[3] += q[3];
                    }
                uint8_t* d = &out[dst + (size_t(y) * dw + x) * 4];
                const float a = acc[3] / 4.f;
                for (int c = 0; c < 3; ++c) d[c] = uint8_t(a > 0.5f ? std::min(255.f, acc[c] / (a / 255.f) / 4.f + 0.5f) : 0);
                d[3] = uint8_t(a + 0.5f);
            }
        src = dst;
    }
}
}
