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
                       AimSettings::Scheme scheme, bool mirrorClassicMenus) {
    using A = AimSettings::Action;
    Buttons out;
    const auto val = [&](A a) { return AimSettings::ActionValue(s,a,raw); };
    const auto down = [&](A a) { return val(a)>=0.5f; };
    const bool classic = scheme == AimSettings::Scheme::Classic;
    auto core = [&](bool pressed,uint16_t bit) { if(pressed) out.core |= bit; };
    auto c4 = [&](A a,uint8_t bit) { if(down(a)) out.classic4 |= bit; };
    auto c5 = [&](A a,uint8_t bit) { if(down(a)) out.classic5 |= bit; };
    if (classic) {
        c4(A::Jump,0x20);c4(A::Fire,0x02);c4(A::Pause,0x04);c4(A::Minus,0x10);c4(A::Home,0x08);
        c4(A::DpadDown,0x40);c4(A::DpadRight,0x80);
        c5(A::Evade,0x80);c5(A::LockOn,0x04);c5(A::Confirm,0x10);c5(A::Cancel,0x40);
        c5(A::ButtonX,0x08);c5(A::ButtonY,0x20);c5(A::DpadUp,0x01);c5(A::DpadLeft,0x02);
        out.lt=val(A::Jump);out.rt=val(A::Fire);
    } else {
        core(down(A::Fire),0x0004);core(down(A::LockOn),0x0008);
        core(down(A::ButtonX),0x0002);core(down(A::ButtonY),0x0001);
        out.c=down(A::Jump);out.z=down(A::Evade);
    }
    if (!classic || mirrorClassicMenus) {
        core(down(A::Confirm),0x0008);core(down(A::Cancel),0x0004);
        core(down(A::Pause),0x1000);core(down(A::Minus),0x0010);core(down(A::Home),0x0080);
        core(down(A::DpadUp),0x0800);core(down(A::DpadDown),0x0400);
        core(down(A::DpadLeft),0x0100);core(down(A::DpadRight),0x0200);
    }
    out.recenter=down(A::Recenter);
    return out;
}
}
