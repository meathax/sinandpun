// Guards the game-over jingle slot against a latent bug in the original game.
//
// fn_8003B950(int* slotVar) asks whether a sound slot is ready. It follows entry.h (+44) and calls a virtual method on
// the object that handle points to. The unload code (0x80038AE8, 0x8003A088) frees that handle but never clears +44, so
// asking about an unloaded slot reads freed memory. On hardware the freed block usually still holds the old pointer and
// the call survives; here a later allocation reuses the block and the call jumps to garbage (docs/SWAP-CRASH-NOTES.md).
//
// The GAME OVER scene (fn_800F8014) polls slot 5 (music 1901, stream gover.brstm) 60 frames after it starts. In a death
// where the jingle load (fn_8003B7F4(1901, 5, 2, 0xBB80), fn_800F6830 / fn_800F823C) was not requested, slot 5 is still
// unloaded. When that is the case this hook requests the load the scene expects before the readiness check runs.
//
// Called from the translator-inserted GuestEntryHook for 0x8003B950 (projects/r2ve01/recomp.yml translation.entry_hooks;
// R2VJ01 0x80039538 in projects/r2vj01/recomp.yml). The slot table layout, unload state 5 and the readiness check are
// identical in R2VJ01, but its loader takes the stream file name instead of a music id: setPreGameOver (0x800EABE0)
// calls fn_80039448("gover.adx" @ 0x802CC3A0, 5, 2, 0xBB80).

#include "guest_abi/dispatch.h"
#include "guest_abi/guest_region.h"
#include "hle/sound_slot_guard.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

namespace {

constexpr uint32_t kGameOverSlot = 5;
constexpr uint32_t kGameOverMusicId = 1901;            // R2VE01 music id (stream gover.brstm)
constexpr uint32_t kGameOverStreamNameJapan = 0x802CC3A0u;  // R2VJ01 "gover.adx" in .rodata
constexpr uint32_t kGameOverFlags = 2;
constexpr uint32_t kGameOverRate = 0xBB80;
constexpr uint32_t kEntryStride = 68;
constexpr uint32_t kEntryStateOffset = 40;
constexpr uint32_t kEntryHandleOffset = 44;
constexpr uint32_t kEntryDataOffset = 48;

bool GuardRd32(uint32_t address, uint32_t& value) {
    return Memory::TryRead32(address, value);
}

}  // namespace

void SinSoundSlotGuard(CpuContext* ctx) {
    static int s_budget = 8;
    uint32_t slotPtr = ctx->gpr[3];
    uint32_t slot = 0, smallData = 0, soundMgr = 0, entries = 0, state = 0;
    if (!GuardRd32(slotPtr, slot) || slot != kGameOverSlot) {
        return;
    }
    // GM pointer: r13-23904 in R2VE01, r13-23936 in R2VJ01; use the region-mapped global directly.
    if (!GuardRd32(MKW_GADDR(804B19A0), smallData) || !GuardRd32(smallData + 92u, soundMgr) ||
        !GuardRd32(soundMgr + 1004u, entries)) {
        return;
    }
    const uint32_t resolvedEntry = entries + slot * kEntryStride;
    if (!GuardRd32(resolvedEntry + kEntryStateOffset, state) || state != SoundSlotGuard::kEntryUnloaded) {
        return;
    }
    if (s_budget > 0) {
        --s_budget;
        RT_LOGF(RT_TAG_HLE, "sound-slot-guard: slot %u is unloaded at the readiness check; requesting music %u\n", slot,
                kGameOverMusicId);
    }

    // The entry hook runs after the translated function has copied its live inputs but before its prologue. Restore
    // the complete context after the nested guest call, not only the argument registers: the loader legitimately
    // clobbers volatile GPRs, condition state, FP state, and LR.
    const CpuContext saved = *ctx;
    const bool japan = ::GuestRegion::Current() == ::GuestRegion::Id::NtscJ;
    ctx->gpr[3] = japan ? kGameOverStreamNameJapan : kGameOverMusicId;
    ctx->gpr[4] = kGameOverSlot;
    ctx->gpr[5] = kGameOverFlags;
    ctx->gpr[6] = kGameOverRate;

    uint32_t loadedSlot = 0xFFFFFFFFu;
    InvokeIndirectCpu(MKW_GADDR(8003B7F4), ctx);  // load music into slot (R2VJ01 0x80039448)
    loadedSlot = ctx->gpr[3];
    *ctx = saved;

    uint32_t postState = state, handle = 0, data = 0, vptr = 0;
    const bool readsSucceeded = GuardRd32(resolvedEntry + kEntryStateOffset, postState) &&
        GuardRd32(resolvedEntry + kEntryHandleOffset, handle) &&
        GuardRd32(resolvedEntry + kEntryDataOffset, data);
    const bool handleIsReadable = readsSucceeded && handle != 0 && GuardRd32(handle, vptr);
    const bool safe = readsSucceeded &&
        SoundSlotGuard::IsLoadResultSafe(slot, loadedSlot, postState, handle, data, handleIsReadable);

    if (s_budget > 0) {
        --s_budget;
        RT_LOGF(RT_TAG_HLE,
                "sound-slot-guard: loader %s result=%d state=%u handle=%08X vptr=%08X data=%08X\n",
                safe ? "succeeded" : "failed", static_cast<int32_t>(loadedSlot), postState, handle, vptr, data);
    }

    if (!safe && readsSucceeded) {
        // An unloaded entry has no valid object at +44; preserve the original function's control flow by pointing the
        // shared handle at an in-entry zero word. The readiness function treats a zero pointee as "not ready".
        if (Memory::TryWrite32(resolvedEntry + kEntryDataOffset, 0) &&
            Memory::TryWrite32(resolvedEntry + kEntryHandleOffset, resolvedEntry + kEntryDataOffset)) {
            if (s_budget > 0) {
                --s_budget;
                RT_LOGF(RT_TAG_HLE, "sound-slot-guard: installed unloaded-slot sentinel after loader failure\n");
            }
        } else if (s_budget > 0) {
            --s_budget;
            RT_LOGF(RT_TAG_HLE, "sound-slot-guard: failed to install unloaded-slot sentinel\n");
        }
    }
}
