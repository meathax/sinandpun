// /dev/net/kd/request and /dev/net/kd/time (NWC24) for the offline runtime.

#include "hle/net/network.h"

#include "hle/runtime_parse_helpers.h"
#include "memory.h"

#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

namespace {

enum class DeviceKind {
    KdRequest,
    KdTime,
};

using IoVector = RuntimeHle::IoVector;

std::mutex g_mutex;
std::map<int32_t, DeviceKind> g_devices;
int32_t g_nextDeviceFd = 2000;

bool WriteReturn(uint32_t outBuf, uint32_t outLen, int32_t value) {
    if (!outBuf || outLen < 4) {
        return false;
    }
    Memory::Write32(outBuf, static_cast<uint32_t>(value));
    return true;
}

bool GetDeviceKind(uint32_t fd, DeviceKind& kind) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_devices.find(static_cast<int32_t>(fd));
    if (it == g_devices.end()) {
        return false;
    }
    kind = it->second;
    return true;
}

// /dev/net/kd/time state. Any negative result here turns into NWC24's generic
// -42 ("Failed to synchronize time").
int64_t g_kdTimeUtcDiff = 0;
uint64_t g_kdTimeRtc = 0;

uint64_t KdAdjustedUtcSeconds() {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    return static_cast<uint64_t>(now + g_kdTimeUtcDiff);
}

int32_t HandleKdTimeIoctl(uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf,
                          uint32_t outLen) {
    // Every reply writes the WC24 result word at out+0; the u64 payloads live at out+4.
    enum : uint32_t {
        IOCTL_NW24_GET_UNIVERSAL_TIME = 0x14,
        IOCTL_NW24_SET_UNIVERSAL_TIME = 0x15,
        IOCTL_NW24_UNIMPLEMENTED = 0x16,
        IOCTL_NW24_SET_RTC_COUNTER = 0x17,
        IOCTL_NW24_GET_TIME_DIFF = 0x18,
    };

    int32_t ipcResult = 0;
    switch (cmd) {
    case IOCTL_NW24_GET_UNIVERSAL_TIME:
        if (outBuf && outLen >= 0x0C) {
            Memory::Write64(outBuf + 4u, KdAdjustedUtcSeconds());
        }
        break;
    case IOCTL_NW24_SET_UNIVERSAL_TIME:
        if (inBuf && inLen >= 8) {
            const uint64_t requestedUtc = Memory::Read64(inBuf);
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            g_kdTimeUtcDiff = static_cast<int64_t>(requestedUtc) - now;
        }
        break;
    case IOCTL_NW24_SET_RTC_COUNTER:
        if (inBuf && inLen >= 4) {
            g_kdTimeRtc = Memory::Read32(inBuf);
        }
        break;
    case IOCTL_NW24_GET_TIME_DIFF:
        if (outBuf && outLen >= 0x0C) {
            Memory::Write64(outBuf + 4u, KdAdjustedUtcSeconds() - g_kdTimeRtc);
        }
        break;
    case IOCTL_NW24_UNIMPLEMENTED:
        ipcResult = -9;
        break;
    default:
        break;
    }

    WriteReturn(outBuf, outLen, 0);
    return ipcResult;
}

// NWC24 request device: scheduler suspend/resume, shutdown and the like. Offline,
// every request succeeds with an empty reply; the time-trigger spans are the only
// payload the SDK reads back.
int32_t HandleKdIoctl(uint32_t cmd, uint32_t outBuf, uint32_t outLen) {
    WriteReturn(outBuf, outLen, 0);
    if (cmd == 0x04 && outBuf && outLen >= 0x0C) { // time triggers: mail / download span (minutes)
        Memory::Write32(outBuf + 4u, 1);
        Memory::Write32(outBuf + 8u, 2);
    }
    return 0;
}

} // namespace

extern "C" int32_t Network_HLE_OpenDevice(const char* path, uint32_t mode) {
    (void)mode;
    if (!path) {
        return -101;
    }
    DeviceKind kind;
    if (std::string_view(path) == "/dev/net/kd/request") {
        kind = DeviceKind::KdRequest;
    } else if (std::string_view(path) == "/dev/net/kd/time") {
        kind = DeviceKind::KdTime;
    } else {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const int32_t fd = g_nextDeviceFd++;
    g_devices.emplace(fd, kind);
    return fd;
}

extern "C" bool Network_HLE_IsFd(uint32_t fd) {
    DeviceKind kind;
    return GetDeviceKind(fd, kind);
}

extern "C" int32_t Network_HLE_Close(uint32_t fd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_devices.erase(static_cast<int32_t>(fd)) != 0 ? 0 : -101;
}

extern "C" int32_t Network_HLE_Ioctl(uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen,
                                      uint32_t outBuf, uint32_t outLen) {
    DeviceKind kind;
    if (!GetDeviceKind(fd, kind)) {
        return -101;
    }
    return kind == DeviceKind::KdTime ? HandleKdTimeIoctl(cmd, inBuf, inLen, outBuf, outLen)
                                      : HandleKdIoctl(cmd, outBuf, outLen);
}

extern "C" int32_t Network_HLE_Ioctlv(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut,
                                       uint32_t vectorPtr) {
    DeviceKind kind;
    if (!GetDeviceKind(fd, kind)) {
        return -101;
    }
    // numIn/numOut are guest-controlled; sum in 64-bit so a wrap cannot make the
    // vector walk run off the end.
    constexpr uint64_t kMaxIoVectors = 32;
    const uint64_t vectorCount = static_cast<uint64_t>(numIn) + numOut;
    if (vectorCount > kMaxIoVectors || (vectorCount != 0 &&
        (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(vectorCount) * 8u)))) {
        return -28; // SO_EINVAL
    }
    std::vector<IoVector> in;
    std::vector<IoVector> out;
    for (uint32_t i = 0; i < numIn; ++i) {
        in.push_back(RuntimeHle::ReadIoVector(vectorPtr, i));
    }
    for (uint32_t i = 0; i < numOut; ++i) {
        out.push_back(RuntimeHle::ReadIoVector(vectorPtr, numIn + i));
    }
    const uint32_t inBuf = in.empty() ? 0 : in[0].address;
    const uint32_t inLen = in.empty() ? 0 : in[0].size;
    const uint32_t outBuf = out.empty() ? 0 : out[0].address;
    const uint32_t outLen = out.empty() ? 0 : out[0].size;
    return kind == DeviceKind::KdTime ? HandleKdTimeIoctl(cmd, inBuf, inLen, outBuf, outLen)
                                      : HandleKdIoctl(cmd, outBuf, outLen);
}
