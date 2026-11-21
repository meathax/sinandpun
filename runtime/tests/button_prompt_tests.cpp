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
static void Dump(const char* path,const ButtonPrompts::Atlas& a) {
    std::ofstream f(path,std::ios::binary);
    for(int y=0;y<256;++y)for(int x=0;x<256;++x){auto p=Pixel(a,x,y);f.write(reinterpret_cast<const char*>(p.data()),4);}
}
int main(int argc,char** argv) {
    using A=AimSettings::Action;
    auto s=AimSettings::DefaultBindings();
    static ButtonPrompts::Atlas xbox,ps,remap,unbound;
    ButtonPrompts::BuildAtlas(s,xbox);
    CHECK(TileEquals(xbox,9,0,0)); // Classic A = modern host A (south).
    CHECK(TileEquals(xbox,10,0,1));
    CHECK(TileEquals(xbox,25,0,9));CHECK(TileEquals(xbox,26,0,16));
    CHECK(TileEquals(xbox,27,0,10));CHECK(TileEquals(xbox,28,0,15));
    CHECK(TileEquals(xbox,31,0,6));
    s.playStationPrompts=true;
    ButtonPrompts::BuildAtlas(s,ps);
    CHECK(ps!=xbox);CHECK(TileEquals(ps,9,1,0));CHECK(TileEquals(ps,10,1,1));
    s.bindings.pad[static_cast<size_t>(A::Confirm)]={InputMap::PadButton(2)};
    ButtonPrompts::BuildAtlas(s,remap);
    CHECK(TileEquals(remap,9,1,2));CHECK(TileEquals(remap,26,1,16));
    s.bindings.pad[static_cast<size_t>(A::Confirm)]={};
    ButtonPrompts::BuildAtlas(s,unbound);
    CHECK(TileEquals(unbound,9,1,20));
    std::array<bool,64> seen{};
    for(int c:ButtonPrompts::kCells){CHECK(!seen[c]);seen[c]=true;}
    for(int y=0;y<256;++y)for(int x=0;x<256;++x)
        if(!seen[(y/32)*8+x/32])CHECK(Pixel(xbox,x,y)[3]==0);
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
