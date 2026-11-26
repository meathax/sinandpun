#include "button_prompt_atlas.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

static int failures=0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); ++failures; } } while(0)
static std::array<uint8_t,4> Pixel(const ButtonPrompts::Atlas& a,int x,int y) {
    const int base=((y/4)*64+x/4)*64, s=((y%4)*4+x%4)*2;
    return {a[base+s+1],a[base+32+s],a[base+33+s],a[base+s]};
}
static bool TileEquals(const ButtonPrompts::Atlas& a,int frame,int style,int glyph) {
    const int cell=ButtonPrompts::kCells[frame];
    for (int y=0;y<32;++y) for (int x=0;x<32;++x) {
        const auto p=Pixel(a,(cell%8)*32+x,(cell/8)*32+y);
        for (int c=0;c<4;++c) if(p[c]!=ButtonPrompts::kGlyphPixels[style][glyph][(y*32+x)*4+c])return false;
    }
    return true;
}
static bool FrameEqualSD(const ButtonPrompts::Atlas& a,int frameA,const ButtonPrompts::Atlas& b,int frameB) {
    const int ca=ButtonPrompts::kCells[frameA], cb=ButtonPrompts::kCells[frameB];
    for (int y=0;y<32;++y) for (int x=0;x<32;++x) {
        if (Pixel(a,(ca%8)*32+x,(ca/8)*32+y)!=Pixel(b,(cb%8)*32+x,(cb/8)*32+y)) return false;
    }
    return true;
}
static bool FrameNonEmptySD(const ButtonPrompts::Atlas& a,int frame) {
    const int c=ButtonPrompts::kCells[frame];
    for (int y=0;y<32;++y) for (int x=0;x<32;++x)
        if (Pixel(a,(c%8)*32+x,(c/8)*32+y)[3]>128) return true;
    return false;
}
static bool FrameEqualHD(const std::vector<uint8_t>& a,int frameA,const std::vector<uint8_t>& b,int frameB) {
    const int ca=ButtonPrompts::kCells[frameA], cb=ButtonPrompts::kCells[frameB];
    constexpr int N=ButtonPrompts::kHdAtlasSize, G=ButtonPrompts::kGlyphHdSize;
    for (int y=0;y<G;++y) for (int x=0;x<G;++x) {
        const size_t oa=(size_t((ca/8)*G+y)*N+(ca%8)*G+x)*4;
        const size_t ob=(size_t((cb/8)*G+y)*N+(cb%8)*G+x)*4;
        for (int c=0;c<4;++c) if (a[oa+c]!=b[ob+c]) return false;
    }
    return true;
}
static bool FrameNonEmptyHD(const std::vector<uint8_t>& a,int frame) {
    const int cell=ButtonPrompts::kCells[frame];
    constexpr int N=ButtonPrompts::kHdAtlasSize, G=ButtonPrompts::kGlyphHdSize;
    for (int y=0;y<G;++y) for (int x=0;x<G;++x) {
        const size_t o=(size_t((cell/8)*G+y)*N+(cell%8)*G+x)*4;
        if (a[o+3]>128) return true;
    }
    return false;
}
static void Dump(const char* path,const ButtonPrompts::Atlas& a) {
    std::ofstream f(path,std::ios::binary);
    for(int y=0;y<256;++y)for(int x=0;x<256;++x){auto p=Pixel(a,x,y);f.write(reinterpret_cast<const char*>(p.data()),4);}
}
int main(int argc,char** argv) {
    using A=AimSettings::Action;
    auto s=AimSettings::DefaultBindings();
    static ButtonPrompts::Atlas xbox,ps,remap,unbound,kbMenu,kbCombat,kbRemap;
    ButtonPrompts::BuildAtlas(s,xbox);
    CHECK(TileEquals(xbox,9,0,0)); // Classic A = modern host A (south).
    CHECK(TileEquals(xbox,10,0,1));
    CHECK(TileEquals(xbox,25,0,10));CHECK(TileEquals(xbox,26,0,16));
    CHECK(TileEquals(xbox,27,0,9));CHECK(TileEquals(xbox,28,0,15));
    CHECK(TileEquals(xbox,31,0,6));
    CHECK(TileEquals(xbox,0,0,0) && TileEquals(xbox,1,0,1));
    ButtonPrompts::BuildAtlas(s,remap,true);
    CHECK(TileEquals(remap,0,0,15) && TileEquals(remap,1,0,16));
    s.buttonPromptStyle=AimSettings::ButtonPromptStyle::PlayStation;
    ButtonPrompts::BuildAtlas(s,ps);
    CHECK(ps!=xbox);CHECK(TileEquals(ps,9,1,0));CHECK(TileEquals(ps,10,1,1));
    s.bindings.pad[static_cast<size_t>(A::Confirm)]={InputMap::PadButton(2)};
    ButtonPrompts::BuildAtlas(s,remap);
    CHECK(TileEquals(remap,0,1,2));CHECK(TileEquals(remap,9,1,2));CHECK(TileEquals(remap,26,1,16));
    s.bindings.pad[static_cast<size_t>(A::Confirm)]={};
    ButtonPrompts::BuildAtlas(s,unbound);
    CHECK(TileEquals(unbound,9,1,20));
    std::array<bool,64> seen{};
    for(int c:ButtonPrompts::kCells){CHECK(!seen[c]);seen[c]=true;}
    for(int y=0;y<256;++y)for(int x=0;x<256;++x)
        if(!seen[(y/32)*8+x/32])CHECK(Pixel(xbox,x,y)[3]==0);

    // Keyboard & Mouse prompts (SD + HD, menu vs combat, remapped keys).
    auto kbs=AimSettings::DefaultBindings();
    kbs.buttonPromptStyle=AimSettings::ButtonPromptStyle::Keyboard;
    CHECK(kbs.IsKeyboardPrompts() && !kbs.IsPlayStationPrompts() && !kbs.IsWiiPrompts());
    ButtonPrompts::BuildAtlas(kbs,kbMenu,false);
    ButtonPrompts::BuildAtlas(kbs,kbCombat,true);
    CHECK(kbMenu!=xbox && kbMenu!=ps);
    CHECK(kbMenu!=kbCombat);
    // All 32 active cells rendered, all unused cells transparent.
    for(int f=0;f<32;++f) CHECK(FrameNonEmptySD(kbMenu,f) && FrameNonEmptySD(kbCombat,f));
    for(int y=0;y<256;++y)for(int x=0;x<256;++x)
        if(!seen[(y/32)*8+x/32]) CHECK(Pixel(kbMenu,x,y)[3]==0);
    // Menu vs combat context: frame 0 (Confirm LMB vs LockOn RMB) and frame 1 (Cancel BKSP vs Fire LMB) differ.
    CHECK(!FrameEqualSD(kbMenu,0,kbCombat,0));
    CHECK(!FrameEqualSD(kbMenu,1,kbCombat,1));
    // Distinct actions produce distinct glyphs.
    CHECK(!FrameEqualSD(kbCombat,2,kbCombat,3));  // Jump (SPC) vs Evade (SHFT)
    CHECK(!FrameEqualSD(kbCombat,5,kbCombat,6));  // Pause (ESC) vs Minus (TAB)
    CHECK(!FrameEqualSD(kbCombat,4,kbCombat,17)); // Move (WASD) vs Aim (Mouse)
    // HD KBM atlas matches expected mip-chain size and context differences.
    std::vector<uint8_t> kbHdMenu,kbHdCombat,kbHdRemap;
    ButtonPrompts::BuildAtlasHD(kbs,kbHdMenu,false);
    ButtonPrompts::BuildAtlasHD(kbs,kbHdCombat,true);
    size_t expectedHdBytes=0;
    for(int m=0;m<ButtonPrompts::kHdAtlasMips;++m)
        expectedHdBytes+=size_t(ButtonPrompts::kHdAtlasSize>>m)*(ButtonPrompts::kHdAtlasSize>>m)*4;
    CHECK(kbHdMenu.size()==expectedHdBytes && kbHdCombat.size()==expectedHdBytes);
    for(int f=0;f<32;++f) CHECK(FrameNonEmptyHD(kbHdMenu,f) && FrameNonEmptyHD(kbHdCombat,f));
    CHECK(!FrameEqualHD(kbHdMenu,0,kbHdCombat,0));
    CHECK(!FrameEqualHD(kbHdMenu,1,kbHdCombat,1));
    // Remapping KBM bindings updates both SD and HD glyphs.
    auto kbsRemap=kbs;
    kbsRemap.bindings.kbm[static_cast<size_t>(A::Jump)]={InputMap::Key(9)};    // F
    kbsRemap.bindings.kbm[static_cast<size_t>(A::Confirm)]={InputMap::Key(9)}; // F
    kbsRemap.bindings.kbm[static_cast<size_t>(A::Fire)]={InputMap::Key(10)};   // G
    ButtonPrompts::BuildAtlas(kbsRemap,kbRemap,true);
    ButtonPrompts::BuildAtlasHD(kbsRemap,kbHdRemap,true);
    CHECK(!FrameEqualSD(kbCombat,2,kbRemap,2));   // Jump changed from SPC to F
    CHECK(!FrameEqualSD(kbCombat,1,kbRemap,1));   // Fire changed from LMB to G
    CHECK(!FrameEqualHD(kbHdCombat,2,kbHdRemap,2));
    CHECK(!FrameEqualHD(kbHdCombat,1,kbHdRemap,1));
    ButtonPrompts::BuildAtlas(kbsRemap,kbRemap,false);
    CHECK(!FrameEqualSD(kbMenu,0,kbRemap,0));     // Menu Confirm changed from LMB to F
    // Original Wii prompt style enum & parsing.
    auto wiis=AimSettings::DefaultBindings();
    wiis.buttonPromptStyle=AimSettings::ButtonPromptStyle::Wii;
    CHECK(wiis.IsWiiPrompts() && !wiis.IsPlayStationPrompts() && !wiis.IsKeyboardPrompts());
    CHECK(AimSettings::ParseButtonPromptStyle("wii")==AimSettings::ButtonPromptStyle::Wii);
    CHECK(AimSettings::ParseButtonPromptStyle("keyboard")==AimSettings::ButtonPromptStyle::Keyboard);
    CHECK(AimSettings::ParseButtonPromptStyle("playstation")==AimSettings::ButtonPromptStyle::PlayStation);

    if(argc==2){
        std::ifstream f(argv[1],std::ios::binary);
        const std::vector<char> b{std::istreambuf_iterator<char>(f),{}};
        CHECK(b.size()==262400);
        if(b.size()==262400) CHECK(XXH64(b.data()+192,262144,0)==0x7d9cbe2a23379258ull);
    }
    Dump("xbox-atlas.rgba",xbox);Dump("playstation-atlas.rgba",ps);
    if(!failures)std::puts("button_prompt_tests: atlas identity, mappings, remaps, styles, tiled RGBA and empty cells passed");
    return failures?1:0;
}
