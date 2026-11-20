#pragma once

#include <cstdint>

namespace SoundSlotGuard {

constexpr uint32_t kEntryUnloaded = 5;

inline bool IsLoadResultSafe(uint32_t requestedSlot, uint32_t loadedSlot, uint32_t state, uint32_t handle, uint32_t data,
                             bool handleIsReadable) {
    return loadedSlot == requestedSlot && state != kEntryUnloaded && handle != 0 && data != 0 && handleIsReadable;
}

}  // namespace SoundSlotGuard
