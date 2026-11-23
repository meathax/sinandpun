// Stage modules (stg00..stg07.rso).
//
// The game loads a stage module into a heap buffer and links it with RSOLinkListFixed; on hardware the
// module's code then runs from that buffer. Here the code is statically translated, so every module is
// pre-linked at one reserved MEM2 window (tools/rso_link.py, base 0x97DC0000) and translated per module
// (tools/module_shards.py). This file emulates the link/unlink step:
//   RSOLinkListFixed : copy the pre-linked image into the window, activate that module's code set, and
//                      fill in the RSO header's absolute prolog/epilog/unresolved pointers.
//   RSOUnLinkList    : deactivate the code set (0x8020EA20, verified: removes the RSO from the list, RSOUnLink
//                      against the rest, then converts the header back to file offsets; the game calls the
//                      header epilog pointer (+0x28) BEFORE this and frees the heap buffer AFTER it).
// Full protocol and evidence: docs/MODULE-LOADER-NOTES.md (loader fn_8001F478 / fn_8001F25C).
//   ModuleOverlay    : resolves calls into the window to the active code set. It is consulted only where
//                      the base registry has no translation, so the base hot path is unaffected.

#include "module_file.h"
#include "abi_bridge.h"
#include "guest_abi/module_codeset.h"
#include "hle_stubs.h"
#include "memory.h"
#include "perf_pacing.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kWindowBase = 0x97DC0000u;
constexpr uint32_t kWindowSize = 0x00200000u;

// RSOObjectHeader (revolution/rso.h): info(link next/prev, numSections, sectionInfoOffset, nameOffset,
// nameSize, version), bssSize, prolog/epilog/unresolved section bytes, then the three function fields.
constexpr uint32_t kHdrNameOffset = 0x10;
constexpr uint32_t kHdrNameSize = 0x14;
constexpr uint32_t kHdrProlog = 0x24;
constexpr uint32_t kHdrEpilog = 0x28;
constexpr uint32_t kHdrUnresolved = 0x2C;

std::atomic<const ModuleCodeSet*> g_active{nullptr};
std::atomic<int> g_activeIndex{-1};

int ModuleIndexFromName(const std::string& name) {
    const size_t at = name.rfind("stg");
    if (at == std::string::npos || at + 5 > name.size()) {
        return -1;
    }
    const char a = name[at + 3];
    const char b = name[at + 4];
    if (a < '0' || a > '9' || b < '0' || b > '9') {
        return -1;
    }
    return (a - '0') * 10 + (b - '0');
}

std::filesystem::path FindImage(int index) {
    char file[32];
    std::snprintf(file, sizeof file, "stg%02d.bin", index);
    // Images generated from the user's disc at import time (disc_import.cpp) come first.
    std::vector<std::filesystem::path> roots = {RuntimeConfigFile::ApplicationDataDirectory() / "Game" / "modules", "modules",
                                                "../modules", "../../build/modules", "build/modules"};
    if (const char* env = std::getenv("SIN_MODULE_DIR")) {
        roots.insert(roots.begin(), env);
    }
    for (const auto& root : roots) {
        std::error_code ec;
        const auto candidate = root / file;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

} // namespace

namespace ModuleOverlay {

bool TryInvoke(uint32_t target, CpuContext* cpu) {
    const ModuleCodeSet* set = g_active.load(std::memory_order_acquire);
    if (set == nullptr || target < set->windowBase || target >= set->windowBase + set->windowSize) {
        return false;
    }
    size_t lo = 0;
    size_t hi = set->count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const uint32_t at = set->records[mid].address;
        if (at == target) {
            const auto& record = set->records[mid];
            RawDispatchRecord raw;
            raw.address = record.address;
            raw.entry = record.entry;
            raw.nonvolatileFprWriteMask =
                record.preservesNonvolatileFprs ? 0u : (record.nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask);
            raw.preserveNonvolatileGprs = false;
            return TryDispatchRawCpuTarget(&raw, cpu);
        }
        if (at < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

} // namespace ModuleOverlay

// RSOLinkListFixed (0x8020EA08): BOOL (RSOObjectHeader* newRso, void* bss, RSOFixedLevel level)
extern "C" uint32_t RSOLinkListFixed_HLE(uint32_t rso, uint32_t bss, uint32_t level) {
    // The module's own bss is inside the pre-linked window image (zeroed on every link below); the game's
    // heap bss buffer (bss arg, cleared by the game itself right after this call) is never used by module code.
    (void)bss;
    // 0x8020EA08: cmpwi r5,2 ; ble -> LinkList ; else li r3,0 ; blr   (the game always passes level 1)
    if (static_cast<int32_t>(level) > 2) {
        return 0;
    }
    try {
        const uint32_t nameOffset = Memory::Read32(rso + kHdrNameOffset);
        const uint32_t nameSize = Memory::Read32(rso + kHdrNameSize);
        std::string name(nameSize < 256 ? nameSize : 255, '\0');
        for (size_t i = 0; i < name.size(); ++i) {
            name[i] = static_cast<char>(Memory::Read8(rso + nameOffset + static_cast<uint32_t>(i)));
        }
        const int index = ModuleIndexFromName(name);
        // Match the code set by name, not by table position, so a partial or reordered module table cannot
        // silently activate the wrong stage.
        const ModuleCodeSet* found = nullptr;
        if (index >= 0) {
            char want[16];
            std::snprintf(want, sizeof want, "stg%02d", index);
            for (unsigned long long i = 0; i < g_moduleCodeSetCount; ++i) {
                if (g_moduleCodeSets[i] != nullptr && std::strcmp(g_moduleCodeSets[i]->name, want) == 0) {
                    found = g_moduleCodeSets[i];
                    break;
                }
            }
        }
        if (found == nullptr) {
            RT_LOG(RT_TAG_OS) << "RSOLinkListFixed: unknown module '" << name << "'" << std::endl;
            return 0;
        }
        const ModuleCodeSet& set = *found;
        const auto path = FindImage(index);
        if (path.empty()) {
            RT_LOG(RT_TAG_OS) << "RSOLinkListFixed: pre-linked image stg" << index
                              << ".bin not found (set SIN_MODULE_DIR or run tools/rso_link.py)" << std::endl;
            return 0;
        }
        const auto rsoStart = std::chrono::steady_clock::now();
        PerfPacing::PhaseScope rsoScope(PerfPacing::kPhaseRso);
        // Images are immutable after import: read each file once (SIN_RSO_CACHE=0 re-reads on every link).
        static std::vector<uint8_t> s_images[100];
        static const bool s_cache = [] {
            const char* v = std::getenv("SIN_RSO_CACHE");
            return !(v && *v == '0');
        }();
        std::vector<uint8_t> scratch;
        std::vector<uint8_t>& image = (s_cache && index >= 0 && index < 100) ? s_images[index] : scratch;
        if (image.empty()) {
            image = ReadModuleFile(path, set.windowSize);
        }
        g_active.store(nullptr, std::memory_order_release);
        uint8_t* window = Memory::GetPointer(set.windowBase, set.windowSize);
        std::memcpy(window, image.data(), image.size());
        std::memset(window + image.size(), 0, set.windowSize - image.size());
        Memory::Write32(rso + kHdrProlog, set.prolog);
        Memory::Write32(rso + kHdrEpilog, set.epilog);
        Memory::Write32(rso + kHdrUnresolved, set.unresolved);
        g_activeIndex.store(index, std::memory_order_relaxed);
        g_active.store(&set, std::memory_order_release);
        PerfPacing::NoteStageLink(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rsoStart).count());
        RT_LOG(RT_TAG_OS) << "RSOLinkListFixed: " << set.name << " (" << name << ") linked: image "
                          << image.size() << " bytes at 0x" << std::hex << set.windowBase << ", prolog 0x"
                          << set.prolog << std::dec << ", " << set.count << " functions" << std::endl;
        return 1;
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "RSOLinkListFixed", e);
        return 0;
    } catch (const std::exception& e) {
        RT_LOG(RT_TAG_OS) << "RSOLinkListFixed: " << e.what() << std::endl;
        return 0;
    }
}
PPC_NATIVE_OVERRIDE(8020EA08, RSOLinkListFixed_HLE, uint32_t, (uint32_t rso, uint32_t bss, uint32_t level),
                    (rso, bss, level));

// RSOUnLinkList (0x8020EA20): BOOL (RSOObjectHeader* oldRso)
extern "C" uint32_t RSOUnLinkList_HLE(uint32_t rso) {
    (void)rso;
    const ModuleCodeSet* set = g_active.exchange(nullptr, std::memory_order_acq_rel);
    if (set != nullptr) {
        RT_LOG(RT_TAG_OS) << "RSOUnLinkList: " << set->name << " unlinked" << std::endl;
    }
    g_activeIndex.store(-1, std::memory_order_relaxed);
    // Real function returns 0 when the RSO is not in the list (next == prev == 0), else 1.
    return set != nullptr ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(8020EA20, RSOUnLinkList_HLE, uint32_t, (uint32_t rso), (rso));
