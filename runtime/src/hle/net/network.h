#pragma once

#include <cstdint>

// Offline-only IOS network surface. Sin & Punishment never opens /dev/net/ip/top,
// /dev/net/ssl or /dev/net/ncd/manage on its boot or gameplay path; only the
// NWC24 time and request devices are touched (OSInit -> __OSInitNet registers
// the NWC24 shutdown function and synchronises the RTC counter). Everything else
// is left unsupported so IOS_Open on it fails like on a console without a network.
extern "C" int32_t Network_HLE_OpenDevice(const char* path, uint32_t mode);
extern "C" bool Network_HLE_IsFd(uint32_t fd);
extern "C" int32_t Network_HLE_Close(uint32_t fd);
extern "C" int32_t Network_HLE_Ioctl(uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen,
                                      uint32_t outBuf, uint32_t outLen);
extern "C" int32_t Network_HLE_Ioctlv(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut,
                                       uint32_t vectorPtr);
