#include "aim_input.h"
#include "aim_settings.h"

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
static SDL_Gamepad* secondPad=nullptr;
SDL_Gamepad* PADGetSDLGamepadForPort(u32 port) { return port == 0 ? testPad : port == 1 ? secondPad : nullptr; }

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

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--keyboard-p2") {
        RuntimeConfigFile::Mutable().secondRemote = 2;
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.scancode = SDL_SCANCODE_W;
        AimInput::HandleSdlEvent(key);
        CHECK(!AimInput::Sample(0).keys[SDL_SCANCODE_W]);
        CHECK(AimInput::Sample(1).keys[SDL_SCANCODE_W]);
        Send(SDL_EVENT_WINDOW_FOCUS_LOST);
        CHECK(!AimInput::Sample(1).keys[SDL_SCANCODE_W]);
        return g_failures ? 1 : 0;
    }
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
    const SDL_VirtualJoystickSensorDesc sensors[] = {{SDL_SENSOR_GYRO, 100.f}, {SDL_SENSOR_ACCEL, 100.f}};
    desc.nsensors = 2; desc.sensors = sensors;
    desc.SetSensorsEnabled = [](void*, bool) { return true; };
    const auto id=SDL_AttachVirtualJoystick(&desc);
    CHECK(id!=0);
    testPad=SDL_OpenGamepad(id);CHECK(testPad!=nullptr);
    const auto secondId = SDL_AttachVirtualJoystick(&desc);
    CHECK(secondId != 0);
    secondPad = SDL_OpenGamepad(secondId);
    CHECK(secondPad != nullptr);
    if (testPad && secondPad) {
        CHECK(SDL_SetJoystickVirtualAxis(SDL_GetGamepadJoystick(testPad), SDL_GAMEPAD_AXIS_LEFTX, 22000));
        CHECK(SDL_SetJoystickVirtualAxis(SDL_GetGamepadJoystick(secondPad), SDL_GAMEPAD_AXIS_LEFTX, -22000));
        SDL_UpdateJoysticks();
        CHECK(AimInput::Sample(0).padAxes[0] > 0.3f);
        CHECK(AimInput::Sample(1).padAxes[0] < -0.3f);
        SDL_CloseGamepad(secondPad);
        secondPad = nullptr;
        SDL_DetachVirtualJoystick(secondId);
        CHECK(AimInput::Sample(1).padAxes[0] == 0.f);
        CHECK(AimInput::Sample(0).padAxes[0] > 0.3f);
        SDL_SetJoystickVirtualAxis(SDL_GetGamepadJoystick(testPad), SDL_GAMEPAD_AXIS_LEFTX, 0);
        SDL_UpdateJoysticks();
    }
    if (testPad) {
        auto settings = AimSettings::DefaultBindings();
        settings.gyro.enabled = true; settings.gyro.space = 1;
        AimSettings::Set(settings);
        AimInput::UpdateWindowState(false);
        CHECK(AimInput::EffectiveScheme() == InputMap::Scheme::Pointer);
        CHECK(SDL_GamepadSensorEnabled(testPad, SDL_SENSOR_GYRO));
        SDL_Event sensor{}; sensor.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
        sensor.gsensor.which = id; sensor.gsensor.sensor = SDL_SENSOR_GYRO;
        sensor.gsensor.data[1] = 1.f;
        auto sample = [&](Uint64 time) { sensor.gsensor.sensor_timestamp = time; AimInput::HandleSdlEvent(sensor); };
        float gx, gy;
        sample(1000000000); sample(1010000000);
        AimInput::ConsumeGyroDelta(gx, gy); CHECK(gx < -1.f && gy == 0.f);
        AimInput::ConsumeGyroDelta(gx, gy); CHECK(gx == 0.f && gy == 0.f);
        sensor.gsensor.which = id + 100;
        sample(1020000000); AimInput::ConsumeGyroDelta(gx, gy); CHECK(gx == 0.f);
        sensor.gsensor.which = id;
        sample(1020000000); Send(SDL_EVENT_WINDOW_FOCUS_LOST);
        AimInput::ConsumeGyroDelta(gx, gy); CHECK(gx == 0.f && gy == 0.f);
        Send(SDL_EVENT_WINDOW_FOCUS_GAINED);
        AimInput::UpdateWindowState(true);
        sample(1030000000); sample(1040000000);
        AimInput::ConsumeGyroDelta(gx, gy); CHECK(gx == 0.f && gy == 0.f);
        AimInput::UpdateWindowState(false);
        sample(1050000000); sample(1060000000);
        AimInput::ConsumeGyroDelta(gx, gy); CHECK(gx < -1.f);
        settings.gyro.activation = 1; AimSettings::Set(settings);
        AimInput::UpdateWindowState(false);
        sample(1070000000); sample(1080000000);
        AimInput::ConsumeGyroDelta(gx, gy); CHECK(gx == 0.f);
        settings.gyro.enabled = false; AimSettings::Set(settings);
        AimInput::UpdateWindowState(false);
        CHECK(!SDL_GamepadSensorEnabled(testPad, SDL_SENSOR_GYRO));
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
