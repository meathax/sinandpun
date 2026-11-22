#include "reflex.hpp"

#if defined(_WIN32) && __has_include("../../../third_party/nvidia/nvapi/nvapi.h")
#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
// The NVIDIA header uses MSVC SAL1 annotations that MinGW omits.
#ifdef __MINGW32__
#ifndef __success
#define __success(...)
#endif
#ifndef __in
#define __in
#endif
#ifndef __out
#define __out
#endif
#ifndef __in_opt
#define __in_opt
#endif
#ifndef __out_opt
#define __out_opt
#endif
#ifndef __inout
#define __inout
#endif
#ifndef __inout_opt
#define __inout_opt
#endif
#ifndef __inout_ecount_part_opt
#define __inout_ecount_part_opt(...)
#endif
#ifndef __in_ecount
#define __in_ecount(...)
#endif
#ifndef __in_bcount
#define __in_bcount(...)
#endif
#ifndef __inout_ecount_full
#define __inout_ecount_full(...)
#endif
#ifndef __inout_ecount
#define __inout_ecount(...)
#endif
#ifndef __out_ecount_full_opt
#define __out_ecount_full_opt(...)
#endif
#ifndef __out_ecount_opt
#define __out_ecount_opt(...)
#endif
#endif
#include "../../../third_party/nvidia/nvapi/nvapi.h"

namespace aurora::reflex {
namespace {
std::shared_mutex lifetime;
HMODULE driver{};
IUnknown* device{};
decltype(&NvAPI_D3D_SetSleepMode) setMode{};
decltype(&NvAPI_D3D_Sleep) sleepFrame{};
decltype(&NvAPI_D3D_SetLatencyMarker) setMarker{};
decltype(&NvAPI_Unload) unload{};
decltype(&NvAPI_D3D_GetSleepStatus) getStatus{};
std::atomic_bool requested{true}, enabled{false};
std::atomic_uint64_t sequence{}, producer{};
using Query = void* (__cdecl*)(unsigned);

void updateMode() {
    NV_SET_SLEEP_MODE_PARAMS options{};
    options.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    options.bLowLatencyMode = requested.load();
    const auto result = device ? setMode(device, &options) : NVAPI_ERROR;
    enabled.store(result == NVAPI_OK && options.bLowLatencyMode);
    if (device) std::fprintf(stderr, "[reflex] requested=%d active=%d driver=%d\n",
                             int(options.bLowLatencyMode), int(enabled.load()), int(result));
}
}
void attach(void* nativeDevice) {
    std::unique_lock lock(lifetime);
    if (driver || !nativeDevice) return;
    driver = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!driver) return;
    const auto query = reinterpret_cast<Query>(GetProcAddress(driver, "nvapi_QueryInterface"));
    if (!query) {
        FreeLibrary(driver);
        driver = nullptr;
        return;
    }
    // Public NVIDIA nvapi_interface.h IDs. The driver DLL stays on the user's system.
    const auto init = reinterpret_cast<decltype(&NvAPI_Initialize)>(query(0x0150e828));
    setMode = reinterpret_cast<decltype(setMode)>(query(0xac1ca9e0));
    sleepFrame = reinterpret_cast<decltype(sleepFrame)>(query(0x852cd1d2));
    setMarker = reinterpret_cast<decltype(setMarker)>(query(0xd9984c05));
    getStatus = reinterpret_cast<decltype(getStatus)>(query(0xaef96ca1));
    const auto quit = reinterpret_cast<decltype(unload)>(query(0xd22bdd7e));
    if (!init || !setMode || !sleepFrame || !setMarker || !quit || init() != NVAPI_OK) {
        FreeLibrary(driver);
        driver = nullptr;
        return;
    }
    unload = quit;
    device = static_cast<IUnknown*>(nativeDevice);
    device->AddRef();
    updateMode();
}
void detach() {
    std::unique_lock lock(lifetime);
    enabled.store(false);
    producer.store(0);
    if (device) {
        NV_SET_SLEEP_MODE_PARAMS off{};
        off.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        setMode(device, &off);
        device->Release();
        device = nullptr;
    }
    if (unload) { unload(); unload = nullptr; }
    if (driver) { FreeLibrary(driver); driver = nullptr; }
}
void request(bool on) {
    std::unique_lock lock(lifetime);
    if (requested.exchange(on) != on) updateMode();
}
bool active() { return enabled.load(); }
void mark(uint64_t frame, Marker type) {
    if (!frame || !active()) return;
    std::shared_lock lock(lifetime);
    if (!device || !active()) return;
    NV_LATENCY_MARKER_PARAMS m{};
    m.version = NV_LATENCY_MARKER_PARAMS_VER;
    m.frameID = frame;
    m.markerType = static_cast<NV_LATENCY_MARKER_TYPE>(type);
    if (const auto error = setMarker(device, &m); error != NVAPI_OK && enabled.exchange(false))
        std::fprintf(stderr, "[reflex] marker failed: %d\n", int(error));
}
void begin() {
    if (!active()) return;
    uint64_t frame;
    {
        std::shared_lock lock(lifetime);
        if (!device || !active()) return;
        if (const auto error = sleepFrame(device); error != NVAPI_OK) {
            enabled.store(false);
            std::fprintf(stderr, "[reflex] sleep failed: %d\n", int(error));
            return;
        }
        frame = ++sequence;
        producer.store(frame);
        if (frame == 120 && getStatus) {
            NV_GET_SLEEP_STATUS_PARAMS state{};
            state.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
            if (getStatus(device, &state) == NVAPI_OK)
                std::fprintf(stderr, "[reflex] driver readback lowLatency=%d gameSleep=%d\n",
                             int(state.bLowLatencyMode), int(state.bUseGameSleep));
        }
    }
    mark(frame, Marker::SimulationStart);
    if (frame == 1) std::fprintf(stderr, "[reflex] first driver sleep completed\n");
}
uint64_t end() {
    const auto frame = producer.exchange(0);
    mark(frame, Marker::SimulationEnd);
    return frame;
}
}
#else
namespace aurora::reflex {
void attach(void*) {} void detach() {} void request(bool) {} bool active() { return false; }
void begin() {} uint64_t end() { return 0; } void mark(uint64_t, Marker) {}
}
#endif
