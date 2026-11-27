#include "hle_stubs.h"

#include "console_identity.h"
#include "sc_serial_contract.h"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "memory.h"
#include "guest_small_data.h"
#include "guest_abi/guest_region.h"
#include "runtime_config.h"
#include "aurora_events.h"
#include "runtime_log.h"

namespace {

// Use the SDK's own value tables, including its unknown-region result.
uint32_t LookupProductRegion(uint32_t table, uint32_t stride, uint32_t count,
                             const std::string& value) {
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t entry = table + index * stride;
        if (!Memory::Contains(entry, stride)) {
            break;
        }
        const auto* bytes = static_cast<const uint8_t*>(Memory::GetPointer(entry, stride));
        if (bytes[0] == 0xFF) {
            break;
        }
        if (value.size() < stride - 1 &&
            std::memcmp(bytes + 1, value.c_str(), value.size() + 1) == 0) {
            return bytes[0];
        }
    }
    return 0xFFFFFFFFu;
}

} // namespace

// SCCheckStatus is polled in OSInit's busy loop (while(SCCheckStatus()==1) waits on async SYSCONF
// load via NAND IPC); we have no async IPC callbacks, so return 0 (SUCCESS) immediately.

// 0x801B0220 -> SCCheckStatus()
// Returns: 0 = success, 1 = busy, 2 = error
extern "C" uint32_t SCCheckStatus_HLE()
{
    // Return 0 (success) immediately to avoid infinite busy-wait in OSInit
    return 0;
}

PPC_NATIVE_OVERRIDE(801B5010, SCCheckStatus_HLE, uint32_t, (), ());

// 0x801B1BE4 -> SCGetAspectRatio()
// Returns: 0 = 4:3, 1 = 16:9
extern "C" uint32_t SCGetAspectRatio_HLE()
{
    return RuntimeConfigFile::WidescreenEnabled(true) ? 1u : 0u;
}

PPC_NATIVE_OVERRIDE(801B6900, SCGetAspectRatio_HLE, uint32_t, (), ());

// 0x801B1CAC -> SCGetEuRgb60Mode()
// Returns: 0 = PAL50, 1 = PAL60/RGB60
extern "C" uint32_t SCGetEuRgb60Mode_HLE()
{
    // Default to PAL60 so PAL builds do not fall back to the half-rate PAL50
    // sync path on modern displays. Actual texture/cache correctness is handled
    // elsewhere; this only exposes the intended SYSCONF setting.
    return 1;
}

PPC_NATIVE_OVERRIDE(801B69E0, SCGetEuRgb60Mode_HLE, uint32_t, (), ());

// Expose the selected emulated NAND identity through the SDK SC APIs.

extern "C" uint32_t SCGetProductArea_HLE()
{
    // R2VE01 SCGetProductArea (0x801B7060) walks the SDK area table at 0x8031F828 (5-byte rows:
    // code + "JPN"/"USA"/...; 0xFF terminates). The donor table address (0x8029CEB0) is Sin code.
    return LookupProductRegion(MKW_GADDR(8031F828), 5, 16,
                               RuntimeConsoleIdentity::Current().area);
}

PPC_NATIVE_OVERRIDE(801B7060, SCGetProductArea_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductCode_HLE()
{
    // Original SC storage for the six-byte CODE value: the SDK's SDA buffer at r13-21408
    // (SCGetProductCode passes it to __SCF1 as the destination). The donor address 0x803869E0
    // is unrelated R2VE01 .bss.
    const uint32_t kProductCodeAddress = GuestSmallData::kSda1Base - 21408u;
    const std::string& productCode = RuntimeConsoleIdentity::Current().productCode;
    const size_t size = productCode.size() + 1;
    if (!Memory::Contains(kProductCodeAddress, size)) {
        return 0;
    }
    std::memcpy(Memory::GetPointer(kProductCodeAddress, size),
                productCode.c_str(), size);
    return kProductCodeAddress;
}

PPC_NATIVE_OVERRIDE(801B70F0, SCGetProductCode_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductSN_HLE(uint32_t serialAddress)
{
    const std::string& serial = RuntimeConsoleIdentity::Current().serial;
    return RuntimeScSerial::Write(serial, serialAddress,
        [](uint32_t address, size_t size) { return Memory::Contains(address, size); },
        [](uint32_t address, uint32_t value) { Memory::Write32(address, value); });
}

PPC_NATIVE_OVERRIDE(801B7130, SCGetProductSN_HLE, uint32_t, (uint32_t serialAddress), (serialAddress));

extern "C" uint32_t SCGetProductGameRegion_HLE()
{
    // R2VE01 game-region table at 0x8031F870 (4-byte rows: code + "JP"/"US"/"EU"/...).
    return LookupProductRegion(MKW_GADDR(8031F870), 4, 8,
                               RuntimeConsoleIdentity::Current().gameRegion);
}

PPC_NATIVE_OVERRIDE(801B71A0, SCGetProductGameRegion_HLE, uint32_t, (), ());
