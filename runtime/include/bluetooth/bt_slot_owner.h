#pragma once

// Which backend (virtual gamepad remote or physical Wii Remote) brought a Bluetooth link up.
//
// The emulated controller keeps one link per slot. When the device behind a slot changes while that link is up
// (a real Wii Remote connects during play, or leaves), the guest must not keep talking to the new device through
// the old link: it negotiated the extension type, report mode and IR setup with the previous one, and the real
// host never sees its own link come up. CompositeHost reports the slot as detached until the old link is down, so
// the guest reconnects and initialises the new device from scratch, as a Wii does when a remote is swapped.
// Pure and lock-free so it is unit-tested with the real Controller (runtime/tests/bluetooth_controller_tests.cpp).

#include "bluetooth/bt_controller.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Bluetooth {

enum class SlotBackend : uint8_t { None, Virtual, Real };

class SlotOwnership {
public:
    static constexpr size_t kTracked = 2;

    // False while the current link belongs to a different backend than the one now behind the slot.
    bool Matches(size_t slot, SlotBackend now) const {
        if (slot >= kTracked) return true;
        const auto owner = static_cast<SlotBackend>(m_owner[slot].load(std::memory_order_relaxed));
        return owner == SlotBackend::None || owner == now;
    }

    // Records the owner when a link starts coming up; returns the owner that is losing the link on Down
    // (None for any other state). The first non-Down state after Down claims the slot.
    SlotBackend OnStateChanged(size_t slot, LinkState state, SlotBackend now) {
        if (slot >= kTracked) return SlotBackend::None;
        if (state == LinkState::Down) {
            return static_cast<SlotBackend>(
                m_owner[slot].exchange(static_cast<uint8_t>(SlotBackend::None), std::memory_order_relaxed));
        }
        uint8_t expected = static_cast<uint8_t>(SlotBackend::None);
        m_owner[slot].compare_exchange_strong(expected, static_cast<uint8_t>(now), std::memory_order_relaxed);
        return SlotBackend::None;
    }

    SlotBackend Owner(size_t slot) const {
        return slot < kTracked ? static_cast<SlotBackend>(m_owner[slot].load(std::memory_order_relaxed))
                               : SlotBackend::None;
    }

private:
    std::array<std::atomic<uint8_t>, kTracked> m_owner{};
};

} // namespace Bluetooth
