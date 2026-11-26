#pragma once
#include "aim_settings.h"

namespace RemoteActions {
struct Buttons {
    uint16_t core = 0;
    uint8_t classic4 = 0, classic5 = 0;
    bool c = false, z = false, recenter = false;
    float lt = 0, rt = 0;
};

// Shared by the real remote encoder and the regression test. Inputs have
// already passed through deadzone/trigger conditioning in AimInput::Sample.
inline Buttons Resolve(const AimSettings::Settings& s, const InputMap::RawSample& raw,
                       AimSettings::Scheme scheme, bool mirrorClassicMenus,
                       bool menuNavigation = false) {
    using A = AimSettings::Action;
    Buttons out;
    const auto val = [&](A a) { return AimSettings::ActionValue(s,a,raw); };
    const auto down = [&](A a) { return val(a)>=0.5f; };
    const bool classic = scheme == AimSettings::Scheme::Classic;
    auto core = [&](bool pressed,uint16_t bit) { if(pressed) out.core |= bit; };
    auto c4 = [&](bool pressed,uint8_t bit) { if(pressed) out.classic4 |= bit; };
    auto c5 = [&](bool pressed,uint8_t bit) { if(pressed) out.classic5 |= bit; };
    const bool mouseLeft = (raw.mouseButtons & 1u) != 0;
    const bool escDown = raw.keys[41];
    const bool backspaceDown = raw.keys[42];
    const bool menuConfirm = down(A::Confirm) || (menuNavigation && mouseLeft);
    const bool menuCancel = down(A::Cancel) || (menuNavigation && (escDown || backspaceDown));
    auto pauseDown = [&]() {
        if (!menuNavigation) return down(A::Pause);
        const size_t i = static_cast<size_t>(A::Pause);
        if (InputMap::SlotsValue(s.bindings.pad[i], raw) >= 0.5f) return true;
        for (const InputMap::Binding& b : s.bindings.kbm[i]) {
            if (b.device == InputMap::Device::Key && b.code == 41) continue;
            if (InputMap::Value(b, raw) >= 0.5f) return true;
        }
        return false;
    };
    const bool pause = pauseDown();
    if (classic) {
        c4(down(A::Jump),0x20);
        if (!menuNavigation) c4(down(A::Fire),0x02);
        c4(pause,0x04);c4(down(A::Minus),0x10);c4(down(A::Home),0x08);
        c4(down(A::DpadDown),0x40);c4(down(A::DpadRight),0x80);
        c5(down(A::Evade),0x80);
        if (!menuNavigation) c5(down(A::LockOn),0x04);
        c5(menuConfirm,0x10);c5(menuCancel,0x40);
        c5(down(A::ButtonX),0x08);c5(down(A::ButtonY),0x20);c5(down(A::DpadUp),0x01);c5(down(A::DpadLeft),0x02);
        out.lt=val(A::Jump);
        out.rt=menuNavigation ? 0.f : val(A::Fire);
    } else {
        if (!menuNavigation) {
            core(down(A::Fire),0x0004);core(down(A::LockOn),0x0008);
        }
        core(down(A::ButtonX),0x0002);core(down(A::ButtonY),0x0001);
        out.c=down(A::Jump);out.z=down(A::Evade);
    }
    if (!classic || mirrorClassicMenus) {
        core(menuConfirm,0x0008);core(menuCancel,0x0004);
        core(pause,0x1000);core(down(A::Minus),0x0010);core(down(A::Home),0x0080);
        core(down(A::DpadUp),0x0800);core(down(A::DpadDown),0x0400);
        core(down(A::DpadLeft),0x0100);core(down(A::DpadRight),0x0200);
    }
    out.recenter=down(A::Recenter);
    return out;
}
}
