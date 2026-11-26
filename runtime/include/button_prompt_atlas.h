#pragma once
#include "aim_settings.h"
#include "button_prompt_pixels.h"
#include "button_prompt_pixels_hd.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
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

// High-resolution variant: the same 32 frames at 4x (128 px glyphs from kGlyphHdRuns) in a linear
// 1024x1024 RGBA8 image followed by its box-filtered mip chain down to 64x64 (5 levels).
inline constexpr int kHdAtlasSize = 1024;
inline constexpr int kHdAtlasMips = 5;

namespace kbm_detail {

enum class GlyphKind : uint8_t {
    Keycap,
    MouseBtn,
    MouseAim,
    MoveCluster,
    DpadCluster,
};

struct GlyphDesc {
    GlyphKind kind = GlyphKind::Keycap;
    int32_t mouseCode = 0;
    std::string label;
    bool operator==(const GlyphDesc&) const = default;
};

inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

inline float SmoothStep(float e0, float e1, float x) {
    const float t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

inline void BlendOver(uint8_t* dst, float r, float g, float b, float a) {
    a = Clamp01(a);
    if (a <= 0.001f) return;
    const float da = dst[3] / 255.f;
    const float oa = a + da * (1.f - a);
    if (oa <= 0.001f) return;
    const float inv = 1.f / oa;
    const float rr = (r * a + (dst[0] / 255.f) * da * (1.f - a)) * inv;
    const float gg = (g * a + (dst[1] / 255.f) * da * (1.f - a)) * inv;
    const float bb = (b * a + (dst[2] / 255.f) * da * (1.f - a)) * inv;
    dst[0] = static_cast<uint8_t>(std::min(255.f, rr * 255.f + 0.5f));
    dst[1] = static_cast<uint8_t>(std::min(255.f, gg * 255.f + 0.5f));
    dst[2] = static_cast<uint8_t>(std::min(255.f, bb * 255.f + 0.5f));
    dst[3] = static_cast<uint8_t>(std::min(255.f, oa * 255.f + 0.5f));
}

inline float SdRoundedBox(float px, float py, float cx, float cy, float hx, float hy, float r) {
    const float qx = std::fabs(px - cx) - (hx - r);
    const float qy = std::fabs(py - cy) - (hy - r);
    const float ax = std::max(qx, 0.f);
    const float ay = std::max(qy, 0.f);
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.f) - r;
}

inline float SdSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float pax = px - ax, pay = py - ay;
    const float bax = bx - ax, bay = by - ay;
    const float denom = bax * bax + bay * bay;
    const float h = denom > 1e-6f ? Clamp01((pax * bax + pay * bay) / denom) : 0.f;
    const float dx = pax - bax * h, dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

inline const uint8_t* Font5x7(char c) {
    static constexpr uint8_t kBlank[7] = {0, 0, 0, 0, 0, 0, 0};
    static constexpr uint8_t kLetters[26][7] = {
        {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // A
        {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // B
        {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // C
        {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, // D
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
        {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}, // G
        {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
        {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // I
        {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}, // J
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
        {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
        {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
        {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // Q
        {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
        {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, // S
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // U
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // V
        {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}, // W
        {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // X
        {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, // Y
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, // Z
    };
    static constexpr uint8_t kDigits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
        {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}, // 2
        {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, // 3
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
        {0x0E, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x0E}, // 6
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}, // 9
    };
    static constexpr uint8_t kMinus[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static constexpr uint8_t kPlus[7]  = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
    static constexpr uint8_t kSlash[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
    static constexpr uint8_t kDot[7]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    static constexpr uint8_t kComma[7] = {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08};
    static constexpr uint8_t kStar[7]  = {0x00, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x00};
    static constexpr uint8_t kEq[7]    = {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00};
    static constexpr uint8_t kLBrk[7]  = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E};
    static constexpr uint8_t kRBrk[7]  = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E};
    static constexpr uint8_t kSemi[7]  = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x08};
    static constexpr uint8_t kQuot[7]  = {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t kBSl[7]   = {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01};
    static constexpr uint8_t kTick[7]  = {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t kUp[7]    = {0x04, 0x0E, 0x15, 0x04, 0x04, 0x04, 0x04};
    static constexpr uint8_t kDown[7]  = {0x04, 0x04, 0x04, 0x04, 0x15, 0x0E, 0x04};
    static constexpr uint8_t kLeft[7]  = {0x00, 0x04, 0x08, 0x1F, 0x08, 0x04, 0x00};
    static constexpr uint8_t kRight[7] = {0x00, 0x04, 0x02, 0x1F, 0x02, 0x04, 0x00};
    if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    switch (c) {
    case '-': return kMinus;
    case '+': return kPlus;
    case '/': return kSlash;
    case '.': return kDot;
    case ',': return kComma;
    case '*': return kStar;
    case '=': return kEq;
    case '[': return kLBrk;
    case ']': return kRBrk;
    case ';': return kSemi;
    case '\'': return kQuot;
    case '\\': return kBSl;
    case '`': return kTick;
    case '^': return kUp;
    case 'v': return kDown;
    case '<': return kLeft;
    case '>': return kRight;
    default: return kBlank;
    }
}

inline bool FontBit(const uint8_t* rows, int gx, int gy) {
    if (gx < 0 || gx >= 5 || gy < 0 || gy >= 7) return false;
    return (rows[gy] & (1u << (4 - gx))) != 0;
}

// Smooth connected-stroke SDF in 5x7 glyph grid space for crisp anti-aliased lettering at 128x128.
inline float SampleGlyphSdf(const uint8_t* rows, float fx, float fy) {
    const int gx = static_cast<int>(std::floor(fx));
    const int gy = static_cast<int>(std::floor(fy));
    float best = 10.f;
    for (int cy = gy - 1; cy <= gy + 1; ++cy) {
        for (int cx = gx - 1; cx <= gx + 1; ++cx) {
            if (!FontBit(rows, cx, cy)) continue;
            const float x0 = cx + 0.5f, y0 = cy + 0.5f;
            const float dx = fx - x0, dy = fy - y0;
            best = std::min(best, std::sqrt(dx * dx + dy * dy));
            if (FontBit(rows, cx + 1, cy))
                best = std::min(best, SdSegment(fx, fy, x0, y0, x0 + 1.f, y0));
            if (FontBit(rows, cx, cy + 1))
                best = std::min(best, SdSegment(fx, fy, x0, y0, x0, y0 + 1.f));
            if (FontBit(rows, cx + 1, cy + 1) && !FontBit(rows, cx + 1, cy) && !FontBit(rows, cx, cy + 1))
                best = std::min(best, SdSegment(fx, fy, x0, y0, x0 + 1.f, y0 + 1.f));
            if (FontBit(rows, cx - 1, cy + 1) && !FontBit(rows, cx - 1, cy) && !FontBit(rows, cx, cy + 1))
                best = std::min(best, SdSegment(fx, fy, x0, y0, x0 - 1.f, y0 + 1.f));
        }
    }
    return best;
}

inline void DrawTextCentered(uint8_t* buf, int size, float cx, float cy, std::string_view text,
                             float cellPx, float r, float g, float b, float a = 1.f, bool shadow = true) {
    if (text.empty()) return;
    const float adv = 6.2f * cellPx;
    const float glyphW = 5.f * cellPx;
    const float glyphH = 7.f * cellPx;
    const float totalW = static_cast<float>(text.size() - 1) * adv + glyphW;
    const float startX = cx - totalW * 0.5f;
    const float startY = cy - glyphH * 0.5f;
    const float strokeR = 0.45f;
    const float aa = std::max(0.12f, 0.85f / std::max(1.f, cellPx));
    const float shadowOff = std::max(1.f, cellPx * 0.35f);

    for (size_t i = 0; i < text.size(); ++i) {
        const uint8_t* rows = Font5x7(text[i]);
        const float ox = startX + static_cast<float>(i) * adv;
        const int x0 = std::max(0, static_cast<int>(std::floor(ox - cellPx)));
        const int x1 = std::min(size - 1, static_cast<int>(std::ceil(ox + glyphW + cellPx)));
        const int y0 = std::max(0, static_cast<int>(std::floor(startY - cellPx)));
        const int y1 = std::min(size - 1, static_cast<int>(std::ceil(startY + glyphH + cellPx + shadowOff)));
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                uint8_t* dst = &buf[(size_t(py) * size + px) * 4];
                if (shadow) {
                    const float sfx = (px + 0.5f - ox) / cellPx;
                    const float sfy = (py + 0.5f - shadowOff - startY) / cellPx;
                    const float sd = SampleGlyphSdf(rows, sfx, sfy);
                    const float sa = (1.f - SmoothStep(strokeR - aa, strokeR + aa * 1.6f, sd)) * 0.65f * a;
                    if (sa > 0.001f) BlendOver(dst, 0.02f, 0.04f, 0.08f, sa);
                }
                const float fx = (px + 0.5f - ox) / cellPx;
                const float fy = (py + 0.5f - startY) / cellPx;
                const float d = SampleGlyphSdf(rows, fx, fy);
                const float cov = (1.f - SmoothStep(strokeR - aa, strokeR + aa, d)) * a;
                if (cov > 0.001f) BlendOver(dst, r, g, b, cov);
            }
        }
    }
}

inline void DrawKeycapBox(uint8_t* buf, int size, float cx, float cy, float hx, float hy, float radius,
                          float rimPx, float skirtPx) {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - hx - 8.f)));
    const int x1 = std::min(size - 1, static_cast<int>(std::ceil(cx + hx + 8.f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - hy - 6.f)));
    const int y1 = std::min(size - 1, static_cast<int>(std::ceil(cy + hy + 10.f)));
    const float topCy = cy - skirtPx * 0.35f;
    const float topHy = hy - skirtPx * 0.55f;
    const float topHx = hx - rimPx * 1.15f;
    const float topR = std::max(2.f, radius - rimPx);

    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            const float fx = px + 0.5f, fy = py + 0.5f;
            uint8_t* dst = &buf[(size_t(py) * size + px) * 4];

            // Soft drop shadow + dark outer stroke so the keycap reads over bright or dark HUDs.
            const float dShadow = SdRoundedBox(fx, fy, cx, cy + 3.0f, hx + 2.5f, hy + 2.5f, radius + 2.5f);
            const float aShadow = (1.f - SmoothStep(-1.f, 4.5f, dShadow)) * 0.58f;
            if (aShadow > 0.001f) BlendOver(dst, 0.02f, 0.03f, 0.06f, aShadow);

            const float dOutline = SdRoundedBox(fx, fy, cx, cy, hx + 2.0f, hy + 2.0f, radius + 2.0f);
            const float aOutline = (1.f - SmoothStep(-0.75f, 0.75f, dOutline)) * 0.92f;
            if (aOutline > 0.001f) BlendOver(dst, 0.04f, 0.06f, 0.11f, aOutline);

            // Keycap skirt / outer housing.
            const float dBase = SdRoundedBox(fx, fy, cx, cy, hx, hy, radius);
            const float aBase = 1.f - SmoothStep(-0.75f, 0.75f, dBase);
            if (aBase > 0.001f) {
                const float t = Clamp01((fy - (cy - hy)) / (2.f * hy));
                const float br = 0.15f - 0.06f * t;
                const float bg = 0.18f - 0.07f * t;
                const float bb = 0.25f - 0.09f * t;
                BlendOver(dst, br, bg, bb, aBase);

                // Crisp bright outer rim highlight.
                const float rimMask = SmoothStep(-rimPx - 0.6f, -rimPx + 0.4f, dBase) * aBase;
                const float topBias = 1.f - SmoothStep(0.f, 0.65f, t);
                BlendOver(dst, 0.80f + 0.16f * topBias, 0.87f + 0.11f * topBias, 0.96f,
                          rimMask * (0.48f + 0.44f * topBias));
            }

            // Inner keycap top face with subtle sheen.
            const float dTop = SdRoundedBox(fx, fy, cx, topCy, topHx, topHy, topR);
            const float aTop = 1.f - SmoothStep(-0.75f, 0.75f, dTop);
            if (aTop > 0.001f) {
                const float t = Clamp01((fy - (topCy - topHy)) / (2.f * topHy));
                const float tr = 0.24f - 0.09f * t;
                const float tg = 0.29f - 0.10f * t;
                const float tb = 0.37f - 0.12f * t;
                BlendOver(dst, tr, tg, tb, aTop);

                // Subtle top inner bevel line.
                const float innerBevel = SmoothStep(-2.2f, -0.4f, dTop) * (1.f - SmoothStep(0.f, 0.45f, t)) * aTop;
                BlendOver(dst, 0.88f, 0.94f, 1.0f, innerBevel * 0.42f);
            }
        }
    }
}

inline void RenderKeycap(uint8_t* buf, int size, std::string_view label) {
    const size_t len = label.size();
    const float hx = len <= 1 ? 47.f : (len == 2 ? 52.f : 56.f);
    const float hy = len <= 1 ? 47.f : 44.f;
    const float skirt = 7.f;
    DrawKeycapBox(buf, size, 64.f, 65.f, hx, hy, 15.f, 3.0f, skirt);

    float cellPx = 8.4f;
    if (len == 2) cellPx = 6.2f;
    else if (len == 3) cellPx = 4.6f;
    else if (len >= 4) cellPx = 3.65f;
    DrawTextCentered(buf, size, 64.f, 65.f - skirt * 0.35f, label, cellPx, 0.97f, 0.99f, 1.0f, 1.f, true);
}

inline void RenderMouse(uint8_t* buf, int size, int32_t activeCode, bool aimMode) {
    const float cx = 64.f, cy = 64.f;
    const float hx = 34.f, hy = 50.f, r = 30.f;
    const float splitY = cy - 6.f;
    const float wheelCx = cx, wheelCy = cy - 23.f, wheelHx = 5.5f, wheelHy = 11.5f, wheelR = 4.8f;

    // Accent colors for highlighted mouse controls.
    float ar = 0.22f, ag = 0.84f, ab = 1.0f; // LMB cyan
    std::string_view badge;
    if (activeCode == 1) { ar = 0.20f; ag = 0.85f; ab = 1.00f; badge = "LMB"; }
    else if (activeCode == 3) { ar = 1.00f; ag = 0.72f; ab = 0.18f; badge = "RMB"; }
    else if (activeCode == 2) { ar = 0.28f; ag = 0.95f; ab = 0.68f; badge = "MMB"; }
    else if (activeCode == 4) { ar = 1.00f; ag = 0.54f; ab = 0.22f; badge = "M4"; }
    else if (activeCode == 5) { ar = 0.80f; ag = 0.48f; ab = 1.00f; badge = "M5"; }
    else if (activeCode == InputMap::kMouseWheelUp) { ar = 0.28f; ag = 0.95f; ab = 0.68f; badge = "W^"; }
    else if (activeCode == InputMap::kMouseWheelDown) { ar = 0.28f; ag = 0.95f; ab = 0.68f; badge = "Wv"; }

    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            const float fx = px + 0.5f, fy = py + 0.5f;
            uint8_t* dst = &buf[(size_t(py) * size + px) * 4];

            // Drop shadow + dark outer contour.
            const float dShadow = SdRoundedBox(fx, fy, cx, cy + 3.f, hx + 3.f, hy + 3.f, r + 3.f);
            const float aShadow = (1.f - SmoothStep(-1.f, 5.f, dShadow)) * 0.58f;
            if (aShadow > 0.001f) BlendOver(dst, 0.02f, 0.03f, 0.06f, aShadow);

            const float dOut = SdRoundedBox(fx, fy, cx, cy, hx + 2.5f, hy + 2.5f, r + 2.5f);
            const float aOut = (1.f - SmoothStep(-0.8f, 0.8f, dOut)) * 0.94f;
            if (aOut > 0.001f) BlendOver(dst, 0.04f, 0.06f, 0.11f, aOut);

            // Mouse body.
            const float dBody = SdRoundedBox(fx, fy, cx, cy, hx, hy, r);
            const float aBody = 1.f - SmoothStep(-0.8f, 0.8f, dBody);
            if (aBody > 0.001f) {
                const float t = Clamp01((fy - (cy - hy)) / (2.f * hy));
                BlendOver(dst, 0.22f - 0.09f * t, 0.26f - 0.10f * t, 0.34f - 0.12f * t, aBody);

                // Inner body clip for button highlight fills.
                const float aInner = 1.f - SmoothStep(-0.7f, 0.7f, dBody + 3.2f);
                const float dWheelCut = SdRoundedBox(fx, fy, wheelCx, wheelCy, wheelHx + 2.5f, wheelHy + 2.5f, wheelR + 2.f);
                const float outWheel = SmoothStep(-0.6f, 0.6f, dWheelCut);
                if (aInner > 0.001f && fy < splitY - 1.5f) {
                    const float topFade = 1.f - Clamp01((fy - (cy - hy)) / (hy - 6.f)) * 0.28f;
                    if (activeCode == 1 && fx < cx - 1.5f) {
                        const float m = aInner * outWheel * (1.f - SmoothStep(cx - 2.5f, cx - 1.0f, fx))
                                      * (1.f - SmoothStep(splitY - 2.5f, splitY - 1.0f, fy));
                        BlendOver(dst, ar * topFade, ag * topFade, ab * topFade, m);
                    } else if (activeCode == 3 && fx > cx + 1.5f) {
                        const float m = aInner * outWheel * SmoothStep(cx + 1.0f, cx + 2.5f, fx)
                                      * (1.f - SmoothStep(splitY - 2.5f, splitY - 1.0f, fy));
                        BlendOver(dst, ar * topFade, ag * topFade, ab * topFade, m);
                    }
                }

                // Divider lines (horizontal split & vertical L/R split).
                if (aInner > 0.001f) {
                    const float hLine = (1.f - SmoothStep(0.8f, 2.0f, std::fabs(fy - splitY))) * aInner;
                    if (hLine > 0.001f) BlendOver(dst, 0.78f, 0.86f, 0.95f, hLine * 0.78f);
                    if (fy < splitY) {
                        const float vLine = (1.f - SmoothStep(0.8f, 2.0f, std::fabs(fx - cx))) * aInner * outWheel;
                        if (vLine > 0.001f) BlendOver(dst, 0.78f, 0.86f, 0.95f, vLine * 0.78f);
                    }
                }

                // Bright outer shell rim.
                const float rim = SmoothStep(-3.5f, -2.0f, dBody) * aBody;
                BlendOver(dst, 0.86f, 0.92f, 0.99f, rim * 0.90f);
            }

            // Side thumb buttons (M4 lower-left, M5 upper-left) when active.
            if (activeCode == 4 || activeCode == 5) {
                const float bx = cx - hx - 3.5f;
                const float by = activeCode == 5 ? (cy - 10.f) : (cy + 10.f);
                const float dSideOut = SdRoundedBox(fx, fy, bx, by, 5.5f, 10.5f, 4.5f);
                const float aSideOut = 1.f - SmoothStep(-0.7f, 0.7f, dSideOut);
                if (aSideOut > 0.001f) BlendOver(dst, 0.04f, 0.06f, 0.11f, aSideOut);
                const float dSide = SdRoundedBox(fx, fy, bx, by, 3.8f, 8.8f, 3.2f);
                const float aSide = 1.f - SmoothStep(-0.7f, 0.7f, dSide);
                if (aSide > 0.001f) BlendOver(dst, ar, ag, ab, aSide);
            }

            // Scroll wheel.
            const float dWheelOut = SdRoundedBox(fx, fy, wheelCx, wheelCy, wheelHx + 1.8f, wheelHy + 1.8f, wheelR + 1.5f);
            const float aWheelOut = 1.f - SmoothStep(-0.7f, 0.7f, dWheelOut);
            if (aWheelOut > 0.001f) BlendOver(dst, 0.05f, 0.08f, 0.13f, aWheelOut * 0.95f);

            const float dWheel = SdRoundedBox(fx, fy, wheelCx, wheelCy, wheelHx, wheelHy, wheelR);
            const float aWheel = 1.f - SmoothStep(-0.7f, 0.7f, dWheel);
            if (aWheel > 0.001f) {
                const bool litWheel = (activeCode == 2 || activeCode == InputMap::kMouseWheelUp ||
                                       activeCode == InputMap::kMouseWheelDown);
                if (litWheel) BlendOver(dst, ar, ag, ab, aWheel);
                else BlendOver(dst, 0.72f, 0.80f, 0.90f, aWheel);
            }
        }
    }

    if (aimMode) {
        // Left/right motion chevrons flanking the mouse to signify mouse-aim.
        DrawTextCentered(buf, size, 16.f, cy, "<", 4.0f, 0.35f, 0.88f, 1.0f, 1.f, true);
        DrawTextCentered(buf, size, 112.f, cy, ">", 4.0f, 0.35f, 0.88f, 1.0f, 1.f, true);
        DrawTextCentered(buf, size, cx, cy + 20.f, "AIM", 3.4f, 0.94f, 0.98f, 1.0f, 1.f, true);
    } else if (!badge.empty()) {
        DrawTextCentered(buf, size, cx, cy + 20.f, badge, 3.6f, 0.97f, 0.99f, 1.0f, 1.f, true);
    }
}

inline void RenderKeyCluster(uint8_t* buf, int size, std::string_view fourChars) {
    // Inverted-T 4-keycap cluster: index 0 = top (W / ^), 1 = left (A / <), 2 = down (S / v), 3 = right (D / >).
    const char c0 = fourChars.size() > 0 ? fourChars[0] : 'W';
    const char c1 = fourChars.size() > 1 ? fourChars[1] : 'A';
    const char c2 = fourChars.size() > 2 ? fourChars[2] : 'S';
    const char c3 = fourChars.size() > 3 ? fourChars[3] : 'D';
    const float hx = 18.f, hy = 18.f, r = 6.5f, rim = 2.0f, skirt = 4.f;
    const float stepX = 40.f, topY = 41.f, botY = 85.f;
    const struct { float x, y; char ch; } keys[4] = {
        {64.f, topY, c0},
        {64.f - stepX, botY, c1},
        {64.f, botY, c2},
        {64.f + stepX, botY, c3},
    };
    for (const auto& k : keys) {
        DrawKeycapBox(buf, size, k.x, k.y, hx, hy, r, rim, skirt);
        char s[2] = {k.ch, '\0'};
        DrawTextCentered(buf, size, k.x, k.y - skirt * 0.35f, std::string_view(s, 1), 3.4f, 0.97f, 0.99f, 1.0f, 1.f, true);
    }
}

inline std::string KeyLabel(int32_t scancode) {
    if (scancode >= 4 && scancode <= 29) return std::string(1, static_cast<char>('A' + (scancode - 4)));
    if (scancode >= 30 && scancode <= 38) return std::string(1, static_cast<char>('1' + (scancode - 30)));
    if (scancode == 39) return "0";
    if (scancode >= 58 && scancode <= 69) return "F" + std::to_string(scancode - 57);
    switch (scancode) {
    case 40: return "ENT";
    case 41: return "ESC";
    case 42: return "BKSP";
    case 43: return "TAB";
    case 44: return "SPC";
    case 45: return "-";
    case 46: return "=";
    case 47: return "[";
    case 48: return "]";
    case 49: return "\\";
    case 51: return ";";
    case 52: return "'";
    case 53: return "`";
    case 54: return ",";
    case 55: return ".";
    case 56: return "/";
    case 57: return "CAPS";
    case 73: return "INS";
    case 74: return "HOME";
    case 75: return "PGUP";
    case 76: return "DEL";
    case 77: return "END";
    case 78: return "PGDN";
    case 79: return ">";
    case 80: return "<";
    case 81: return "v";
    case 82: return "^";
    case 84: return "N/";
    case 85: return "N*";
    case 86: return "N-";
    case 87: return "N+";
    case 88: return "NENT";
    case 89: return "N1";
    case 90: return "N2";
    case 91: return "N3";
    case 92: return "N4";
    case 93: return "N5";
    case 94: return "N6";
    case 95: return "N7";
    case 96: return "N8";
    case 97: return "N9";
    case 98: return "N0";
    case 99: return "N.";
    case 224: case 228: return "CTRL";
    case 225: case 229: return "SHFT";
    case 226: case 230: return "ALT";
    case 227: case 231: return "WIN";
    default: break;
    }
    const auto& hooks = InputMap::KeyHooks();
    if (hooks.name) {
        std::string n = hooks.name(scancode);
        std::string out;
        for (char c : n) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '/') {
                out.push_back(c);
                if (out.size() >= 4) break;
            }
        }
        if (!out.empty()) return out;
    }
    return "K" + std::to_string(scancode % 100);
}

inline GlyphDesc BindingToDesc(const InputMap::Binding& b) {
    if (b.device == InputMap::Device::Mouse) {
        return {GlyphKind::MouseBtn, b.code, {}};
    }
    if (b.device == InputMap::Device::Key) {
        return {GlyphKind::Keycap, 0, KeyLabel(b.code)};
    }
    return {GlyphKind::Keycap, 0, "-"};
}

inline GlyphDesc ActionDesc(const AimSettings::Settings& s, AimSettings::Action a, bool menuConfirmPreferLmb = false) {
    const auto& slots = s.bindings.kbm[static_cast<size_t>(a)];
    if (menuConfirmPreferLmb && s.mouse.enabled && slots[0].device == InputMap::Device::Key && slots[0].code == 40) {
        // Default Confirm is Return; in pointer menus Left-Click selects directly.
        return {GlyphKind::MouseBtn, 1, {}};
    }
    for (const auto& b : slots) {
        if (b.Valid()) return BindingToDesc(b);
    }
    return {GlyphKind::Keycap, 0, "-"};
}

inline GlyphDesc MoveClusterDesc(const AimSettings::Settings& s) {
    using A = AimSettings::Action;
    const A dirs[4] = {A::MoveUp, A::MoveLeft, A::MoveDown, A::MoveRight};
    const char fallback[4] = {'W', 'A', 'S', 'D'};
    std::string chars;
    chars.reserve(4);
    for (int i = 0; i < 4; ++i) {
        char ch = fallback[i];
        for (const auto& b : s.bindings.kbm[static_cast<size_t>(dirs[i])]) {
            if (b.device == InputMap::Device::Key) {
                const std::string lbl = KeyLabel(b.code);
                if (lbl.size() == 1) ch = lbl[0];
                break;
            }
        }
        chars.push_back(ch);
    }
    return {GlyphKind::MoveCluster, 0, std::move(chars)};
}

inline std::array<GlyphDesc, 32> BuildFrameDescs(const AimSettings::Settings& s, bool combat) {
    using A = AimSettings::Action;
    const auto act = [&](A a) { return ActionDesc(s, a, false); };
    const GlyphDesc confirm = ActionDesc(s, A::Confirm, !combat);
    const GlyphDesc cancel = act(A::Cancel);
    const GlyphDesc move = MoveClusterDesc(s);
    const GlyphDesc aim = {GlyphKind::MouseAim, 0, {}};
    const GlyphDesc dpad = {GlyphKind::DpadCluster, 0, "^<v>"};
    const GlyphDesc dpadDown = [&]() -> GlyphDesc {
        for (const auto& b : s.bindings.kbm[static_cast<size_t>(A::DpadDown)]) {
            if (b.Valid()) return BindingToDesc(b);
        }
        return {GlyphKind::Keycap, 0, "v"};
    }();
    return {
        combat ? act(A::LockOn) : confirm, // 0
        combat ? act(A::Fire) : cancel,    // 1
        act(A::Jump),                      // 2
        act(A::Evade),                     // 3
        move,                              // 4
        act(A::Pause),                     // 5
        act(A::Minus),                     // 6
        act(A::ButtonX),                   // 7
        act(A::ButtonY),                   // 8
        confirm,                           // 9
        cancel,                            // 10
        act(A::ButtonX),                   // 11
        act(A::ButtonY),                   // 12
        act(A::Evade),                     // 13
        act(A::Jump),                      // 14
        act(A::Fire),                      // 15
        move,                              // 16
        aim,                               // 17
        dpad,                              // 18
        dpadDown,                          // 19
        act(A::Pause),                     // 20
        act(A::ButtonY),                   // 21
        act(A::ButtonX),                   // 22
        confirm,                           // 23
        cancel,                            // 24
        act(A::Jump),                      // 25
        act(A::Fire),                      // 26
        act(A::Evade),                     // 27
        act(A::LockOn),                    // 28
        move,                              // 29
        aim,                               // 30
        act(A::Pause),                     // 31
    };
}

inline void RenderGlyphHD(const GlyphDesc& desc, uint8_t* out /* 128*128*4 */) {
    std::fill_n(out, size_t(kGlyphHdSize) * kGlyphHdSize * 4, uint8_t(0));
    switch (desc.kind) {
    case GlyphKind::Keycap:
        RenderKeycap(out, kGlyphHdSize, desc.label);
        break;
    case GlyphKind::MouseBtn:
        RenderMouse(out, kGlyphHdSize, desc.mouseCode, false);
        break;
    case GlyphKind::MouseAim:
        RenderMouse(out, kGlyphHdSize, 0, true);
        break;
    case GlyphKind::MoveCluster:
    case GlyphKind::DpadCluster:
        RenderKeyCluster(out, kGlyphHdSize, desc.label);
        break;
    }
}

} // namespace kbm_detail

inline void BuildAtlas(const AimSettings::Settings& s, Atlas& out, bool combat = false) {
    out.fill(0);
    if (s.IsKeyboardPrompts()) {
        const auto descs = kbm_detail::BuildFrameDescs(s, combat);
        std::vector<kbm_detail::GlyphDesc> cachedDescs;
        std::vector<std::array<uint8_t, 32 * 32 * 4>> cachedTiles;
        std::vector<uint8_t> hd(size_t(kGlyphHdSize) * kGlyphHdSize * 4);
        for (int frame = 0; frame < 32; ++frame) {
            size_t idx = cachedDescs.size();
            for (size_t i = 0; i < cachedDescs.size(); ++i) {
                if (cachedDescs[i] == descs[frame]) { idx = i; break; }
            }
            if (idx == cachedDescs.size()) {
                kbm_detail::RenderGlyphHD(descs[frame], hd.data());
                std::array<uint8_t, 32 * 32 * 4> tile{};
                for (int y = 0; y < 32; ++y) {
                    for (int x = 0; x < 32; ++x) {
                        float acc[4] = {0, 0, 0, 0};
                        for (int dy = 0; dy < 4; ++dy) {
                            for (int dx = 0; dx < 4; ++dx) {
                                const uint8_t* q = &hd[(size_t(4 * y + dy) * kGlyphHdSize + (4 * x + dx)) * 4];
                                const float a = q[3] / 255.f;
                                acc[0] += q[0] * a; acc[1] += q[1] * a; acc[2] += q[2] * a; acc[3] += q[3];
                            }
                        }
                        uint8_t* d = &tile[(y * 32 + x) * 4];
                        const float a = acc[3] / 16.f;
                        for (int c = 0; c < 3; ++c)
                            d[c] = uint8_t(a > 0.5f ? std::min(255.f, acc[c] / (a / 255.f) / 16.f + 0.5f) : 0);
                        d[3] = uint8_t(a + 0.5f);
                    }
                }
                cachedDescs.push_back(descs[frame]);
                cachedTiles.push_back(tile);
            }
            const int cell = kCells[frame], ox = (cell % 8) * 32, oy = (cell / 8) * 32;
            const auto& tilePixels = cachedTiles[idx];
            for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
                const uint8_t* rgba = &tilePixels[(y * 32 + x) * 4];
                const int px = ox + x, py = oy + y;
                const size_t tile = ((py / 4) * 64 + px / 4) * 64;
                const size_t sample = ((py % 4) * 4 + px % 4) * 2;
                out[tile + sample] = rgba[3]; out[tile + sample + 1] = rgba[0];
                out[tile + 32 + sample] = rgba[1]; out[tile + 32 + sample + 1] = rgba[2];
            }
        }
        return;
    }
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
    const int style = s.IsPlayStationPrompts() ? 1 : 0;
    for (int frame=0;frame<32;++frame) {
        const int cell=kCells[frame], ox=(cell%8)*32, oy=(cell/8)*32;
        const int icon = icons[frame];
        for (int y=0;y<32;++y) for (int x=0;x<32;++x) {
            const auto* rgba=&kGlyphPixels[style][icon][(y*32+x)*4];
            const int px=ox+x,py=oy+y;
            const size_t tile=((py/4)*64+px/4)*64;
            const size_t sample=((py%4)*4+px%4)*2;
            out[tile+sample]=rgba[3];out[tile+sample+1]=rgba[0];
            out[tile+32+sample]=rgba[1];out[tile+32+sample+1]=rgba[2];
        }
    }
}

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
    constexpr int N = kHdAtlasSize, G = kGlyphHdSize;
    size_t total = 0;
    for (int m = 0; m < kHdAtlasMips; ++m) total += size_t(N >> m) * (N >> m) * 4;
    out.assign(total, 0);
    if (s.IsKeyboardPrompts()) {
        const auto descs = kbm_detail::BuildFrameDescs(s, combat);
        std::vector<kbm_detail::GlyphDesc> cachedDescs;
        std::vector<std::vector<uint8_t>> cachedGlyphs;
        for (int frame = 0; frame < 32; ++frame) {
            size_t idx = cachedDescs.size();
            for (size_t i = 0; i < cachedDescs.size(); ++i) {
                if (cachedDescs[i] == descs[frame]) { idx = i; break; }
            }
            if (idx == cachedDescs.size()) {
                std::vector<uint8_t> g(size_t(G) * G * 4);
                kbm_detail::RenderGlyphHD(descs[frame], g.data());
                cachedDescs.push_back(descs[frame]);
                cachedGlyphs.push_back(std::move(g));
            }
            const int cell = kCells[frame], ox = (cell % 8) * G, oy = (cell / 8) * G;
            const auto& glyph = cachedGlyphs[idx];
            for (int y = 0; y < G; ++y)
                for (int x = 0; x < G; ++x)
                    for (int c = 0; c < 4; ++c)
                        out[(size_t(oy + y) * N + ox + x) * 4 + c] = glyph[(size_t(y) * G + x) * 4 + c];
        }
    } else {
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
        std::vector<uint8_t> glyph(size_t(G) * G * 4);
        const int style = s.IsPlayStationPrompts() ? 1 : 0;
        for (int frame = 0; frame < 32; ++frame) {
            const int cell = kCells[frame], ox = (cell % 8) * G, oy = (cell / 8) * G;
            DecodeHdGlyph(style, icons[frame], glyph.data());
            for (int y = 0; y < G; ++y)
                for (int x = 0; x < G; ++x)
                    for (int c = 0; c < 4; ++c)
                        out[(size_t(oy + y) * N + ox + x) * 4 + c] = glyph[(size_t(y) * G + x) * 4 + c];
        }
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
