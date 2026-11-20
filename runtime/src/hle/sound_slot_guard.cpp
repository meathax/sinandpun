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
// Called from the translator-inserted GuestEntryHook for 0x8003B950 (projects/r2ve01/recomp.yml translation.entry_hooks).

#include "guest_abi/dispatch.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

namespace {

constexpr uint32_t kGameOverSlot = 5;
constexpr uint32_t kGameOverMusicId = 1901;
constexpr uint32_t kGameOverFlags = 2;
constexpr uint32_t kGameOverRate = 0xBB80;
constexpr uint32_t kLoadMusic = 0x8003B7F4u;  // fn_8003B7F4(musicId, slot, flags, rate) -> slot
constexpr uint32_t kEntryStride = 68;
constexpr uint32_t kEntryStateOffset = 40;
constexpr uint32_t kEntryUnloaded = 5;

bool GuardRd32(uint32_t address, uint32_t& value) {
    if (!Memory::Contains(address, 4)) {
        return false;
    }
    value = Memory::Read32(address);
    return true;
}

}  // namespace

void SinSoundSlotGuard(CpuContext* ctx) {
    static int s_budget = 8;
    uint32_t slotPtr = ctx->gpr[3];
    uint32_t slot = 0, smallData = 0, soundMgr = 0, entries = 0, state = 0;
    if (!GuardRd32(slotPtr, slot) || slot != kGameOverSlot) {
        return;
    }
    if (!GuardRd32(ctx->gpr[13] - 23904u, smallData) || !GuardRd32(smallData + 92u, soundMgr) ||
        !GuardRd32(soundMgr + 1004u, entries) || !GuardRd32(entries + slot * kEntryStride + kEntryStateOffset, state)) {
        return;
    }
    if (state != kEntryUnloaded) {
        return;
    }
    if (s_budget > 0) {
        --s_budget;
        RT_LOGF(RT_TAG_HLE, "sound-slot-guard: slot %u is unloaded at the readiness check; requesting music %u\n", slot,
                kGameOverMusicId);
    }
    // Call the game's own loader, preserving every register the interrupted function may still need.
    const uint32_t savedGpr[13] = {ctx->gpr[0], ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5],
                                   ctx->gpr[6], ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10], ctx->gpr[11],
                                   ctx->gpr[12]};
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = kGameOverMusicId;
    ctx->gpr[4] = kGameOverSlot;
    ctx->gpr[5] = kGameOverFlags;
    ctx->gpr[6] = kGameOverRate;
    InvokeIndirectCpu(kLoadMusic, ctx);
    for (int i = 0; i < 13; ++i) {
        ctx->gpr[i] = savedGpr[i];
    }
    ctx->lr = savedLr;
}
