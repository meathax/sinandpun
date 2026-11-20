#include "hle/sound_slot_guard.h"

#include <cstdlib>

int main() {
    if (!SoundSlotGuard::IsLoadResultSafe(5, 5, 0, 0x92000000u, 0x93000000u, true)) {
        return EXIT_FAILURE;
    }
    if (SoundSlotGuard::IsLoadResultSafe(5, 0xFFFFFFFFu, 0, 0x92000000u, 0x93000000u, true)) {
        return EXIT_FAILURE;
    }
    if (SoundSlotGuard::IsLoadResultSafe(5, 5, SoundSlotGuard::kEntryUnloaded, 0x92000000u, 0x93000000u, true)) {
        return EXIT_FAILURE;
    }
    if (SoundSlotGuard::IsLoadResultSafe(5, 5, 0, 0, 0x93000000u, false)) {
        return EXIT_FAILURE;
    }
    if (SoundSlotGuard::IsLoadResultSafe(5, 5, 0, 0x92000000u, 0, true)) {
        return EXIT_FAILURE;
    }
    if (SoundSlotGuard::IsLoadResultSafe(5, 5, 0, 0x92000000u, 0x93000000u, false)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
