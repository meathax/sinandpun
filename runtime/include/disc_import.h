#pragma once

// First-run disc import: reads a Wii disc image (.iso, .wbfs, .rvz, .wia, .chd, .ciso, .gcz, .bin + .cue), checks that it is
// Sin & Punishment: Star Successor (R2VE01), decrypts the Wii data partition and extracts the game files into
// <gameDir>/{sys,files}. Everything the runtime derives from the disc (the prelinked stage module images,
// formerly tools/rso_link.py) is generated into <gameDir>/modules/stgNN.bin at the same time.
//
// This unit is self-contained (no runtime headers, Windows CNG for AES/SHA-1) so it can be tested standalone.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace DiscImport {

inline constexpr const char* kGameId = "R2VE01";

struct GameVersionInfo {
    const char* gameId;
    const char* displayName;
    const char* moduleExportFile;
};

inline constexpr GameVersionInfo kGameVersions[] = {
    {"R2VE01", "Sin & Punishment: Star Successor (USA)", "gs2.sel"},
};

inline constexpr const GameVersionInfo* FindGameVersion(std::string_view gameId) {
    for (const GameVersionInfo& version : kGameVersions) {
        if (gameId == version.gameId) return &version;
    }
    return nullptr;
}

// Shown for a disc or folder that is not the supported USA release. The Japanese release (R2VJ01) is not
// supported: the USA release already contains the Japanese voice track.
inline std::string UnsupportedGameMessage(const std::string& gameId) {
    if (gameId == "R2VJ01")
        return "This is the Japanese version (R2VJ01), which is not supported. Use the North American (R2VE01) "
               "disc instead: it includes the Japanese voices (Settings > Audio > Voice > Language).";
    if (gameId.size() == 6 && gameId.compare(0, 3, "R2V") == 0) {
        const char* name = gameId[3] == 'P' ? "European (PAL)" : gameId[3] == 'K' ? "Korean" : "another region's";
        return std::string("This is the ") + name + " version of Sin & Punishment: Star Successor (" + gameId +
               "). Only the North American (R2VE01) version is supported.";
    }
    return "This is \"" + gameId + "\", not Sin & Punishment: Star Successor for North America (R2VE01).";
}

inline constexpr uint32_t kModuleBase = 0x97DC0000u;  // reserved MEM2 window the stage modules are linked at
inline constexpr uint32_t kModuleWindowSize = 0x200000u;
inline constexpr int kModuleCount = 8;

struct Progress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{1};
    std::atomic<bool> cancel{false};

    void SetStage(std::string stage, std::string detail = {});
    void Get(std::string& stage, std::string& detail) const;

private:
    mutable std::mutex m_lock;
    std::string m_stage, m_detail;
};

struct Summary {
    std::string gameId;
    uint32_t files = 0;
    uint64_t bytes = 0;
    uint32_t modules = 0;
    double seconds = 0.0;
    std::string keySource;  // file the Wii key came from, or why none was needed (never the key itself)
};

struct DiscInfo {
    std::string gameId, title, format;
    bool wii = false;
    bool supported = false;  // the North American R2VE01 disc
};

enum class Result { Ok, Cancelled, Failed, KeyRequired };

// Imports `image` into `gameDir` (built in a sibling "<gameDir>.importing" directory and swapped in only when
// everything succeeded). On Failed, `error` holds a user-presentable explanation.
Result ImportDisc(const std::filesystem::path& image, const std::filesystem::path& gameDir, Progress& progress,
                  Summary& summary, std::string& error, const std::filesystem::path& keyFile);

// True when `gameDir` holds a complete extracted game (sys/{boot.bin,fst.bin,main.dol} and files/).
bool IsGameDirectoryComplete(const std::filesystem::path& gameDir);

// Port of tools/rso_link.py: links stgNN.rso against gs2.sel at `base` into a flat image. Returns false and sets
// `error` for malformed input; `unresolved` (optional) receives the count of imports the SEL could not resolve.
bool BuildModuleImage(const std::vector<uint8_t>& rso, const std::vector<uint8_t>& sel, uint32_t base,
                      std::vector<uint8_t>& image, std::string& error, int* unresolved = nullptr);
bool BuildModuleImage(const std::vector<uint8_t>& rso, const std::vector<uint8_t>& sel,
                      const std::vector<uint8_t>& mainDol, uint32_t base,
                      std::vector<uint8_t>& image, std::string& error, int* unresolved = nullptr);

// Locations a disc picker may return: .cue is resolved to its .bin by the importer.
bool IsSupportedDiscExtension(const std::filesystem::path& path);

// Cheap header check for the first-run picker (no decryption, no key needed).
bool ProbeDisc(const std::filesystem::path& image, DiscInfo& info, std::string& error);

}  // namespace DiscImport
