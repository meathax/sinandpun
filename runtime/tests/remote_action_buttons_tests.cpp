#include "remote_action_buttons.h"
#include <cstdio>

static int failures=0;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);++failures; } } while(0)
int main() {
    using A=AimSettings::Action;
    using S=AimSettings::Scheme;
    auto settings=AimSettings::DefaultBindings();
    InputMap::RawSample raw;
    for (S scheme : {S::Pointer,S::Classic}) {
        raw={};raw.padAxes[5]=1;
        auto fire=RemoteActions::Resolve(settings,raw,scheme,true);
        CHECK(scheme==S::Pointer ? fire.core==0x0004 : fire.classic4==0x02 && fire.rt==1 && fire.classic5==0);
        raw={};raw.padButtons[0]=true;
        auto a=RemoteActions::Resolve(settings,raw,scheme,true);
        CHECK((a.core&0x0004)==0 && (a.classic4&0x02)==0); // A must never shoot.
        CHECK(scheme==S::Pointer ? a.core==0x0008 : a.classic5==0x10);
        raw={};raw.padAxes[4]=1;
        auto lock=RemoteActions::Resolve(settings,raw,scheme,true);
        CHECK(scheme==S::Pointer ? lock.core==0x0008 : lock.classic5==0x04);
        raw={};raw.padButtons[10]=true;
        auto jump=RemoteActions::Resolve(settings,raw,scheme,true);
        CHECK(scheme==S::Pointer ? jump.c : jump.classic4==0x20);
        raw={};raw.padButtons[9]=true;
        auto evade=RemoteActions::Resolve(settings,raw,scheme,true);
        CHECK(scheme==S::Pointer ? evade.z : evade.classic5==0x80);
        raw={};raw.mouseButtons=1;
        fire=RemoteActions::Resolve(settings,raw,scheme,true);
        CHECK(scheme==S::Pointer ? fire.core==0x0004 : fire.classic4==0x02);
    }
    raw={};raw.keys[41]=true;
    CHECK(RemoteActions::Resolve(settings,raw,S::Pointer,true).core==0x1000);
    // Witness the reported old-layout defect using the same production resolver.
    raw={};raw.padButtons[0]=true;
    CHECK((RemoteActions::Resolve(AimSettings::LegacyBindings(),raw,S::Pointer,true).core&4)!=0);
    // A real reassignment must release the old trigger, activate the new button,
    // and swap the previous owner rather than silently firing two actions.
    InputMap::AssignSwap(settings.bindings.pad,static_cast<size_t>(A::Fire),0,InputMap::PadButton(2));
    raw={};raw.padAxes[5]=1;
    CHECK((RemoteActions::Resolve(settings,raw,S::Classic,true).classic4&2)==0);
    raw={};raw.padButtons[2]=true;
    CHECK(RemoteActions::Resolve(settings,raw,S::Classic,true).classic4==2);
    CHECK(InputMap::CountConflicts(settings.bindings.pad)==0);
    settings.bindings.kbm[static_cast<size_t>(A::Fire)]={InputMap::MouseButton(InputMap::kMouseWheelDown)};
    raw={};raw.mouseButtons=1;
    CHECK((RemoteActions::Resolve(settings,raw,S::Pointer,true).core&4)==0);
    raw={};raw.wheelDown=true;
    CHECK(RemoteActions::Resolve(settings,raw,S::Pointer,true).core==4);
    raw={};raw.padAxes[5]=0.1f;
    CHECK(AimSettings::ActionValue(AimSettings::DefaultBindings(),A::Fire,raw)<0.5f);

    // Menu navigation controls (mouse left = select, right = suppressed, Esc/Backspace = back, Return = select).
    const auto menuSettings = AimSettings::DefaultBindings();
    for (S scheme : {S::Pointer, S::Classic}) {
        raw={};raw.mouseButtons=1;
        auto lclick=RemoteActions::Resolve(menuSettings,raw,scheme,true,true);
        if (scheme==S::Pointer) {
            CHECK(lclick.core==0x0008 && (lclick.core&0x0004)==0);
        } else {
            CHECK(lclick.classic5==0x10 && (lclick.classic4&0x02)==0 && lclick.rt==0.f);
        }
        raw={};raw.mouseButtons=4;
        auto rclick=RemoteActions::Resolve(menuSettings,raw,scheme,true,true);
        CHECK((rclick.core&(0x0008|0x0004))==0 && rclick.classic4==0 && rclick.classic5==0);
        raw={};raw.keys[41]=true; // Escape -> Back (B), not Pause (+)
        auto esc=RemoteActions::Resolve(menuSettings,raw,scheme,true,true);
        if (scheme==S::Pointer) {
            CHECK(esc.core==0x0004 && (esc.core&0x1000)==0);
        } else {
            CHECK(esc.classic5==0x40 && (esc.classic4&0x04)==0);
        }
        raw={};raw.keys[42]=true; // Backspace -> Back (B)
        auto bs=RemoteActions::Resolve(menuSettings,raw,scheme,true,true);
        CHECK(scheme==S::Pointer ? bs.core==0x0004 : bs.classic5==0x40);
        raw={};raw.keys[40]=true; // Return -> Select (A)
        auto ret=RemoteActions::Resolve(menuSettings,raw,scheme,true,true);
        CHECK(scheme==S::Pointer ? ret.core==0x0008 : ret.classic5==0x10);
        raw={};raw.keys[19]=true; // P -> Pause (+) still works in menus
        auto pkey=RemoteActions::Resolve(menuSettings,raw,scheme,true,true);
        CHECK(scheme==S::Pointer ? pkey.core==0x1000 : pkey.classic4==0x04);
    }
    // Combat (menuNavigation = false) keeps mouse:left -> B, mouse:right -> A, Escape -> Pause (+).
    raw={};raw.mouseButtons=1;
    CHECK(RemoteActions::Resolve(menuSettings,raw,S::Pointer,true,false).core==0x0004);
    raw={};raw.mouseButtons=4;
    CHECK(RemoteActions::Resolve(menuSettings,raw,S::Pointer,true,false).core==0x0008);
    raw={};raw.keys[41]=true;
    CHECK(RemoteActions::Resolve(menuSettings,raw,S::Pointer,true,false).core==0x1000);

    if(!failures)std::puts("remote_action_buttons_tests: old A-fire reproduced; RT/LT/LB/RB, keyboard/mouse, menu nav, remaps and releases passed");
    return failures?1:0;
}
