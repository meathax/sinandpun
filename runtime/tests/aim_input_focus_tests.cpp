#include "aim_input.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

SDL_Window* aurora_get_window() { return nullptr; }
bool aurora_get_present_viewport(float*, float*, float*, float*) { return false; }
SDL_Gamepad* PADGetSDLGamepadForIndex(u32) { return nullptr; }

namespace VirtualMouse {
void Tick() {}
} // namespace VirtualMouse

namespace VirtualRemote {
void SetScheme(InputMap::Scheme) {}
} // namespace VirtualRemote

static void Send(SDL_EventType type) {
    SDL_Event event{};
    event.type = static_cast<Uint32>(type);
    AimInput::HandleSdlEvent(event);
}

static void TestFocusLossInvalidatesAbsoluteMouse() {
    SDL_Event motion{};
    motion.type = static_cast<Uint32>(SDL_EVENT_MOUSE_MOTION);
    motion.motion.x = 731.f;
    motion.motion.y = 219.f;
    motion.motion.xrel = 17.f;
    motion.motion.yrel = -9.f;
    AimInput::HandleSdlEvent(motion);

    const AimInput::MouseSample beforeLoss = AimInput::MouseNow();
    CHECK(beforeLoss.valid);
    CHECK(beforeLoss.x == 731.f && beforeLoss.y == 219.f);

    Send(SDL_EVENT_WINDOW_FOCUS_LOST);
    CHECK(!AimInput::Focused());
    const AimInput::MouseSample afterLoss = AimInput::MouseNow();
    CHECK(!afterLoss.valid);

    float consumedX = -1.f, consumedY = -1.f;
    if (afterLoss.valid) {
        consumedX = afterLoss.x;
        consumedY = afterLoss.y;
    }
    CHECK(consumedX == -1.f && consumedY == -1.f);

    Send(SDL_EVENT_WINDOW_FOCUS_GAINED);
    CHECK(AimInput::Focused());
    CHECK(!AimInput::MouseNow().valid);

    motion.motion.x = 144.f;
    motion.motion.y = 288.f;
    AimInput::HandleSdlEvent(motion);
    const AimInput::MouseSample fresh = AimInput::MouseNow();
    CHECK(fresh.valid);
    CHECK(fresh.x == 144.f && fresh.y == 288.f);
}

int main() {
    TestFocusLossInvalidatesAbsoluteMouse();
    if (g_failures == 0) std::puts("aim_input_focus_tests: all passed");
    return g_failures == 0 ? 0 : 1;
}
