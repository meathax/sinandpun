// Opens the recomp settings overlay from the game's own OPTIONS menu entry (title menu and pause menu).
//
// Nothing here replaces game logic. Two guest functions are wrapped (they stay translated and are called
// through MKW_GUEST_NEXT), and a wrapper only observes the menu object it is handed, then runs the
// original. Evidence and the full reverse-engineering notes: docs/MENU-HOOKS.md.
//
// The game's menus are data driven (ctl::IconMenu). A page is a descriptor in guest .data:
//   { flags, id, msgId, 0 } header, { flags, 1, handlerFn, 0 } record, then 16-byte item records.
// handlerFn(menu) is called by the menu update (fn_800E83B4 -> tail call at 0x800E93E4, per frame) and once
// at construction (fn_800D97DC -> tail call at 0x800DA014, menu+0x84 bit 31 set = "first call").
//   0x8010BC48  handler of the TITLE main menu's  "Options" page   (descriptor 0x802DA158, msg 169)
//   0x8010C1E4  handler of the PAUSE menu's       "Options" page   (descriptor 0x802DA1E8, msg 163)
//   0x8010C6C8  handler of the title main menu itself ("Select an option": 1P/2P/Options/Rankings)
// A page handler only runs while its page exists, so the first call after a gap = the player chose OPTIONS.
//
// Environment:
//   SIN_MENU_HOOK=0           disable the hook entirely (stock game menu)
//   SIN_MENU_HOOK_LOG=1       log every page-handler entry (rate limited) with menu pointer and flags
//   SIN_OPEN_OPTIONS=1        debug: open the overlay once when the title main menu has been up ~30 frames
//   SIN_OPEN_OPTIONS_AT=<s>   debug: open the overlay <s> seconds after launch, in whatever scene is showing

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"
#include "settings_overlay.h"
#include "virtual_remote.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

void SinSoundSlotGuard(CpuContext* ctx);  // hle/sound_slot_guard.cpp

namespace {

// Debug aid: SIN_OPEN_OPTIONS_AT=<seconds> opens the overlay from a timer, so it can be inspected in any scene.
struct TimedOpen {
    TimedOpen() {
        const char* v = std::getenv("SIN_OPEN_OPTIONS_AT");
        if (!v || !*v) {
            return;
        }
        const int seconds = std::atoi(v);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            RT_LOGF(RT_TAG_HLE, "game-menu: SIN_OPEN_OPTIONS_AT=%d -> overlay\n", seconds);
            settings_overlay::OpenFromGame();
        }).detach();
    }
} g_timedOpen;

constexpr uint32_t kMenuFlagsOffset = 0x84;       // u32 menu flags; bit 31 set on the construction call
constexpr uint32_t kMenuFirstCallFlag = 0x80000000u;
constexpr auto kPageGap = std::chrono::milliseconds(250);  // longer than any per-frame call spacing
constexpr int kTitleMenuWarmupCalls = 30;

using Clock = std::chrono::steady_clock;

bool EnvFlag(const char* name, bool defaultValue) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return defaultValue;
    }
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0 || std::strcmp(v, "off") == 0);
}

const bool g_enabled = EnvFlag("SIN_MENU_HOOK", true);
const bool g_log = EnvFlag("SIN_MENU_HOOK_LOG", false);
const bool g_autoOpen = EnvFlag("SIN_OPEN_OPTIONS", false);

struct PageTracker {
    const char* name;
    uint32_t lastMenu = 0;
    Clock::time_point lastCall{};
    bool seen = false;
    int logBudget = 40;
};

uint32_t ReadFlags(uint32_t menu) {
    uint32_t flags = 0;
    if (menu != 0 && Memory::Contains(menu + kMenuFlagsOffset, 4)) {
        flags = Memory::Read32(menu + kMenuFlagsOffset);
    }
    return flags;
}

// True when this call is the first one after the page was (re)opened.
bool PageEntered(PageTracker& page, uint32_t menu, uint32_t flags) {
    const auto now = Clock::now();
    const bool entered = !page.seen || menu != page.lastMenu || (now - page.lastCall) > kPageGap;
    page.seen = true;
    page.lastMenu = menu;
    page.lastCall = now;
    if (g_log && (entered || (flags & kMenuFirstCallFlag)) && page.logBudget > 0) {
        --page.logBudget;
        RT_LOGF(RT_TAG_HLE, "game-menu %s: menu=0x%08X flags=0x%08X entered=%d overlayOpen=%d\n", page.name, menu,
                flags, entered ? 1 : 0, settings_overlay::IsOpenFromGame() ? 1 : 0);
    }
    return entered;
}

bool g_openedByHook = false;
bool g_sawOpen = false;

void OpenOverlayFromPage(PageTracker& page, uint32_t menu) {
    if (settings_overlay::IsOpenFromGame()) {
        return;
    }
    RT_LOGF(RT_TAG_HLE, "game-menu: OPTIONS chosen in the %s menu (menu object 0x%08X) -> recomp settings overlay\n",
            page.name, menu);
    settings_overlay::OpenFromGame();
    g_openedByHook = true;
    g_sawOpen = false;
    if (const char* v = std::getenv("SIN_MENU_AUTOCLOSE")) {  // debug: dismiss the overlay after <s> seconds
        const int seconds = std::atoi(v);
        if (seconds > 0) {
            std::thread([seconds] {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                settings_overlay::CloseFromGame();
            }).detach();
        }
    }
}

// The stock Options page is still alive under our overlay. When the overlay has been dismissed, press B once on the
// virtual remote so the page closes itself and the player lands back on the parent menu.
void ServicePostClose() {
    if (!g_openedByHook) {
        return;
    }
    if (settings_overlay::IsOpenFromGame()) {
        g_sawOpen = true;
    } else if (g_sawOpen) {
        g_openedByHook = false;
        g_sawOpen = false;
        RT_LOGF(RT_TAG_HLE, "game-menu: overlay closed -> closing the stock Options page (B)\n");
        VirtualRemote::InjectCancel(150);
    }
}

PageTracker g_titleOptions{"title"};
PageTracker g_pauseOptions{"pause"};
PageTracker g_titleRoot{"title-root"};
int g_titleRootCalls = 0;
bool g_autoOpened = false;

}  // namespace

static void TitleOptionsPage(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    ServicePostClose();
    if (PageEntered(g_titleOptions, menu, ReadFlags(menu))) {
        OpenOverlayFromPage(g_titleOptions, menu);
    }
}

static void PauseOptionsPage(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    ServicePostClose();
    if (PageEntered(g_pauseOptions, menu, ReadFlags(menu))) {
        OpenOverlayFromPage(g_pauseOptions, menu);
    }
}

// Title main menu ("Select an option": 1P Start / 2P / Options / Rankings). Observation and debug auto-open only.
static void TitleRoot(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    const bool entered = PageEntered(g_titleRoot, menu, ReadFlags(menu));
    if (entered) {
        g_titleRootCalls = 0;
    }
    ++g_titleRootCalls;
    if (g_autoOpen && !g_autoOpened && g_titleRootCalls >= kTitleMenuWarmupCalls) {
        g_autoOpened = true;
        RT_LOGF(RT_TAG_HLE, "game-menu: SIN_OPEN_OPTIONS=1, title menu reached (menu 0x%08X) -> overlay\n", menu);
        settings_overlay::OpenFromGame();
    }
}

// Called by the translated handlers listed in projects/r2ve01/recomp.yml translation.entry_hooks. The menu page
// handlers are only reached through an indirect call, so a registry wrapper cannot see them.
void GuestEntryHook(uint32_t address, CpuContext* ctx) {
    // R2VE01 StrapMsg update: state 10 waits for the existing fade to finish,
    // then returns completion to the task owner. The owner still releases the
    // strap archive and runs controller/save initialization before the title.
    // Only redirect a newly constructed StrapMsg, never other task classes.
    if (address == 0x8008F408u && ctx != nullptr) {
        const uint32_t task = ctx->gpr[3];
        if (task != 0 && Memory::Contains(task, 0x54) &&
            Memory::Read32(task) == 0x802FB018u && Memory::Read8(task + 0x36) == 0) {
            Memory::Write8(task + 0x36, 10);
            RT_LOGF(RT_TAG_HLE, "startup: skipping StrapMsg control notices\n");
        }
        return;
    }
    if (address == 0x8003B950u && ctx != nullptr) {
        SinSoundSlotGuard(ctx);  // hle/sound_slot_guard.cpp
        return;
    }
    if (!g_enabled || ctx == nullptr) {
        return;
    }
    switch (address) {
    case 0x8010BC48u:
        TitleOptionsPage(ctx);
        break;
    case 0x8010C1E4u:
        PauseOptionsPage(ctx);
        break;
    case 0x8010C6C8u:
        TitleRoot(ctx);
        break;
    default:
        break;
    }
}
