#pragma once

// Drawing and input layer for the in-game options menu. Everything is drawn straight into ImGui draw lists
// (no ImGui windows or widgets) so the menu can match the game's own blue glass UI: rounded white-rimmed panel,
// yellow outlined titles, white outlined item text, chevron buttons on adjustable rows, pill tabs.
//
// Layout units ("u") are 1080p pixels; Scale() = window height / 1080, so the same code serves 720p to 4K.
// The input model is controller first (d-pad / stick / A / B / X / Y / LB / RB) with keyboard and mouse
// mirroring it. Everything here runs on the thread that calls settings_overlay::Draw().

#include <imgui.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace game_ui {

// ---- colours ---------------------------------------------------------------------------------------
inline ImU32 Fade(ImU32 colour, float alpha) {
    const float a = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) * (alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha);
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kTextNormal = IM_COL32(214, 230, 242, 255);
inline constexpr ImU32 kTextDim = IM_COL32(196, 218, 236, 255);
inline constexpr ImU32 kTextOutline = IM_COL32(12, 30, 52, 235);
inline constexpr ImU32 kTitleYellow = IM_COL32(255, 238, 40, 255);
inline constexpr ImU32 kTitleOutline = IM_COL32(70, 56, 0, 245);
inline constexpr ImU32 kSection = IM_COL32(255, 240, 160, 255);
inline constexpr ImU32 kWarn = IM_COL32(255, 196, 80, 255);
inline constexpr ImU32 kGood = IM_COL32(150, 238, 150, 255);
inline constexpr ImU32 kBad = IM_COL32(255, 130, 120, 255);

// ---- scale and font --------------------------------------------------------------------------------
// Adds the bundled UI font to the ImGui atlas. Idempotent; safe to call before the first frame or lazily.
void InstallFont();
// False for the frame in which the atlas still has to be rebuilt; drawing then uses ImGui's default font.
bool FontReady();
ImFont* Font();
float Scale();
inline float Px(float u) { return u * Scale(); }

// ---- text ------------------------------------------------------------------------------------------
enum class Outline { None, Thin, Thick };

ImVec2 TextSize(float sizeU, const char* text);
void Text(ImDrawList* dl, float sizeU, ImVec2 pos, ImU32 colour, const char* text, Outline outline = Outline::Thick,
              float alpha = 1.0f);
void TextCentered(ImDrawList* dl, float sizeU, ImVec2 centre, ImU32 colour, const char* text,
                      Outline outline = Outline::Thick, float alpha = 1.0f);
void TextRight(ImDrawList* dl, float sizeU, ImVec2 rightTop, ImU32 colour, const char* text,
                   Outline outline = Outline::Thick, float alpha = 1.0f);
// Word-wrapped; returns the height used in pixels.
float DrawWrapped(ImDrawList* dl, float sizeU, ImVec2 pos, float maxWidthPx, ImU32 colour, const std::string& text,
                  Outline outline = Outline::Thin, float alpha = 1.0f, bool centered = false);
float WrappedHeight(float sizeU, float maxWidthPx, const std::string& text);
std::string Ellipsize(const std::string& text, float sizeU, float maxWidthPx, bool middle = false);

// ---- input -----------------------------------------------------------------------------------------
enum class Device { Gamepad, Keyboard, Mouse };

// One snapshot per ImGui frame. Navigation events already include key repeat.
struct Frame {
    float dt = 0.0f;
    bool up = false, down = false, left = false, right = false;
    bool accept = false, back = false, apply = false;
    bool resetRow = false, resetPage = false;
    bool tabPrev = false, tabNext = false;
    bool pageUp = false, pageDown = false;
    int adjustMul = 1;  // grows while left/right stays held, for sliders
    bool navFresh = false;  // the up/down/left/right event this frame is a first press, not a repeat
    bool anyNav = false;
    ImVec2 mouse{-1.0f, -1.0f};
    bool mouseValid = false;
    bool mouseMoved = false;
    bool mouseClicked = false;   // left press this frame
    bool mouseDown = false;
    bool mouseReleased = false;
    bool rightClicked = false;
    float wheel = 0.0f;
    Device device = Device::Gamepad;
};

// Polls gamepad (any connected pad), keyboard and mouse once per ImGui frame.
Frame& PollInput();
// Ignore accept/back/etc. for a short time; used right after a layer opens so the opening press is not reused.
void Suppress(float seconds);
Device LastDevice();
bool PointIn(ImVec2 p, ImVec2 min, ImVec2 max);

// ---- primitives ------------------------------------------------------------------------------------
void DrawGlassPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha, float rimU = 9.0f, float radiusU = 40.0f);
void DrawChevron(ImDrawList* dl, ImVec2 centre, float radiusPx, int dir, bool hot, bool enabled, float alpha);
void DrawPill(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 fill, ImU32 rim, float alpha, float rimPx = 1.5f);
enum class Badge { Restart, Recommended, Experimental, Confirm, Neutral };
// Returns the width used in pixels. `pos` is the top-left corner.
float DrawBadge(ImDrawList* dl, ImVec2 pos, const char* text, Badge kind, float alpha);
float BadgeWidth(const char* text);
// A round controller glyph ("A") or rounded key cap ("LB", "Enter"). Returns the width used.
float DrawGlyph(ImDrawList* dl, ImVec2 leftCentre, const char* text, float alpha, bool round = false);
float GlyphWidth(const char* text, bool round = false);
void DrawStar(ImDrawList* dl, ImVec2 centre, float radiusPx, ImU32 colour);
// Footer-style button hints: {glyph, label} pairs, e.g. {"A", "Select"}. Single-letter glyphs are drawn round.
// Returns the total width in pixels; `align` 0 = left edge at pos.x, 1 = centred on pos.x, 2 = right edge at pos.x.
float DrawHints(ImDrawList* dl, ImVec2 pos, const std::vector<std::pair<std::string, std::string>>& hints, float alpha,
                int align = 0);

// ---- list ------------------------------------------------------------------------------------------
enum RowFlags : uint32_t {
    kRowNone = 0,
    kRowDisabled = 1u << 0,
    kRowConfirm = 1u << 2,       // change goes through the keep/revert countdown
    kRowExperimental = 1u << 3,
    kRowIndent = 1u << 4,
    kRowDanger = 1u << 5,
    kRowNextLaunch = 1u << 6,
};

using Change = unsigned;
inline constexpr Change kChanged = 1u;  // value changed: apply it
inline constexpr Change kCommit = 2u;   // user finished editing: persist it

enum class RowKind { None, Toggle, Choice, Slider, Action, Bindings, Tabs, Info };

struct ListState {
    int focus = 0;
    int cell = 0;  // focused cell of a Bindings row
    float scroll = 0.0f;
    float scrollTarget = 0.0f;
    float hiTop = 0.0f;
    float hiHeight = 0.0f;
    bool hiInit = false;
    int count = 0;
    float contentHeight = 0.0f;
    int dragRow = -1;
    std::vector<float> top;     // content-space row tops of the previous frame (u)
    std::vector<float> bottom;
    void Reset() { *this = ListState{}; }
};

class ListView {
public:
    ListView(ListState& state, Frame& input, ImDrawList* dl, ImVec2 areaMin, ImVec2 areaMax, bool interactive,
             float alpha, float xOffsetPx);
    ~ListView();
    ListView(const ListView&) = delete;
    ListView& operator=(const ListView&) = delete;

    void Section(const char* title);
    void Spacer(float heightU);
    void Note(const std::string& text, ImU32 colour = kTextDim);
    void Info(const char* label, const std::string& value, ImU32 valueColour = kTextNormal, bool ellipsizeMiddle = false);
    void Block(float heightU, const std::function<void(ImDrawList*, ImVec2, ImVec2)>& draw);

    Change Toggle(const char* label, bool* value, bool def, const char* help, uint32_t flags = 0,
                  const char* recommended = nullptr);
    // `index` may be -1 together with `customText` to show a value that is not in `items`.
    Change Choice(const char* label, int* index, const char* const* items, int count, int def, const char* help,
                  uint32_t flags = 0, const char* recommended = nullptr, const char* customText = nullptr);
    Change Slider(const char* label, float* value, float lo, float hi, float step, const char* format, float def,
                  const char* help, uint32_t flags = 0, const char* recommended = nullptr);
    // True when activated. `valueText` is shown in the value column.
    bool Action(const char* label, const std::string& valueText, const char* help, uint32_t flags = 0);
    // Three-cell binding row. Returns the activated cell (0..2) or -1; `cleared` is set to the cell cleared with X/right click.
    int Bindings(const char* label, const std::array<std::string, 3>& names, const char* help, int* cleared,
                 uint32_t flags = 0);
    // Pill strip as a focusable row; left/right (or a click) changes the selection.
    Change Tabs(const char* const* names, int count, int* selected, const char* help);

    void End();

    RowKind FocusedKind() const { return m_focusedKind; }
    const std::string& FocusedHelp() const { return m_focusedHelp; }
    bool FocusedResettable() const { return m_focusedResettable; }
    bool HasFocus() const { return m_count > 0; }

private:
    struct Row;
    Row Begin(RowKind kind, float heightU, const char* help, uint32_t flags, bool resettable);
    void DrawLabel(const Row& row, const char* label, uint32_t flags, const char* recommended, const std::string& valueText);
    int DrawChevronValue(const Row& row, const std::string& value, bool canLeft, bool canRight, ImU32 colour,
                         bool dim);

    ListState& m_state;
    Frame& m_in;
    ImDrawList* m_dl;
    ImVec2 m_min, m_max;
    bool m_interactive;
    float m_alpha;
    float m_xOff;
    float m_s;
    float m_y = 0.0f;   // content-space cursor (u)
    int m_count = 0;
    bool m_ended = false;
    bool m_focusMoved = false;
    bool m_mouseInside = false;
    std::vector<float> m_top, m_bottom;
    RowKind m_focusedKind = RowKind::None;
    std::string m_focusedHelp;
    bool m_focusedResettable = false;
};

// ---- modal dialogs ---------------------------------------------------------------------------------
struct ModalState {
    bool open = false;
    int focus = -1;
    float anim = 0.0f;
};

struct ModalSpec {
    std::string title;
    std::vector<std::string> lines;
    std::vector<std::string> buttons;
    int defaultButton = 0;
    int cancelButton = -1;      // chosen by B / Esc
    float progress = -1.0f;     // 0..1 countdown bar when >= 0
    bool mouseOnly = false;     // pad/keyboard are owned by something else (press-to-bind capture)
    bool sticky = false;        // keep the dialog open (no re-open animation) after a button is chosen
    std::string hint;
};

// Draws on top of everything and returns the chosen button index, or -1 while open.
int DrawModal(ModalState& state, Frame& input, const ModalSpec& spec);
// True when the pointer was over a modal button during the latest DrawModal() call.
bool ModalHot();

// Generic yes/no confirmation shared by the whole menu.
void RequestConfirm(std::string title, std::string message, std::string yesLabel, std::function<void()> onYes,
                    bool danger = false);
bool ConfirmOpen();
void DrawConfirm(Frame& input);

// A small glass pill for the FPS counter and similar HUD bits; returns its height in pixels.
float DrawHudBox(ImDrawList* dl, ImVec2 anchorTopRight, const std::vector<std::pair<std::string, ImU32>>& lines,
                 float alpha = 1.0f);

} // namespace game_ui
