// gx_module_natives.cpp - GX FIFO writers reached by the stage modules (stg00..stg07) that had no
// native at their Sin (R2VE01) address. See docs/MODULE-IMPORTS.md (tools/module_imports.py).
//
// The real functions write BP registers through the gather pipe (0xCC008000), which the runtime
// does not route, so the translated bodies would be silent no-ops. Addresses and signatures were
// verified against the guest bodies in main.dol (lis 0xCC01 / stb 0x61 / stw -0x8000) and the
// SDK name map (tools/sdk_name_map.txt, gx.a, diff=0).
#include "gx_internal.h"

// GXSetCopyClear (0x801D7DE0): void GXSetCopyClear(GXColor clear_clr, u32 clear_z).
// CodeWarrior passes the GXColor struct by reference, so r3 is a guest pointer to {r,g,b,a}
// (same convention as the cp argument of GXSetFog). Used by Ng::RiTex::captureFB paths.
extern "C" void GX__SetCopyClear_801d7de0(uint32_t clrPtr, uint32_t clearZ) {
    const GXColor color = DecodeGxColor(Memory::Read32(clrPtr));
    GxWorker::Run([color, clearZ] { GXSetCopyClear(color, clearZ); });
}
PPC_NATIVE_OVERRIDE_VOID(801D7DE0, GX__SetCopyClear_801d7de0, (uint32_t clrPtr, uint32_t clearZ), (clrPtr, clearZ));

// GXSetFogRangeAdj (0x801DAB20): void GXSetFogRangeAdj(GXBool enable, u16 center, GXFogAdjTable* table).
// table is a guest pointer to u16[10] (big-endian); only read when enabled, as in the SDK.
extern "C" void GX__SetFogRangeAdj_801dab20(uint32_t enable, uint32_t center, uint32_t tablePtr) {
    GXFogAdjTable table{};
    if (enable && tablePtr != 0) {
        for (uint32_t i = 0; i < 10; ++i) {
            table.r[i] = Memory::Read16(tablePtr + i * 2);
        }
    }
    GxWorker::Run([enable, center, table]() mutable {
        GXSetFogRangeAdj(static_cast<GXBool>(enable), static_cast<u16>(center), &table);
    });
}
PPC_NATIVE_OVERRIDE_VOID(801DAB20, GX__SetFogRangeAdj_801dab20, (uint32_t enable, uint32_t center, uint32_t tablePtr), (enable, center, tablePtr));
