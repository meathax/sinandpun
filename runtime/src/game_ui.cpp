#include "game_ui.h"
#include "game_ui_font.h"

#include <SDL3/SDL_gamepad.h>
#include <aurora/aurora.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace game_ui {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kBakePx = 56.0f;
ImFont* g_font = nullptr;
int g_fontAddedFrame = 1 << 30;
bool g_fontTried = false;

float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float FontPx(float sizeU) { return std::max(6.0f, sizeU * Scale()); }

ImU32 OutlineFor(ImU32 colour) {
    const int r = static_cast<int>((colour >> IM_COL32_R_SHIFT) & 0xFF);
    const int g = static_cast<int>((colour >> IM_COL32_G_SHIFT) & 0xFF);
    const int b = static_cast<int>((colour >> IM_COL32_B_SHIFT) & 0xFF);
    return (r > 200 && g > 190 && b < 140) ? kTitleOutline : kTextOutline;
}

void WrapLines(float sizeU, float maxWidthPx, const std::string& text, std::vector<std::string>& out) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string paragraph = text.substr(start, end - start);
        std::string line;
        size_t pos = 0;
        while (pos <= paragraph.size()) {
            size_t space = paragraph.find(' ', pos);
            if (space == std::string::npos) space = paragraph.size();
            const std::string word = paragraph.substr(pos, space - pos);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && TextSize(sizeU, candidate.c_str()).x > maxWidthPx) {
                out.push_back(line);
                line = word;
            } else {
                line = candidate;
            }
            pos = space + 1;
        }
        out.push_back(line);
        if (end >= text.size()) break;
        start = end + 1;
    }
}

} // namespace

// ---- scale and font ----------------------------------------------------------------------------------

void InstallFont() {
    if (g_fontTried || ImGui::GetCurrentContext() == nullptr) return;
    ImGuiIO& io = ImGui::GetIO();
    // The atlas cannot change between NewFrame() and Render(); callers retry from the event hook.
    if (io.Fonts->Locked) return;
    g_fontTried = true;
    if (io.Fonts->Fonts.Size == 0) io.Fonts->AddFontDefault();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.RasterizerDensity = 1.0f;
    const AuroraWindowSize ws = aurora_get_window_size();
    if (ws.width > 0 && ws.native_fb_width > 0) {
        cfg.RasterizerDensity =
            std::clamp(static_cast<float>(ws.native_fb_width) / static_cast<float>(ws.width), 1.0f, 2.0f);
    }
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2018, 0x201D, 0x2022, 0x2022, 0x2026, 0x2026, 0};
    g_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kUiFontTtf), static_cast<int>(kUiFontTtfSize),
                                            kBakePx, &cfg, kRanges);
    g_fontAddedFrame = ImGui::GetFrameCount();
}

bool FontReady() {
    if (g_font == nullptr || ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::GetIO().Fonts->IsBuilt() && ImGui::GetFrameCount() > g_fontAddedFrame && g_font->Glyphs.Size > 0;
}

ImFont* Font() { return FontReady() ? g_font : ImGui::GetFont(); }

float Scale() {
    if (ImGui::GetCurrentContext() == nullptr) return 1.0f;
    return std::clamp(ImGui::GetIO().DisplaySize.y / 1080.0f, 0.4f, 4.0f);
}

// ---- text --------------------------------------------------------------------------------------------

ImVec2 TextSize(float sizeU, const char* text) {
    if (text == nullptr || *text == '\0') return ImVec2(0.0f, FontPx(sizeU));
    return Font()->CalcTextSizeA(FontPx(sizeU), FLT_MAX, 0.0f, text);
}

void Text(ImDrawList* dl, float sizeU, ImVec2 pos, ImU32 colour, const char* text, Outline outline, float alpha) {
    if (text == nullptr || *text == '\0') return;
    ImFont* font = Font();
    const float px = FontPx(sizeU);
    const float own = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    if (outline != Outline::None) {
        const float t = std::max(1.0f, px * (outline == Outline::Thick ? 0.075f : 0.055f));
        const ImU32 oc = Fade(OutlineFor(colour), alpha * own);
        const float d = t * 0.72f;
        static const float kDiag[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (const auto& o : kDiag) dl->AddText(font, px, ImVec2(pos.x + o[0] * d, pos.y + o[1] * d), oc, text);
        if (outline == Outline::Thick) {
            static const float kAxis[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto& o : kAxis) dl->AddText(font, px, ImVec2(pos.x + o[0] * t, pos.y + o[1] * t), oc, text);
        }
    }
    dl->AddText(font, px, pos, Fade(colour, alpha), text);
}

void TextCentered(ImDrawList* dl, float sizeU, ImVec2 centre, ImU32 colour, const char* text, Outline outline,
                      float alpha) {
    const ImVec2 size = TextSize(sizeU, text);
    Text(dl, sizeU, ImVec2(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f), colour, text, outline, alpha);
}

void TextRight(ImDrawList* dl, float sizeU, ImVec2 rightTop, ImU32 colour, const char* text, Outline outline,
                   float alpha) {
    const ImVec2 size = TextSize(sizeU, text);
    Text(dl, sizeU, ImVec2(rightTop.x - size.x, rightTop.y), colour, text, outline, alpha);
}

float WrappedHeight(float sizeU, float maxWidthPx, const std::string& text) {
    std::vector<std::string> lines;
    WrapLines(sizeU, maxWidthPx, text, lines);
    return static_cast<float>(lines.size()) * FontPx(sizeU) * 1.28f;
}

float DrawWrapped(ImDrawList* dl, float sizeU, ImVec2 pos, float maxWidthPx, ImU32 colour, const std::string& text,
                  Outline outline, float alpha, bool centered) {
    std::vector<std::string> lines;
    WrapLines(sizeU, maxWidthPx, text, lines);
    const float step = FontPx(sizeU) * 1.28f;
    float y = pos.y;
    for (const std::string& line : lines) {
        float x = pos.x;
        if (centered) x = pos.x + (maxWidthPx - TextSize(sizeU, line.c_str()).x) * 0.5f;
        Text(dl, sizeU, ImVec2(x, y), colour, line.c_str(), outline, alpha);
        y += step;
    }
    return y - pos.y;
}

std::string Ellipsize(const std::string& text, float sizeU, float maxWidthPx, bool middle) {
    if (TextSize(sizeU, text.c_str()).x <= maxWidthPx) return text;
    const std::string dots = "...";
    size_t keep = text.size();
    while (keep > 1) {
        --keep;
        std::string candidate;
        if (middle) {
            const size_t head = keep / 3;
            const size_t tail = keep - head;
            candidate = text.substr(0, head) + dots + text.substr(text.size() - tail);
        } else {
            size_t cut = keep;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
            candidate = text.substr(0, cut) + dots;
        }
        if (TextSize(sizeU, candidate.c_str()).x <= maxWidthPx) return candidate;
    }
    return dots;
}

float FittedTextSize(const std::string& text, float preferredU, float minimumU, float maxWidthPx) {
    if (maxWidthPx <= 0.0f) return minimumU;
    float size = preferredU;
    while (size > minimumU && TextSize(size, text.c_str()).x > maxWidthPx) {
        size = std::max(minimumU, size - 1.0f);
    }
    return size;
}

// ---- input -------------------------------------------------------------------------------------------
namespace {

struct Repeater {
    bool down = false;
    float held = 0.0f;
    float next = 0.0f;
};

// 0 = nothing, 1 = first press, 2 = repeat.
int Step(Repeater& r, bool isDown, float dt) {
    if (!isDown) {
        r = Repeater{};
        return 0;
    }
    if (!r.down) {
        r.down = true;
        r.held = 0.0f;
        r.next = 0.38f;
        return 1;
    }
    r.held += dt;
    if (r.held >= r.next) {
        r.next += r.held > 1.6f ? 0.045f : 0.085f;
        return 2;
    }
    return 0;
}

Frame g_frame;
int g_frameStamp = -1;
Device g_device = Device::Mouse;
Clock::time_point g_suppressUntil{};
Repeater g_rUp, g_rDown, g_rLeft, g_rRight;
std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> g_prevButtons{};
bool g_prevLt = false, g_prevRt = false;

struct PadSnapshot {
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    float lx = 0.0f, ly = 0.0f;
    bool lt = false, rt = false;
    bool any = false;
};

PadSnapshot ReadPads() {
    PadSnapshot s;
    const uint32_t count = PADCount();
    for (uint32_t i = 0; i < count; ++i) {
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(i);
        if (pad == nullptr) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) {
                s.buttons[static_cast<size_t>(b)] = true;
                s.any = true;
            }
        }
        const float x = static_cast<float>(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
        const float y = static_cast<float>(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
        if (std::fabs(x) > std::fabs(s.lx)) s.lx = x;
        if (std::fabs(y) > std::fabs(s.ly)) s.ly = y;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) s.lt = true;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) s.rt = true;
    }
    return s;
}

} // namespace

Frame& PollInput() {
    const int stamp = ImGui::GetFrameCount();
    if (stamp == g_frameStamp) return g_frame;
    g_frameStamp = stamp;
    ImGuiIO& io = ImGui::GetIO();
    Frame f;
    f.dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);

    const PadSnapshot pad = ReadPads();
    auto padEdge = [&](SDL_GamepadButton b) {
        const size_t i = static_cast<size_t>(b);
        return pad.buttons[i] && !g_prevButtons[i];
    };
    auto padDown = [&](SDL_GamepadButton b) { return pad.buttons[static_cast<size_t>(b)]; };

    const bool kbUp = ImGui::IsKeyDown(ImGuiKey_UpArrow);
    const bool kbDown = ImGui::IsKeyDown(ImGuiKey_DownArrow);
    const bool kbLeft = ImGui::IsKeyDown(ImGuiKey_LeftArrow);
    const bool kbRight = ImGui::IsKeyDown(ImGuiKey_RightArrow);
    const bool padUp = padDown(SDL_GAMEPAD_BUTTON_DPAD_UP) || pad.ly < -0.55f;
    const bool padDownDir = padDown(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || pad.ly > 0.55f;
    const bool padLeft = padDown(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || pad.lx < -0.55f;
    const bool padRight = padDown(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || pad.lx > 0.55f;

    const int su = Step(g_rUp, kbUp || padUp, f.dt);
    const int sd = Step(g_rDown, kbDown || padDownDir, f.dt);
    const int sl = Step(g_rLeft, kbLeft || padLeft, f.dt);
    const int sr = Step(g_rRight, kbRight || padRight, f.dt);
    f.up = su != 0;
    f.down = sd != 0;
    f.left = sl != 0;
    f.right = sr != 0;
    f.navFresh = su == 1 || sd == 1 || sl == 1 || sr == 1;
    const float held = std::max(g_rLeft.down ? g_rLeft.held : 0.0f, g_rRight.down ? g_rRight.held : 0.0f);
    f.adjustMul = held > 2.4f ? 8 : held > 1.2f ? 4 : held > 0.6f ? 2 : 1;

    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;
    const bool kbAccept = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_Space, false);
    const bool kbBack = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const bool kbReset = ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    const bool kbPrev = ImGui::IsKeyPressed(ImGuiKey_Q, false) || (shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    const bool kbNext = ImGui::IsKeyPressed(ImGuiKey_E, false) || (!shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    f.accept = padEdge(SDL_GAMEPAD_BUTTON_SOUTH) || kbAccept;
    f.back = padEdge(SDL_GAMEPAD_BUTTON_EAST) || kbBack;
    f.resetRow = padEdge(SDL_GAMEPAD_BUTTON_WEST) || (kbReset && !ctrl);
    f.resetPage = padEdge(SDL_GAMEPAD_BUTTON_NORTH) || (kbReset && ctrl);
    f.tabPrev = padEdge(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || kbPrev;
    f.tabNext = padEdge(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || kbNext;
    f.pageUp = (pad.lt && !g_prevLt) || ImGui::IsKeyPressed(ImGuiKey_PageUp, true);
    f.pageDown = (pad.rt && !g_prevRt) || ImGui::IsKeyPressed(ImGuiKey_PageDown, true);

    const bool padEvent = f.accept ? padEdge(SDL_GAMEPAD_BUTTON_SOUTH) : false;
    const bool padAny = padUp || padDownDir || padLeft || padRight || padEvent || padEdge(SDL_GAMEPAD_BUTTON_EAST) ||
                        padEdge(SDL_GAMEPAD_BUTTON_WEST) || padEdge(SDL_GAMEPAD_BUTTON_NORTH) ||
                        padEdge(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || padEdge(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    const bool kbAny = kbUp || kbDown || kbLeft || kbRight || kbAccept || kbBack || kbReset || kbPrev || kbNext ||
                       ImGui::IsKeyPressed(ImGuiKey_PageUp, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false);

    f.mouseValid = ImGui::IsMousePosValid(&io.MousePos);
    f.mouse = f.mouseValid ? io.MousePos : ImVec2(-1.0f, -1.0f);
    f.mouseMoved = f.mouseValid && (std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y)) > 0.5f;
    f.mouseClicked = f.mouseValid && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
    f.mouseDown = f.mouseValid && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    f.mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    f.rightClicked = f.mouseValid && ImGui::IsMouseClicked(ImGuiMouseButton_Right, false);
    f.wheel = io.MouseWheel;
    const bool mouseAny = f.mouseMoved || f.mouseClicked || f.rightClicked || f.wheel != 0.0f;

    if (padAny) g_device = Device::Gamepad;
    if (kbAny) g_device = Device::Keyboard;
    if (mouseAny && !padAny && !kbAny) g_device = Device::Mouse;
    f.device = g_device;
    f.anyNav = f.up || f.down || f.left || f.right || f.accept || f.back;

    g_prevButtons = pad.buttons;
    g_prevLt = pad.lt;
    g_prevRt = pad.rt;

    if (Clock::now() < g_suppressUntil) {
        f.accept = f.back = f.resetRow = f.resetPage = f.tabPrev = f.tabNext = false;
        f.mouseClicked = f.rightClicked = false;
    }
    g_frame = f;
    return g_frame;
}

void Suppress(float seconds) {
    const auto until = Clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f));
    if (until > g_suppressUntil) g_suppressUntil = until;
    // Also drop whatever this frame already collected.
    g_frame.accept = g_frame.back = g_frame.resetRow = g_frame.resetPage = g_frame.tabPrev = g_frame.tabNext = false;
    g_frame.mouseClicked = g_frame.rightClicked = false;
}

Device LastDevice() { return g_device; }

bool PointIn(ImVec2 p, ImVec2 min, ImVec2 max) { return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y; }

// ---- primitives --------------------------------------------------------------------------------------

void DrawGlassPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha, float rimU, float radiusU) {
    const float rim = std::max(2.0f, Px(rimU));
    const float r = Px(radiusU);
    for (int i = 4; i >= 1; --i) {
        const float e = Px(7.0f) * static_cast<float>(i);
        dl->AddRectFilled(ImVec2(min.x - e, min.y - e * 0.6f), ImVec2(max.x + e, max.y + e * 1.4f),
                          Fade(IM_COL32(0, 8, 24, 34), alpha), r + e);
    }
    dl->AddRectFilled(min, max, Fade(IM_COL32(246, 250, 253, 255), alpha), r);
    const ImVec2 imin(min.x + rim, min.y + rim);
    const ImVec2 imax(max.x - rim, max.y - rim);
    const float ir = std::max(2.0f, r - rim);
    dl->AddRectFilled(imin, imax, Fade(IM_COL32(100, 150, 192, 244), alpha), ir);
    // Glass sheen (light at the top, darker at the bottom) and faint scanlines, drawn as thin slices that are
    // inset along the rounded corners.
    const float h = imax.y - imin.y;
    const float step = std::max(2.0f, Px(4.0f));
    const float thick = std::max(1.0f, step * 0.34f);
    for (float y = imin.y; y + step < imax.y; y += step) {
        const float top = y - imin.y;
        const float bot = imax.y - (y + step);
        const float d = std::min(top, bot);
        float inset = 0.0f;
        if (d < ir) {
            const float k = ir - d;
            inset = ir - std::sqrt(std::max(0.0f, ir * ir - k * k));
        }
        const float t = (y - imin.y) / h;
        const ImU32 sheen = t < 0.55f
            ? IM_COL32(200, 232, 252, static_cast<int>(34.0f * (1.0f - t / 0.55f)))
            : IM_COL32(20, 52, 100, static_cast<int>(40.0f * (t - 0.55f) / 0.45f));
        dl->AddRectFilled(ImVec2(imin.x + inset, y), ImVec2(imax.x - inset, y + step), Fade(sheen, alpha));
        dl->AddRectFilled(ImVec2(imin.x + inset, y + step * 0.5f), ImVec2(imax.x - inset, y + step * 0.5f + thick),
                          Fade(IM_COL32(16, 48, 96, 30), alpha));
        dl->AddRectFilled(ImVec2(imin.x + inset, y), ImVec2(imax.x - inset, y + thick), Fade(IM_COL32(255, 255, 255, 13), alpha));
    }    dl->AddRect(imin, imax, Fade(IM_COL32(26, 66, 112, 150), alpha), ir, 0, std::max(1.0f, Px(2.0f)));
}

void DrawChevron(ImDrawList* dl, ImVec2 c, float r, int dir, bool hot, bool enabled, float alpha) {
    const float a = alpha * (enabled ? 1.0f : 0.32f);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.08f), r * 1.06f, Fade(IM_COL32(0, 10, 30, 70), a), 32);
    dl->AddCircleFilled(c, r, Fade(IM_COL32(244, 249, 253, 255), a), 32);
    dl->AddCircleFilled(c, r * 0.86f, Fade(hot ? IM_COL32(74, 156, 255, 255) : IM_COL32(36, 112, 226, 255), a), 32);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.1f, c.y - r * 0.25f), r * 0.52f, Fade(IM_COL32(160, 214, 255, 110), a), 24);
    const float k = r * 0.36f;
    const float d = static_cast<float>(dir);
    const ImVec2 pts[3] = {ImVec2(c.x - d * k * 0.55f, c.y - k * 1.15f), ImVec2(c.x + d * k * 0.75f, c.y),
                           ImVec2(c.x - d * k * 0.55f, c.y + k * 1.15f)};
    dl->AddPolyline(pts, 3, Fade(kWhite, a), 0, std::max(1.5f, r * 0.2f));
}

void DrawPill(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 fill, ImU32 rim, float alpha, float rimPx) {
    const float r = (max.y - min.y) * 0.5f;
    dl->AddRectFilled(min, max, Fade(fill, alpha), r);
    if ((rim >> IM_COL32_A_SHIFT) != 0) dl->AddRect(min, max, Fade(rim, alpha), r, 0, rimPx);
}

float BadgeWidth(const char* text) { return TextSize(16.0f, text).x + Px(22.0f); }

float DrawBadge(ImDrawList* dl, ImVec2 pos, const char* text, Badge kind, float alpha) {
    ImU32 fill = IM_COL32(150, 170, 190, 255);
    ImU32 ink = IM_COL32(20, 40, 64, 255);
    switch (kind) {
    case Badge::Restart: fill = IM_COL32(255, 184, 52, 255); ink = IM_COL32(64, 34, 0, 255); break;
    case Badge::Recommended: fill = IM_COL32(120, 222, 140, 255); ink = IM_COL32(8, 56, 24, 255); break;
    case Badge::Experimental: fill = IM_COL32(214, 150, 255, 255); ink = IM_COL32(52, 16, 80, 255); break;
    case Badge::Confirm: fill = IM_COL32(130, 214, 255, 255); ink = IM_COL32(8, 44, 78, 255); break;
    case Badge::Neutral: break;
    }
    const float w = BadgeWidth(text);
    const float h = Px(26.0f);
    DrawPill(dl, pos, ImVec2(pos.x + w, pos.y + h), fill, IM_COL32(255, 255, 255, 150), alpha, 1.0f);
    const ImVec2 ts = TextSize(16.0f, text);
    Text(dl, 16.0f, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), ink, text, Outline::None, alpha);
    return w;
}

float GlyphWidth(const char* text, bool round) {
    if (round) return Px(34.0f);
    return std::max(Px(40.0f), TextSize(20.0f, text).x + Px(22.0f));
}

float DrawGlyph(ImDrawList* dl, ImVec2 lc, const char* text, float alpha, bool round) {
    const float h = Px(34.0f);
    const float w = GlyphWidth(text, round);
    if (round) {
        const ImVec2 c(lc.x + h * 0.5f, lc.y);
        dl->AddCircleFilled(c, h * 0.5f + 1.0f, Fade(IM_COL32(20, 50, 90, 200), alpha), 28);
        dl->AddCircleFilled(c, h * 0.5f - 1.0f, Fade(IM_COL32(238, 245, 251, 255), alpha), 28);
        dl->AddCircleFilled(ImVec2(c.x, c.y - h * 0.12f), h * 0.36f, Fade(IM_COL32(255, 255, 255, 130), alpha), 20);
        TextCentered(dl, 22.0f, c, IM_COL32(26, 62, 110, 255), text, Outline::None, alpha);
    } else {
        const ImVec2 mn(lc.x, lc.y - h * 0.5f);
        const ImVec2 mx(lc.x + w, lc.y + h * 0.5f);
        dl->AddRectFilled(mn, mx, Fade(IM_COL32(20, 50, 90, 200), alpha), h * 0.32f);
        dl->AddRectFilled(ImVec2(mn.x + 1, mn.y + 1), ImVec2(mx.x - 1, mx.y - 1), Fade(IM_COL32(238, 245, 251, 255), alpha),
                          h * 0.3f);
        TextCentered(dl, 19.0f, ImVec2(lc.x + w * 0.5f, lc.y), IM_COL32(26, 62, 110, 255), text, Outline::None, alpha);
    }
    return w;
}

void DrawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 colour) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + static_cast<float>(i) * 0.6283185f;
        const float rr = (i % 2 == 0) ? r : r * 0.42f;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    dl->AddConvexPolyFilled(pts, 10, colour);
    dl->AddPolyline(pts, 10, IM_COL32(40, 90, 40, (colour >> IM_COL32_A_SHIFT) & 0xFF), ImDrawFlags_Closed, 1.0f);
}

float DrawHints(ImDrawList* dl, ImVec2 pos, const std::vector<std::pair<std::string, std::string>>& hints, float alpha,
                int align) {
    const float gap = Px(30.0f);
    const float inner = Px(9.0f);
    float total = 0.0f;
    for (size_t i = 0; i < hints.size(); ++i) {
        total += GlyphWidth(hints[i].first.c_str(), hints[i].first.size() == 1) + inner +
                 TextSize(24.0f, hints[i].second.c_str()).x;
        if (i + 1 < hints.size()) total += gap;
    }
    float x = align == 1 ? pos.x - total * 0.5f : align == 2 ? pos.x - total : pos.x;
    for (const auto& hint : hints) {
        const bool round = hint.first.size() == 1;
        x += DrawGlyph(dl, ImVec2(x, pos.y), hint.first.c_str(), alpha, round) + inner;
        const ImVec2 ts = TextSize(24.0f, hint.second.c_str());
        Text(dl, 24.0f, ImVec2(x, pos.y - ts.y * 0.5f), kWhite, hint.second.c_str(), Outline::Thick, alpha);
        x += ts.x + gap;
    }
    return total;
}

// ---- list view ---------------------------------------------------------------------------------------

struct ListView::Row {
    int index = 0;
    float x0 = 0, x1 = 0;           // label start / content end (px)
    float vx0 = 0;                  // value column start (px)
    float yTop = 0, yBottom = 0, cy = 0;
    bool focused = false;
    bool hovered = false;
    bool clicked = false;
    bool rightClicked = false;
    bool usable = false;            // interactive and not disabled
    bool visible = false;
};

ListView::ListView(ListState& state, Frame& input, ImDrawList* dl, ImVec2 areaMin, ImVec2 areaMax, bool interactive,
                   float alpha, float xOffsetPx)
    : m_state(state), m_in(input), m_dl(dl), m_min(areaMin), m_max(areaMax), m_interactive(interactive), m_alpha(alpha),
      m_xOff(xOffsetPx), m_s(Scale()) {
    m_dl->PushClipRect(m_min, m_max, true);
    m_y = 4.0f;
    const float viewH = m_max.y - m_min.y;
    m_mouseInside = m_in.mouseValid && PointIn(m_in.mouse, m_min, m_max);
    const int count = m_state.count;
    if (m_interactive && count > 0) {
        const int old = m_state.focus;
        int focus = old;
        if (m_in.up) focus = focus > 0 ? focus - 1 : (m_in.navFresh ? count - 1 : 0);
        if (m_in.down) focus = focus < count - 1 ? focus + 1 : (m_in.navFresh ? 0 : count - 1);
        if (m_in.pageUp) focus = std::max(0, focus - 6);
        if (m_in.pageDown) focus = std::min(count - 1, focus + 6);
        m_state.focus = focus;
        if (focus != old) {
            m_state.cell = 0;
            m_focusMoved = true;
        }
        if (m_mouseInside && m_in.wheel != 0.0f) m_state.scrollTarget -= m_in.wheel * Px(100.0f);
    }
    if (count > 0) m_state.focus = std::clamp(m_state.focus, 0, count - 1);
    const float maxScroll = std::max(0.0f, m_state.contentHeight * m_s - viewH);
    if (m_focusMoved && static_cast<size_t>(m_state.focus) < m_state.top.size()) {
        const float topPx = m_state.top[static_cast<size_t>(m_state.focus)] * m_s;
        const float botPx = m_state.bottom[static_cast<size_t>(m_state.focus)] * m_s;
        const float margin = Px(20.0f);
        if (m_state.focus == 0) {
            m_state.scrollTarget = 0.0f;
        } else if (m_state.focus == count - 1) {
            m_state.scrollTarget = maxScroll;
        } else if (topPx - margin < m_state.scrollTarget) {
            m_state.scrollTarget = topPx - margin;
        } else if (botPx + margin > m_state.scrollTarget + viewH) {
            m_state.scrollTarget = botPx + margin - viewH;
        }
    }
    m_state.scrollTarget = std::clamp(m_state.scrollTarget, 0.0f, maxScroll);
    const float k = 1.0f - std::exp(-m_in.dt * 18.0f);
    m_state.scroll += (m_state.scrollTarget - m_state.scroll) * k;
    if (std::fabs(m_state.scrollTarget - m_state.scroll) < 0.4f) m_state.scroll = m_state.scrollTarget;
    m_state.scroll = std::clamp(m_state.scroll, 0.0f, maxScroll);

    // Highlight bar for the focused row, eased toward the row position of the previous frame.
    if (count > 0 && static_cast<size_t>(m_state.focus) < m_state.top.size()) {
        const float tTop = m_state.top[static_cast<size_t>(m_state.focus)] * m_s;
        const float tH = (m_state.bottom[static_cast<size_t>(m_state.focus)] - m_state.top[static_cast<size_t>(m_state.focus)]) * m_s;
        if (!m_state.hiInit) {
            m_state.hiTop = tTop;
            m_state.hiHeight = tH;
            m_state.hiInit = true;
        }
        const float kh = 1.0f - std::exp(-m_in.dt * 26.0f);
        m_state.hiTop += (tTop - m_state.hiTop) * kh;
        m_state.hiHeight += (tH - m_state.hiHeight) * kh;
        const float y0 = m_min.y + m_state.hiTop - m_state.scroll;
        const ImVec2 a(m_min.x + Px(14.0f) + m_xOff, y0 + Px(2.0f));
        const ImVec2 b(m_max.x - Px(30.0f) + m_xOff, y0 + m_state.hiHeight - Px(2.0f));
        const float rr = Px(16.0f);
        m_dl->AddRectFilled(ImVec2(a.x - 2, a.y + 3), ImVec2(b.x + 2, b.y + 5), Fade(IM_COL32(0, 20, 50, 40), m_alpha), rr + 2);
        m_dl->AddRectFilled(a, b, Fade(IM_COL32(236, 246, 255, 78), m_alpha), rr);
        m_dl->AddRectFilled(a, ImVec2(b.x, a.y + (b.y - a.y) * 0.5f), Fade(IM_COL32(255, 255, 255, 38), m_alpha), rr,
                            ImDrawFlags_RoundCornersTop);
        m_dl->AddRect(a, b, Fade(IM_COL32(255, 255, 255, 215), m_alpha), rr, 0, std::max(1.5f, Px(2.2f)));
    }
}

ListView::~ListView() {
    if (!m_ended) End();
}

ListView::Row ListView::Begin(RowKind kind, float heightU, const char* help, uint32_t flags, bool resettable) {
    Row r;
    r.index = m_count++;
    const float top = m_y;
    const float bottom = m_y + heightU;
    m_top.push_back(top);
    m_bottom.push_back(bottom);
    m_y = bottom;
    r.yTop = m_min.y + top * m_s - m_state.scroll;
    r.yBottom = m_min.y + bottom * m_s - m_state.scroll;
    r.cy = (r.yTop + r.yBottom) * 0.5f;
    r.x0 = m_min.x + Px(44.0f) + m_xOff + ((flags & kRowIndent) ? Px(30.0f) : 0.0f);
    r.x1 = m_max.x - Px(44.0f + 18.0f) + m_xOff;
    r.vx0 = m_min.x + Px(44.0f) + m_xOff + (r.x1 - (m_min.x + Px(44.0f) + m_xOff)) * 0.50f;
    r.visible = r.yBottom > m_min.y && r.yTop < m_max.y;
    r.usable = m_interactive && !(flags & kRowDisabled);
    if (m_interactive && m_mouseInside && r.visible) {
        const ImVec2 mn(m_min.x, std::max(r.yTop, m_min.y));
        const ImVec2 mx(m_max.x, std::min(r.yBottom, m_max.y));
        r.hovered = PointIn(m_in.mouse, mn, mx);
    }
    if (m_interactive && r.hovered && m_in.mouseMoved && m_state.focus != r.index) {
        m_state.focus = r.index;
        m_state.cell = 0;
    }
    r.focused = m_state.focus == r.index;
    if (m_interactive && r.hovered && m_in.mouseClicked) {
        m_state.focus = r.index;
        r.focused = true;
        r.clicked = true;
    }
    if (m_interactive && r.hovered && m_in.rightClicked) r.rightClicked = true;
    if (r.focused) {
        m_focusedKind = kind;
        m_focusedHelp = help != nullptr ? help : "";
        m_focusedResettable = resettable && !(flags & kRowDisabled);
    }
    return r;
}

void ListView::DrawLabel(const Row& row, const char* label, uint32_t flags, const char*, const std::string&) {
    const bool dim = (flags & kRowDisabled) != 0;
    ImU32 colour = row.focused ? kWhite : kTextNormal;
    if (flags & kRowDanger) colour = row.focused ? IM_COL32(255, 214, 206, 255) : IM_COL32(255, 176, 168, 255);
    if (dim) colour = kTextDim;
    const float alpha = m_alpha * (dim ? 0.7f : 1.0f);
    const float limit = row.vx0 - Px(8.0f);
    float badgeWidth = 0.0f;
    if (flags & kRowRestart) badgeWidth += BadgeWidth("RESTART") + Px(8.0f);
    if (flags & kRowNextLaunch) badgeWidth += BadgeWidth("NEXT LAUNCH") + Px(8.0f);
    if (flags & kRowConfirm) badgeWidth += BadgeWidth("CONFIRM") + Px(8.0f);
    if (flags & kRowExperimental) badgeWidth += BadgeWidth("EXPERIMENTAL") + Px(8.0f);
    const float labelSize = FittedTextSize(label, 32.0f, 20.0f, limit - row.x0 - badgeWidth - Px(16.0f));
    const ImVec2 ts = TextSize(labelSize, label);
    Text(m_dl, labelSize, ImVec2(row.x0, row.cy - ts.y * 0.5f), colour, label, Outline::Thick, alpha);
    float x = row.x0 + ts.x + Px(16.0f);
    auto badge = [&](const char* text, Badge kind) {
        if (x + BadgeWidth(text) > limit) return;
        x += DrawBadge(m_dl, ImVec2(x, row.cy - Px(13.0f)), text, kind, m_alpha) + Px(8.0f);
    };
    if (flags & kRowRestart) badge("RESTART", Badge::Restart);
    if (flags & kRowNextLaunch) badge("NEXT LAUNCH", Badge::Restart);
    if (flags & kRowConfirm) badge("CONFIRM", Badge::Confirm);
    if (flags & kRowExperimental) badge("EXPERIMENTAL", Badge::Experimental);
}

// Returns -1 / +1 for a click on a chevron, 2 for a click elsewhere on the row, 0 otherwise.
int ListView::DrawChevronValue(const Row& row, const std::string& value, bool canLeft, bool canRight, ImU32 colour,
                               bool dim) {
    const float r = Px(21.0f);
    const ImVec2 cl(row.vx0 + r, row.cy);
    const ImVec2 cr(row.x1 - r, row.cy);
    const bool hotL = row.hovered && m_in.mouseValid && std::fabs(m_in.mouse.x - cl.x) < r * 1.3f;
    const bool hotR = row.hovered && m_in.mouseValid && std::fabs(m_in.mouse.x - cr.x) < r * 1.3f;
    if (row.focused || row.hovered) {
        DrawChevron(m_dl, cl, r, -1, hotL, canLeft && row.usable, m_alpha);
        DrawChevron(m_dl, cr, r, 1, hotR, canRight && row.usable, m_alpha);
    }
    const float maxW = (cr.x - cl.x) - r * 2.0f - Px(24.0f);
    const float valueSize = FittedTextSize(value, 32.0f, 20.0f, maxW);
    const std::string text = Ellipsize(value, valueSize, maxW);
    const ImVec2 ts = TextSize(valueSize, text.c_str());
    const ImVec2 centre((cl.x + cr.x) * 0.5f, row.cy);
    Text(m_dl, valueSize, ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f), colour, text.c_str(), Outline::Thick,
             m_alpha * (dim ? 0.7f : 1.0f));
    if (row.clicked && row.usable) {
        if (std::fabs(m_in.mouse.x - cl.x) < r * 1.3f && m_in.mouse.x < cl.x + r * 1.3f) return -1;
        if (std::fabs(m_in.mouse.x - cr.x) < r * 1.3f) return 1;
        return 2;
    }
    return 0;
}

void ListView::Section(const char* title) {
    const float h = (m_count == 0 && m_y < 8.0f) ? 44.0f : 58.0f;
    const float top = m_y;
    m_y += h;
    const float yTop = m_min.y + top * m_s - m_state.scroll;
    const float x0 = m_min.x + Px(44.0f) + m_xOff;
    const float x1 = m_max.x - Px(44.0f + 18.0f) + m_xOff;
    const float yBase = yTop + Px(h) - Px(8.0f);
    if (yBase < m_min.y || yTop > m_max.y) return;
    const ImVec2 ts = TextSize(24.0f, title);
    Text(m_dl, 24.0f, ImVec2(x0, yBase - ts.y - Px(6.0f)), kSection, title, Outline::Thick, m_alpha);
    m_dl->AddLine(ImVec2(x0, yBase), ImVec2(x1, yBase), Fade(IM_COL32(255, 255, 255, 90), m_alpha), std::max(1.0f, Px(1.5f)));
}

void ListView::Spacer(float heightU) { m_y += heightU; }

void ListView::Note(const std::string& text, ImU32 colour) {
    const float x0 = m_min.x + Px(44.0f) + m_xOff;
    const float x1 = m_max.x - Px(44.0f + 18.0f) + m_xOff;
    const float hPx = WrappedHeight(22.0f, x1 - x0, text);
    const float top = m_y;
    m_y += hPx / m_s + 14.0f;
    const float yTop = m_min.y + top * m_s - m_state.scroll + Px(4.0f);
    if (yTop + hPx < m_min.y || yTop > m_max.y) return;
    DrawWrapped(m_dl, 22.0f, ImVec2(x0, yTop), x1 - x0, colour, text, Outline::Thin, m_alpha);
}

void ListView::Info(const char* label, const std::string& value, ImU32 valueColour, bool middle) {
    const float top = m_y;
    const float x0 = m_min.x + Px(44.0f) + m_xOff;
    const float x1 = m_max.x - Px(44.0f + 18.0f) + m_xOff;
    const float valueX = x0 + (x1 - x0) * 0.50f;
    const float valueW = x1 - valueX;
    const float labelW = valueX - x0 - Px(16.0f);
    const float labelSize = FittedTextSize(label, 28.0f, 20.0f, labelW);
    const float valueSize = FittedTextSize(value, 28.0f, 20.0f, valueW);
    const float valueHeight = WrappedHeight(valueSize, valueW, value);
    const float rowHeightPx = std::max(Px(48.0f), valueHeight + Px(12.0f));
    m_y += rowHeightPx / m_s;
    const float yTop = m_min.y + top * m_s - m_state.scroll;
    const float cy = yTop + rowHeightPx * 0.5f;
    if (yTop + rowHeightPx < m_min.y || yTop > m_max.y) return;
    const ImVec2 labelSizePx = TextSize(labelSize, label);
    Text(m_dl, labelSize, ImVec2(x0, cy - labelSizePx.y * 0.5f), kTextDim, label, Outline::Thick, m_alpha);
    DrawWrapped(m_dl, valueSize, ImVec2(valueX, cy - valueHeight * 0.5f), valueW, valueColour, value,
                    Outline::Thick, m_alpha, false);
}

void ListView::Block(float heightU, const std::function<void(ImDrawList*, ImVec2, ImVec2)>& draw) {
    const float top = m_y;
    m_y += heightU + 8.0f;
    const float yTop = m_min.y + top * m_s - m_state.scroll;
    const ImVec2 mn(m_min.x + Px(44.0f) + m_xOff, yTop);
    const ImVec2 mx(m_max.x - Px(44.0f + 18.0f) + m_xOff, yTop + Px(heightU));
    if (mx.y < m_min.y || mn.y > m_max.y) return;
    draw(m_dl, mn, mx);
}

Change ListView::Toggle(const char* label, bool* value, bool def, const char* help, uint32_t flags,
                        const char* recommended) {
    Row row = Begin(RowKind::Toggle, 58.0f, help, flags, true);
    Change out = 0;
    if (row.focused && row.usable) {
        if (m_in.left && *value) { *value = false; out = kChanged | kCommit; }
        else if (m_in.right && !*value) { *value = true; out = kChanged | kCommit; }
        else if (m_in.accept) { *value = !*value; out = kChanged | kCommit; }
        else if (m_in.resetRow && *value != def) { *value = def; out = kChanged | kCommit; }
    }
    if (!row.visible) return out;
    DrawLabel(row, label, flags, recommended, "");
    const bool starred = recommended != nullptr && (std::strcmp(recommended, *value ? "On" : "Off") == 0);
    const int click = DrawChevronValue(row, *value ? "On" : "Off", *value, !*value, *value ? kWhite : kTextDim,
                                       (flags & kRowDisabled) != 0);
    if (starred) {
        const float w = TextSize(32.0f, *value ? "On" : "Off").x;
        DrawStar(m_dl, ImVec2((row.vx0 + row.x1) * 0.5f - w * 0.5f - Px(20.0f), row.cy), Px(10.0f), IM_COL32(120, 232, 130, 255));
    }
    if (click != 0) {
        if (click == -1 && *value) *value = false;
        else if (click == 1 && !*value) *value = true;
        else if (click == 2) *value = !*value;
        else return out;
        out = kChanged | kCommit;
    }
    if (recommended != nullptr && row.focused) m_focusedHelp += std::string(" Recommended: ") + recommended + ".";
    return out;
}

Change ListView::Choice(const char* label, int* index, const char* const* items, int count, int def, const char* help,
                        uint32_t flags, const char* recommended, const char* customText) {
    Row row = Begin(RowKind::Choice, 58.0f, help, flags, true);
    Change out = 0;
    auto set = [&](int v) {
        v = std::clamp(v, 0, count - 1);
        if (v != *index) {
            *index = v;
            out = kChanged | kCommit;
        }
    };
    if (row.focused && row.usable && count > 0) {
        if (m_in.left) set(*index < 0 ? count - 1 : *index - 1);
        else if (m_in.right) set(*index + 1);
        else if (m_in.accept) set(*index < 0 ? 0 : (*index + 1) % count);
        else if (m_in.resetRow && *index != def) set(def);
    }
    if (!row.visible) return out;
    DrawLabel(row, label, flags, recommended, "");
    const bool custom = *index < 0 || *index >= count;
    const std::string value = custom ? std::string(customText != nullptr ? customText : "Custom") : items[*index];
    const int click = DrawChevronValue(row, value, *index > 0, *index < count - 1, kWhite, (flags & kRowDisabled) != 0);
    const bool starred = recommended != nullptr && value == recommended;
    if (starred) {
        const float w = TextSize(32.0f, value.c_str()).x;
        const float cx = (row.vx0 + row.x1) * 0.5f;
        DrawStar(m_dl, ImVec2(cx - w * 0.5f - Px(20.0f), row.cy), Px(10.0f), IM_COL32(120, 232, 130, 255));
    }
    if (click == -1) set(*index - 1);
    else if (click == 1) set(*index + 1);
    else if (click == 2) set(*index < 0 ? 0 : (*index + 1) % count);
    if (recommended != nullptr && row.focused) m_focusedHelp += std::string(" Recommended: ") + recommended + ".";
    return out;
}

Change ListView::Slider(const char* label, float* value, float lo, float hi, float step, const char* format, float def,
                        const char* help, uint32_t flags, const char* recommended) {
    Row row = Begin(RowKind::Slider, 58.0f, help, flags, true);
    Change out = 0;
    auto snap = [&](float v) {
        if (step > 0.0f) v = lo + std::round((v - lo) / step) * step;
        return std::clamp(v, lo, hi);
    };
    auto setValue = [&](float v, Change flagsOut) {
        v = snap(v);
        if (std::fabs(v - *value) > 1e-6f) {
            *value = v;
            out |= flagsOut | kChanged;
        }
    };
    if (row.focused && row.usable) {
        const float delta = step * static_cast<float>(m_in.adjustMul);
        if (m_in.left) setValue(*value - delta, kCommit);
        else if (m_in.right) setValue(*value + delta, kCommit);
        else if (m_in.resetRow && std::fabs(*value - def) > 1e-6f) { *value = def; out = kChanged | kCommit; }
    }
    const float r = Px(21.0f);
    const float trackX0 = row.vx0 + r * 2.0f + Px(18.0f);
    const float trackX1 = row.x1 - r * 2.0f - Px(18.0f) - Px(122.0f);
    const bool dragging = m_state.dragRow == row.index;
    if (row.usable && row.visible) {
        const ImVec2 mn(trackX0 - Px(10.0f), row.cy - Px(20.0f));
        const ImVec2 mx(trackX1 + Px(10.0f), row.cy + Px(20.0f));
        if (m_in.mouseClicked && PointIn(m_in.mouse, mn, mx)) m_state.dragRow = row.index;
    }
    if (m_state.dragRow == row.index) {
        if (m_in.mouseDown && m_in.mouseValid) {
            const float t = std::clamp((m_in.mouse.x - trackX0) / std::max(1.0f, trackX1 - trackX0), 0.0f, 1.0f);
            setValue(lo + t * (hi - lo), 0);
        } else {
            out |= kCommit;
            m_state.dragRow = -1;
        }
    }
    if (!row.visible) return out;
    DrawLabel(row, label, flags, recommended, "");
    const bool dim = (flags & kRowDisabled) != 0;
    if (row.focused || row.hovered) {
        const bool hotL = row.hovered && std::fabs(m_in.mouse.x - (row.vx0 + r)) < r * 1.3f;
        const bool hotR = row.hovered && std::fabs(m_in.mouse.x - (row.x1 - r)) < r * 1.3f;
        DrawChevron(m_dl, ImVec2(row.vx0 + r, row.cy), r, -1, hotL, *value > lo && row.usable, m_alpha);
        DrawChevron(m_dl, ImVec2(row.x1 - r, row.cy), r, 1, hotR, *value < hi && row.usable, m_alpha);
        if (row.clicked && row.usable && m_state.dragRow != row.index) {
            if (hotL) setValue(*value - step, kCommit);
            else if (hotR) setValue(*value + step, kCommit);
        }
    }
    const float t = hi > lo ? std::clamp((*value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    const float th = Px(10.0f);
    const ImVec2 tmn(trackX0, row.cy - th * 0.5f);
    const ImVec2 tmx(trackX1, row.cy + th * 0.5f);
    const float ta = m_alpha * (dim ? 0.6f : 1.0f);
    m_dl->AddRectFilled(tmn, tmx, Fade(IM_COL32(8, 28, 56, 175), ta), th * 0.5f);
    const float fx = trackX0 + (trackX1 - trackX0) * t;
    if (fx > trackX0 + 1.0f) {
        m_dl->AddRectFilled(tmn, ImVec2(fx, tmx.y), Fade(row.focused ? IM_COL32(255, 255, 255, 240) : IM_COL32(205, 228, 246, 230), ta),
                            th * 0.5f);
    }
    m_dl->AddRect(ImVec2(tmn.x - 1, tmn.y - 1), ImVec2(tmx.x + 1, tmx.y + 1), Fade(IM_COL32(255, 255, 255, 120), ta),
                  th * 0.5f + 1.0f, 0, 1.0f);
    const float kr = Px(dragging ? 14.0f : 12.0f);
    m_dl->AddCircleFilled(ImVec2(fx, row.cy + 1.5f), kr + 1.5f, Fade(IM_COL32(0, 16, 40, 90), ta), 24);
    m_dl->AddCircleFilled(ImVec2(fx, row.cy), kr, Fade(kWhite, ta), 24);
    m_dl->AddCircleFilled(ImVec2(fx, row.cy), kr * 0.62f, Fade(IM_COL32(60, 132, 232, 255), ta), 20);
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, format, static_cast<double>(*value));
    const ImVec2 ts = TextSize(30.0f, buffer);
    Text(m_dl, 30.0f, ImVec2(row.x1 - r * 2.0f - Px(18.0f) - ts.x, row.cy - ts.y * 0.5f), kWhite, buffer,
             Outline::Thick, ta);
    if (recommended != nullptr && row.focused) m_focusedHelp += std::string(" Recommended: ") + recommended + ".";
    return out;
}

bool ListView::Action(const char* label, const std::string& valueText, const char* help, uint32_t flags) {
    Row row = Begin(RowKind::Action, 58.0f, help, flags, false);
    bool fired = false;
    if (row.focused && row.usable && m_in.accept) fired = true;
    if (row.clicked && row.usable) fired = true;
    if (!row.visible) return fired;
    DrawLabel(row, label, flags, nullptr, "");
    if (!valueText.empty()) {
        const float inset = Px(21.0f);
        const ImVec2 mn(row.vx0 + inset * 0.5f, row.cy - Px(22.0f));
        const ImVec2 mx(row.x1 - inset * 0.5f, row.cy + Px(22.0f));
        const bool dim = (flags & kRowDisabled) != 0;
        DrawPill(m_dl, mn, mx, row.focused ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 28),
                 row.focused ? IM_COL32(255, 255, 255, 220) : IM_COL32(255, 255, 255, 110), m_alpha * (dim ? 0.6f : 1.0f),
                 std::max(1.5f, Px(2.0f)));
        const std::string text = Ellipsize(valueText, 28.0f, mx.x - mn.x - Px(30.0f), true);
        TextCentered(m_dl, 28.0f, ImVec2((mn.x + mx.x) * 0.5f, row.cy), row.focused ? kWhite : kTextNormal, text.c_str(),
                         Outline::Thick, m_alpha * (dim ? 0.7f : 1.0f));
    }
    return fired;
}

int ListView::Bindings(const char* label, const std::array<std::string, 3>& names, const char* help, int* cleared,
                       uint32_t flags) {
    Row row = Begin(RowKind::Bindings, 58.0f, help, flags, true);
    int fired = -1;
    if (cleared != nullptr) *cleared = -1;
    const float gap = Px(10.0f);
    const float cellW = (row.x1 - row.vx0 - gap * 2.0f) / 3.0f;
    auto cellRect = [&](int i, ImVec2& mn, ImVec2& mx) {
        mn = ImVec2(row.vx0 + (cellW + gap) * static_cast<float>(i), row.cy - Px(22.0f));
        mx = ImVec2(mn.x + cellW, row.cy + Px(22.0f));
    };
    if (row.focused && row.usable) {
        if (m_in.left) m_state.cell = std::max(0, m_state.cell - 1);
        if (m_in.right) m_state.cell = std::min(2, m_state.cell + 1);
        if (m_in.accept) fired = m_state.cell;
        if (m_in.resetRow && cleared != nullptr) *cleared = m_state.cell;
    }
    if (row.visible && row.usable) {
        for (int i = 0; i < 3; ++i) {
            ImVec2 mn, mx;
            cellRect(i, mn, mx);
            if (!PointIn(m_in.mouse, mn, mx) || !m_in.mouseValid) continue;
            if (m_in.mouseMoved && row.focused) m_state.cell = i;
            if (row.clicked) {
                m_state.cell = i;
                fired = i;
            }
            if (row.rightClicked && cleared != nullptr) *cleared = i;
        }
    }
    if (!row.visible) return fired;
    DrawLabel(row, label, flags, nullptr, "");
    for (int i = 0; i < 3; ++i) {
        ImVec2 mn, mx;
        cellRect(i, mn, mx);
        const bool sel = row.focused && m_state.cell == i;
        const bool empty = names[static_cast<size_t>(i)].empty() || names[static_cast<size_t>(i)] == "-";
        DrawPill(m_dl, mn, mx, sel ? IM_COL32(255, 255, 255, 78) : IM_COL32(8, 30, 62, 70),
                 sel ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 80), m_alpha, sel ? std::max(1.5f, Px(2.2f)) : 1.0f);
        const std::string text = Ellipsize(empty ? std::string("-") : names[static_cast<size_t>(i)], 23.0f, cellW - Px(24.0f));
        TextCentered(m_dl, 23.0f, ImVec2((mn.x + mx.x) * 0.5f, row.cy), empty ? kTextDim : (sel ? kWhite : kTextNormal),
                         text.c_str(), Outline::Thin, m_alpha);
    }
    return fired;
}

Change ListView::Tabs(const char* const* names, int count, int* selected, const char* help) {
    Row row = Begin(RowKind::Tabs, 62.0f, help, kRowNone, false);
    Change out = 0;
    if (row.focused && row.usable) {
        if (m_in.left && *selected > 0) { --*selected; out = kChanged | kCommit; }
        if (m_in.right && *selected < count - 1) { ++*selected; out = kChanged | kCommit; }
    }
    if (!row.visible) return out;
    const float gap = Px(10.0f);
    float total = 0.0f;
    std::vector<float> widths;
    for (int i = 0; i < count; ++i) {
        widths.push_back(TextSize(27.0f, names[i]).x + Px(44.0f));
        total += widths.back() + (i + 1 < count ? gap : 0.0f);
    }
    float x = row.x0 + std::max(0.0f, (row.x1 - row.x0 - total) * 0.5f);
    for (int i = 0; i < count; ++i) {
        const ImVec2 mn(x, row.cy - Px(22.0f));
        const ImVec2 mx(x + widths[static_cast<size_t>(i)], row.cy + Px(22.0f));
        const bool sel = i == *selected;
        const bool hot = row.hovered && m_in.mouseValid && PointIn(m_in.mouse, mn, mx);
        DrawPill(m_dl, mn, mx,
                 sel ? IM_COL32(20, 62, 120, 235) : (hot ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 30)),
                 sel ? IM_COL32(255, 255, 255, 245) : IM_COL32(255, 255, 255, 110), m_alpha, sel ? std::max(1.5f, Px(2.2f)) : 1.0f);
        TextCentered(m_dl, 27.0f, ImVec2((mn.x + mx.x) * 0.5f, row.cy), sel ? kWhite : kTextNormal, names[i],
                         Outline::Thick, m_alpha);
        if (row.clicked && hot && i != *selected) {
            *selected = i;
            out = kChanged | kCommit;
        }
        x += widths[static_cast<size_t>(i)] + gap;
    }
    return out;
}

void ListView::End() {
    if (m_ended) return;
    m_ended = true;
    m_dl->PopClipRect();
    const float viewH = m_max.y - m_min.y;
    const float contentH = m_y + 6.0f;
    const float contentPx = contentH * m_s;
    if (contentPx > viewH + 1.0f) {
        const float trackX = m_max.x - Px(30.0f) + m_xOff;
        const float tw = Px(8.0f);
        const ImVec2 tmn(trackX, m_min.y + Px(6.0f));
        const ImVec2 tmx(trackX + tw, m_max.y - Px(6.0f));
        m_dl->AddRectFilled(tmn, tmx, Fade(IM_COL32(8, 28, 56, 90), m_alpha), tw * 0.5f);
        const float span = tmx.y - tmn.y;
        const float thumb = std::max(Px(36.0f), span * viewH / contentPx);
        const float maxScroll = contentPx - viewH;
        const float pos = maxScroll > 0.0f ? (m_state.scroll / maxScroll) * (span - thumb) : 0.0f;
        m_dl->AddRectFilled(ImVec2(tmn.x, tmn.y + pos), ImVec2(tmx.x, tmn.y + pos + thumb), Fade(IM_COL32(255, 255, 255, 170), m_alpha),
                            tw * 0.5f);
    }
    m_state.count = m_count;
    m_state.contentHeight = contentH;
    m_state.top = std::move(m_top);
    m_state.bottom = std::move(m_bottom);
    if (m_state.count > 0) m_state.focus = std::clamp(m_state.focus, 0, m_state.count - 1);
}

// ---- modal dialogs -----------------------------------------------------------------------------------

namespace {

bool g_modalHot = false;

struct ConfirmState {
    bool open = false;
    std::string title, message, yes;
    std::function<void()> onYes;
    bool danger = false;
    ModalState modal;
} g_confirm;

} // namespace

int DrawModal(ModalState& st, Frame& in, const ModalSpec& spec) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 disp = io.DisplaySize;
    if (!st.open) {
        st.open = true;
        st.focus = spec.defaultButton;
        st.anim = 0.0f;
        Suppress(0.22f);
    }
    st.anim = std::min(1.0f, st.anim + in.dt * 7.0f);
    const float a = Ease(st.anim);
    dl->AddRectFilled(ImVec2(0, 0), disp, Fade(IM_COL32(0, 6, 18, 150), a));

    const float pad = Px(46.0f);
    const float boxW = std::min(Px(860.0f), disp.x * 0.92f);
    const float inner = boxW - pad * 2.0f;
    float textH = 0.0f;
    for (const std::string& line : spec.lines) textH += WrappedHeight(27.0f, inner, line) + Px(10.0f);
    const float titleH = Px(64.0f);
    const float progressH = spec.progress >= 0.0f ? Px(40.0f) : 0.0f;
    const float buttonH = Px(58.0f);
    const float hintH = Px(52.0f);
    const float boxH = pad * 0.8f + titleH + textH + progressH + buttonH + hintH + pad * 0.5f;
    const float slide = (1.0f - a) * Px(24.0f);
    const ImVec2 mn((disp.x - boxW) * 0.5f, (disp.y - boxH) * 0.5f + slide);
    const ImVec2 mx(mn.x + boxW, mn.y + boxH);
    DrawGlassPanel(dl, mn, mx, a, 8.0f, 34.0f);

    float y = mn.y + pad * 0.8f;
    TextCentered(dl, 42.0f, ImVec2((mn.x + mx.x) * 0.5f, y + titleH * 0.4f), kTitleYellow, spec.title.c_str(),
                     Outline::Thick, a);
    y += titleH;
    for (const std::string& line : spec.lines) {
        y += DrawWrapped(dl, 27.0f, ImVec2(mn.x + pad, y), inner, kWhite, line, Outline::Thick, a, true) + Px(10.0f);
    }
    if (spec.progress >= 0.0f) {
        const ImVec2 bmn(mn.x + pad, y + Px(6.0f));
        const ImVec2 bmx(mx.x - pad, y + Px(24.0f));
        dl->AddRectFilled(bmn, bmx, Fade(IM_COL32(8, 28, 56, 170), a), Px(9.0f));
        const float t = std::clamp(spec.progress, 0.0f, 1.0f);
        dl->AddRectFilled(bmn, ImVec2(bmn.x + (bmx.x - bmn.x) * t, bmx.y), Fade(IM_COL32(255, 232, 80, 255), a), Px(9.0f));
        dl->AddRect(bmn, bmx, Fade(IM_COL32(255, 255, 255, 140), a), Px(9.0f), 0, 1.0f);
        y += progressH;
    }

    const int n = static_cast<int>(spec.buttons.size());
    const float bw = std::min(Px(250.0f), (inner - Px(24.0f) * static_cast<float>(std::max(0, n - 1))) / static_cast<float>(std::max(1, n)));
    const float totalW = bw * static_cast<float>(n) + Px(24.0f) * static_cast<float>(std::max(0, n - 1));
    float bx = (mn.x + mx.x) * 0.5f - totalW * 0.5f;
    int chosen = -1;
    bool anyHot = false;
    for (int i = 0; i < n; ++i) {
        const ImVec2 b0(bx, y + Px(6.0f));
        const ImVec2 b1(bx + bw, y + Px(6.0f) + buttonH);
        const bool hot = in.mouseValid && PointIn(in.mouse, b0, b1);
        anyHot |= hot;
        if (hot && in.mouseMoved && !spec.mouseOnly) st.focus = i;
        if (hot && in.mouseClicked) chosen = i;
        const bool sel = st.focus == i && (!spec.mouseOnly || hot);
        DrawPill(dl, b0, b1, sel ? IM_COL32(255, 255, 255, 110) : IM_COL32(10, 40, 84, 140),
                 sel ? IM_COL32(255, 255, 255, 250) : IM_COL32(255, 255, 255, 120), a, sel ? std::max(2.0f, Px(3.0f)) : 1.5f);
        TextCentered(dl, 30.0f, ImVec2((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f), sel ? kWhite : kTextNormal,
                         spec.buttons[static_cast<size_t>(i)].c_str(), Outline::Thick, a);
        bx += bw + Px(24.0f);
    }
    g_modalHot = anyHot;
    const float hintY = mx.y - pad * 0.5f - hintH * 0.35f;
    if (spec.mouseOnly || !spec.hint.empty()) {
        if (!spec.hint.empty()) {
            TextCentered(dl, 22.0f, ImVec2((mn.x + mx.x) * 0.5f, hintY), kTextNormal, spec.hint.c_str(), Outline::Thin, a);
        }
    } else {
        const bool pad_ = in.device == Device::Gamepad;
        std::vector<std::pair<std::string, std::string>> hints;
        hints.push_back({pad_ ? "A" : "Enter", "Select"});
        if (spec.cancelButton >= 0) hints.push_back({pad_ ? "B" : "Esc", spec.buttons[static_cast<size_t>(spec.cancelButton)]});
        DrawHints(dl, ImVec2((mn.x + mx.x) * 0.5f, hintY), hints, a, 1);
    }

    if (!spec.mouseOnly && chosen < 0) {
        if (in.left) st.focus = std::max(0, st.focus - 1);
        if (in.right) st.focus = std::min(n - 1, st.focus + 1);
        if (in.accept) chosen = st.focus;
        else if (in.back && spec.cancelButton >= 0) chosen = spec.cancelButton;
    }
    if (chosen >= 0) {
        if (!spec.sticky) st = ModalState{};
        return chosen;
    }
    return -1;
}

bool ModalHot() { return g_modalHot; }

void RequestConfirm(std::string title, std::string message, std::string yesLabel, std::function<void()> onYes,
                    bool danger) {
    g_confirm = ConfirmState{};
    g_confirm.open = true;
    g_confirm.title = std::move(title);
    g_confirm.message = std::move(message);
    g_confirm.yes = std::move(yesLabel);
    g_confirm.onYes = std::move(onYes);
    g_confirm.danger = danger;
    Suppress(0.25f);
}

bool ConfirmOpen() { return g_confirm.open; }

void DrawConfirm(Frame& input) {
    if (!g_confirm.open) return;
    ModalSpec spec;
    spec.title = g_confirm.title;
    spec.lines = {g_confirm.message};
    spec.buttons = {g_confirm.yes, "Cancel"};
    spec.defaultButton = 1;
    spec.cancelButton = 1;
    const int result = DrawModal(g_confirm.modal, input, spec);
    if (result < 0) return;
    std::function<void()> action = result == 0 ? g_confirm.onYes : std::function<void()>();
    g_confirm = ConfirmState{};
    if (action) action();
}

float DrawHudBox(ImDrawList* dl, ImVec2 anchor, const std::vector<std::pair<std::string, ImU32>>& lines, float alpha) {
    if (lines.empty()) return 0.0f;
    const float pad = Px(12.0f);
    float w = 0.0f;
    float h = pad * 2.0f;
    for (const auto& line : lines) {
        w = std::max(w, TextSize(22.0f, line.first.c_str()).x);
        h += FontPx(22.0f) * 1.2f;
    }
    w += pad * 2.0f;
    const ImVec2 mn(anchor.x - w, anchor.y);
    const ImVec2 mx(anchor.x, anchor.y + h);
    dl->AddRectFilled(mn, mx, Fade(IM_COL32(10, 36, 72, 175), alpha), Px(14.0f));
    dl->AddRect(mn, mx, Fade(IM_COL32(255, 255, 255, 130), alpha), Px(14.0f), 0, std::max(1.0f, Px(1.6f)));
    float y = mn.y + pad;
    for (const auto& line : lines) {
        Text(dl, 22.0f, ImVec2(mn.x + pad, y), line.second, line.first.c_str(), Outline::Thin, alpha);
        y += FontPx(22.0f) * 1.2f;
    }
    return h;
}

} // namespace game_ui
