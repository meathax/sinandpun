#include "aim_input.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

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
static SDL_Gamepad* testPad=nullptr;
SDL_Gamepad* PADGetSDLGamepadForIndex(u32) { return testPad; }

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
    for (SDL_Scancode code : {SDL_SCANCODE_ESCAPE,SDL_SCANCODE_DELETE,SDL_SCANCODE_F1}) {
        AimInput::BeginCapture(InputMap::Profile::KeyboardMouse);
        SDL_Event event{};event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=code;
        AimInput::HandleSdlEvent(event);
        const auto result=AimInput::PollCapture(false);
        CHECK(result.state==AimInput::CaptureState::Done);
        CHECK(result.binding==InputMap::Key(code));
        event.type=SDL_EVENT_KEY_UP;AimInput::HandleSdlEvent(event);
    }
    AimInput::BeginCapture(InputMap::Profile::KeyboardMouse);
    SDL_Event reserved{};reserved.type=SDL_EVENT_KEY_DOWN;reserved.key.scancode=SDL_SCANCODE_F10;
    AimInput::HandleSdlEvent(reserved);
    CHECK(AimInput::PollCapture(false).state==AimInput::CaptureState::Pending);
    AimInput::CancelCapture();
    AimInput::BeginCapture(InputMap::Profile::KeyboardMouse);
    SDL_Event wheel{};wheel.type=SDL_EVENT_MOUSE_WHEEL;wheel.wheel.y=-1;
    AimInput::HandleSdlEvent(wheel);
    const auto captured=AimInput::PollCapture(false);
    CHECK(captured.state==AimInput::CaptureState::Done);
    CHECK(captured.binding==InputMap::MouseButton(InputMap::kMouseWheelDown));
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc{};SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<(SDL_GAMEPAD_BUTTON_DPAD_RIGHT+1))-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;desc.name="Sin isolated remap test";
    const auto id=SDL_AttachVirtualJoystick(&desc);
    CHECK(id!=0);
    testPad=SDL_OpenGamepad(id);CHECK(testPad!=nullptr);
    if (testPad) {
        auto* joystick=SDL_GetGamepadJoystick(testPad);
        SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true);SDL_UpdateJoysticks();
        AimInput::BeginCapture(InputMap::Profile::Gamepad);
        CHECK(AimInput::PollCapture(false).state==AimInput::CaptureState::Pending); // held opener is ignored
        SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,false);SDL_UpdateJoysticks();
        CHECK(AimInput::PollCapture(false).state==AimInput::CaptureState::Pending);
        SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_WEST,true);SDL_UpdateJoysticks();
        const auto button=AimInput::PollCapture(false);
        CHECK(button.state==AimInput::CaptureState::Done && button.binding==InputMap::PadButton(SDL_GAMEPAD_BUTTON_WEST));
        SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_WEST,false);SDL_UpdateJoysticks();
        AimInput::BeginCapture(InputMap::Profile::Gamepad);
        SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767);SDL_UpdateJoysticks();
        const auto trigger=AimInput::PollCapture(false);
        CHECK(trigger.state==AimInput::CaptureState::Done && trigger.binding==InputMap::PadAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,1));
        SDL_CloseGamepad(testPad);testPad=nullptr;
    }
    SDL_DetachVirtualJoystick(id);SDL_Quit();
    if (g_failures == 0) std::puts("aim_input_focus_tests: all passed");
    return g_failures == 0 ? 0 : 1;
}
