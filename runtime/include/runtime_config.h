#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <toml.hpp>
#include "platform/host_platform.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

struct RuntimeUserConfig {
    std::optional<bool> widescreen;
    std::optional<bool> forceAspect;
    bool legacyForceAspect = false;
    std::optional<int32_t> windowPosX;
    std::optional<int32_t> windowPosY;
    std::optional<uint32_t> windowWidth;
    std::optional<uint32_t> windowHeight;
    std::optional<float> resolutionMultiplier;
    std::optional<std::string> graphicsApi;
    std::optional<std::string> displayMode;
    std::optional<uint32_t> frameInterpolationFps;
    // video.frame_rate: "original", "display", "uncapped" or a number of fps (see ParseFrameRate).
    std::optional<std::string> frameRate;
    std::optional<uint32_t> frameRateCap;
    // video.vrr: "auto", "on" or "off"; video.vrr_margin_fps: latency limiter margin below the refresh rate;
    // video.vrr_limiter: false lets the frame rate run unlimited inside the VRR present mode.
    std::optional<std::string> vrr;
    std::optional<uint32_t> vrrMarginFps;
    std::optional<bool> vrrLimiter;
    std::optional<bool> metalFxSpatialUpscaling;
    // video.upscaler: "off", "fsr1", "fsr3" or "dlss"; video.fsr_quality: see kFsrQualities;
    // video.fsr_sharpness: 0-100 (0 = no RCAS sharpening).
    std::optional<std::string> upscaler;
    std::optional<std::string> fsrQuality;
    std::optional<uint32_t> fsrSharpness;
    // video.dlss_mode (used when video.upscaler = "dlss"): ultra_performance, performance, balanced, quality or dlaa;
    // video.dlss_sharpness: 0-100 (0 = no sharpening after DLSS); video.dlss_motion_vectors: enable image-based motion estimation
    // for presentations without a GX velocity snapshot.
    std::optional<std::string> dlssMode;
    std::optional<uint32_t> dlssSharpness;
    std::optional<bool> dlssMotionVectors;
    std::optional<bool> frameGeneration;
    std::optional<bool> reflex;
    std::optional<bool> skipUnreadyPipelines;
    std::optional<bool> vsync;
    std::optional<bool> gxWorker;
    std::optional<bool> disableCopyFilter;
    std::optional<bool> enhancementsEnabled;
    std::optional<bool> sceneGlow;
    std::optional<bool> enhancedBloom;
    std::optional<bool> sharpeningEnabled;
    std::optional<bool> ambientOcclusion;
    std::optional<bool> globalIllumination;
    std::optional<bool> rayTracedGi;
    std::optional<bool> retainCulledGi;
    std::optional<bool> strongBounce;
    std::optional<bool> projectedCaustics;
    std::optional<bool> surfaceHighlights;
    std::optional<bool> probeMaterials;
    std::optional<bool> combatLight;
    std::optional<bool> contactShadow;
    std::optional<bool> stageAtmosphere;
    std::optional<bool> pixelLighting;
    std::optional<bool> hdHud;
    std::optional<bool> hdWorld;
    std::optional<bool> generateMips;
    std::optional<bool> textureReplacements;
    std::optional<bool> textureDumps;
    std::optional<bool> showFps;
    std::optional<uint32_t> disabledPostProcessingPaths;
    std::optional<float> audioVolume;
    std::optional<float> audioMusicVolume;
    std::optional<float> audioSoundEffectsVolume;
    std::optional<float> audioUiVolume;
    std::optional<float> audioVoicesVolume;
    std::optional<bool> audioMuted;
    std::optional<bool> audioMixWorker;
    std::optional<bool> attenuateMusicWhenMediaPlays;
    // Real Wii Remotes passed through to the game's own Bluetooth stack. Off hands
    // Nintendo controllers (e.g. the Wii U Pro Controller) to SDL as plain gamepads.
    std::optional<bool> wiiRemotes;
    // Look for remotes in pairing mode (1+2 / SYNC) while none is connected.
    std::optional<bool> wiiAutoPair;
    // The console's Wii Remote settings (SYSCONF BT.BAR, BT.SENS, BT.SPKV, BT.MOT).
    std::optional<bool> wiiSensorBarAbove;
    std::optional<int32_t> wiiIrSensitivity;
    std::optional<int32_t> wiiSpeakerVolume;
    std::optional<bool> wiiRumble;
    // Outside a Steam launch, ask a running Steam client for a game controller layout while
    // the window has focus instead of its desktop one (buttons bound to mouse and keyboard).
    std::optional<bool> steamInputOverride;
    std::optional<uint32_t> steamInputAppId;
    std::optional<std::string> nandRoot;
    std::optional<std::string> dvdRoot;
    // Controller mappings use Wii/GameCube button names as keys and up to two
    // comma-separated SDL-style physical button names ("south", or
    // "dpad_up,left_shoulder") as values; pressing either bound button counts.
    std::array<std::optional<std::string>, 12> controllerButtons;
    // Per-port overrides of controllerButtons ("a_1" .. "right_4", the suffix is port + 1).
    // A port falls back to the shared controllerButtons entry while its override is unset, so
    // configs written before per-port bindings keep applying to every port unchanged.
    std::array<std::array<std::optional<std::string>, 12>, 4> controllerPortButtons;
    std::optional<bool> enhancedCoop;
    std::optional<int64_t> secondRemote;
    std::optional<bool> rumbleEnabled;
    // Shake a motion controller to press the D-pad, like shaking a Wii Remote.
    std::optional<bool> motionTrick;
    std::optional<int32_t> motionTrickSensitivity;
    std::optional<int32_t> muteHotkey;
    std::map<std::string, std::string> controllerExpressions;
};

namespace RuntimeConfigFile {

// Narrow path strings are UTF-8 everywhere in the runtime; string() and the
// char path constructor would use the ANSI codepage on Windows, which drops
// characters the codepage cannot represent.
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::optional<std::filesystem::path>& ApplicationDataDirectoryOverride() {
    static std::optional<std::filesystem::path> override;
    return override;
}

inline void SetApplicationDataDirectory(std::filesystem::path path) {
    std::error_code ec;
    if (path.is_relative()) {
        path = std::filesystem::absolute(path, ec);
    }
    ApplicationDataDirectoryOverride() = path.lexically_normal();
}

// Runtime globals load settings before main() runs. Inspect --data directly from the process
// command line so a caller that uses the CLI override gets the same early path as
// RECOMPLAUNCHER_DATA. The in-main parser still owns validation and friendly diagnostics.
inline std::optional<std::filesystem::path> ProcessArgumentDataDirectory() {
#ifdef _WIN32
    static const std::optional<std::filesystem::path> result = []() {
        int argc = 0;
        LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        if (!arguments) {
            return std::optional<std::filesystem::path>{};
        }
        std::optional<std::filesystem::path> found;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(arguments[index]);
            if (argument.starts_with(L"--data=") && argument.size() > 7) {
                found = std::filesystem::path(argument.substr(7));
                break;
            }
            if (argument == L"--data" && index + 1 < argc &&
                !std::wstring_view(arguments[index + 1]).starts_with(L"--")) {
                found = std::filesystem::path(arguments[index + 1]);
                break;
            }
        }
        ::LocalFree(arguments);
        return found;
    }();
    return result;
#else
    return std::nullopt;
#endif
}

inline constexpr const char* kConfigFileName = "Config.toml";
// Game-specific so this port never shares Config.toml, Game/ or NAND/ with another WiiCompiled-based
// port: a foreign Game/ folder booted that game's main.dol against this game's translated code.
inline constexpr const char* kApplicationDirectoryName = "SinAndPunishmentRecomp";
// Folder used up to 1.0.9; migrated by MigrateLegacyApplicationData when it holds this game's data.
inline constexpr const char* kLegacyApplicationDirectoryName = "WiiCompiled";

// Portable layout. A directory holding kPortableMarkerFileName is a portable root; every piece of
// runtime user state (Config.toml, NAND, cache, Logs) lives in <root>/UserData instead of
// %LOCALAPPDATA%. The marker is searched for from the executable's directory upwards, which is what
// makes an installation survive being moved or carried on removable media.
inline constexpr const char* kPortableMarkerFileName = "portable.txt";
inline constexpr const char* kPortableUserDataDirectoryName = "UserData";

// The installed layout puts products two levels below the root (<root>/Install/Base/game.exe). The
// bound is deliberately small so an unrelated marker far up a drive can never capture an ordinary
// installation.
inline constexpr int kPortableSearchDepth = 4;

inline std::string Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

inline bool IsSupportedResolutionMultiplier(float value) {
    static constexpr std::array values{0.0f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Must stay in step with the backend table in main.cpp, which is what actually
// maps these to AuroraBackend.
inline bool IsSupportedGraphicsApi(std::string_view value) {
#if defined(__APPLE__)
    static constexpr std::array<std::string_view, 2> values{"auto", "metal"};
// only vulkan for linux
#elif defined(__linux__)
    static constexpr std::array<std::string_view, 2> values{"auto", "vulkan"};
#elif defined(_WIN32)
    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};
#endif
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline bool IsSupportedDisplayMode(std::string_view value) {
    static constexpr std::array<std::string_view, 3> values{
        "windowed", "borderless", "exclusive",
    };
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Legacy key video.frame_interpolation_fps: 0 (off) or a target above 60. Any such value is
// accepted now (240 included); it is only read when video.frame_rate is absent.
inline constexpr uint32_t kFrameRateHardCap = 1000;
inline bool IsSupportedFrameInterpolationFps(uint32_t value) {
    return value == 0 || (value > 60 && value <= kFrameRateHardCap);
}

// Host presentation rate. The game's logic, physics, audio and time base always run at the original
// 60 Hz; this only decides how many frames are shown per game frame.
enum class FrameRateKind : uint8_t { Original, Display, Fixed, Uncapped };
struct FrameRateSetting {
    FrameRateKind kind = FrameRateKind::Original;
    uint32_t fps = 0; // Fixed only
    bool operator==(const FrameRateSetting& o) const { return kind == o.kind && fps == o.fps; }
};

// Accepts "original"/"off"/"60", "display"/"match"/"refresh"/"auto", "uncapped"/"unlimited"/"max" and
// a number with an optional "fps"/"hz" suffix. Numbers up to 60 mean original pacing; larger ones are
// clamped to the hard cap.
inline bool ParseFrameRate(std::string text, FrameRateSetting& out) {
    std::string t;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"' && c != '_' && c != '-') {
            t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    for (const char* suffix : {"fps", "hz"}) {
        const std::string s = suffix;
        if (t.size() > s.size() && t.compare(t.size() - s.size(), s.size(), s) == 0) {
            t.resize(t.size() - s.size());
        }
    }
    if (t == "original" || t == "off" || t == "native" || t == "default" || t == "false" || t == "none") {
        out = {FrameRateKind::Original, 0};
        return true;
    }
    if (t == "display" || t == "match" || t == "matchdisplay" || t == "refresh" || t == "auto" || t == "vsync") {
        out = {FrameRateKind::Display, 0};
        return true;
    }
    if (t == "uncapped" || t == "unlimited" || t == "max" || t == "unlocked") {
        out = {FrameRateKind::Uncapped, 0};
        return true;
    }
    if (t.empty() || t.size() > 9 || !std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    const uint64_t value = std::stoull(t);
    if (value <= 60) {
        out = {FrameRateKind::Original, 0};
    } else {
        out = {FrameRateKind::Fixed, static_cast<uint32_t>(std::min<uint64_t>(value, kFrameRateHardCap))};
    }
    return true;
}

inline std::string FormatFrameRate(const FrameRateSetting& value) {
    switch (value.kind) {
    case FrameRateKind::Display:
        return "display";
    case FrameRateKind::Uncapped:
        return "uncapped";
    case FrameRateKind::Fixed:
        return std::to_string(value.fps);
    default:
        return "original";
    }
}

// Upscaler selector shared by FSR and DLSS: video.upscaler = off | fsr1 | fsr3 | dlss, plus the FSR quality
// preset (AMD's standard per-axis scale factors) and the RCAS sharpness. Parsing is tolerant: case, spaces,
// '_' '-' '.' are ignored and a few aliases are accepted ("fsr" = fsr1, "ultra quality", "UltraQuality").
struct FsrQualityInfo {
    const char* name;
    float factor;
};
inline constexpr std::array<FsrQualityInfo, 6> kFsrQualities{{
    {"native", 1.0f},
    {"ultra_quality", 1.3f},
    {"quality", 1.5f},
    {"balanced", 1.7f},
    {"performance", 2.0f},
    {"ultra_performance", 3.0f},
}};
inline constexpr uint32_t kDefaultFsrSharpness = 80;

inline std::string CompactToken(std::string_view text) {
    std::string t;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"' && c != '_' && c != '-' && c != '.') {
            t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return t;
}

inline std::optional<std::string> ParseUpscaler(std::string_view text) {
    const std::string t = CompactToken(text);
    if (t == "off" || t == "none" || t == "false" || t == "0" || t == "native") return "off";
    if (t == "fsr1" || t == "fsr" || t == "fsr10" || t == "easu" || t == "fsrspatial" || t == "fsr1spatial") return "fsr1";
    if (t == "fsr3" || t == "fsr31" || t == "fsr3temporal" || t == "fsr2") return "fsr3";
    if (t == "dlss") return "dlss";
    return std::nullopt;
}

inline std::optional<std::string> ParseFsrQuality(std::string_view text) {
    const std::string t = CompactToken(text);
    for (const FsrQualityInfo& q : kFsrQualities) {
        if (t == CompactToken(q.name)) return std::string(q.name);
    }
    if (t == "ultraqual" || t == "uq") return "ultra_quality";
    if (t == "perf") return "performance";
    if (t == "ultraperf" || t == "up") return "ultra_performance";
    return std::nullopt;
}

// NVIDIA DLSS quality modes: per-axis output / render ratio (Ultra Performance 33%, Performance 50%, Balanced 58%,
// Quality 67%, DLAA 100%). The ratios are the ones DLSS itself reports.
struct DlssModeInfo {
    const char* name;
    float factor;
};
inline constexpr std::array<DlssModeInfo, 5> kDlssModes{{
    {"ultra_performance", 3.0f},
    {"performance", 2.0f},
    {"balanced", 1.0f / 0.58f},
    {"quality", 1.5f},
    {"dlaa", 1.0f},
}};
inline constexpr size_t kDefaultDlssModeIndex = 3;
inline constexpr uint32_t kDefaultDlssSharpness = 0;

inline std::optional<std::string> ParseDlssMode(std::string_view text) {
    const std::string t = CompactToken(text);
    for (const DlssModeInfo& m : kDlssModes) {
        if (t == CompactToken(m.name)) return std::string(m.name);
    }
    if (t == "ultraperf" || t == "up" || t == "ultra") return "ultra_performance";
    if (t == "perf" || t == "maxperformance") return "performance";
    if (t == "maxquality") return "quality";
    if (t == "native") return "dlaa";
    return std::nullopt;
}

inline size_t DlssModeIndex(std::string_view name) {
    for (size_t i = 0; i < kDlssModes.size(); ++i) {
        if (name == kDlssModes[i].name) return i;
    }
    return kDefaultDlssModeIndex;
}

inline float FsrScaleFactor(std::string_view quality) {
    for (const FsrQualityInfo& q : kFsrQualities) {
        if (quality == q.name) return q.factor;
    }
    return 1.0f;
}

inline std::optional<std::filesystem::path> ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    return RuntimePlatform::ExecutableDirectory();
#else
    // /proc/self/exe is a Linux-specific magic symlink to the running executable; readlink()
    // does not NUL-terminate and silently truncates if the buffer is too small, so this grows
    // the buffer until the result no longer fills it completely, the same doubling strategy as
    // the Windows branch above uses for GetModuleFileNameW.
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<size_t>(length));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

// The portable root this executable lives under, or nullopt for a normal installation. The answer
// cannot change while the process runs, so it is resolved exactly once: every user-state path
// derives from it and they must not disagree with each other.
inline const std::optional<std::filesystem::path>& PortableRootDirectory() {
    static const std::optional<std::filesystem::path> root = []() -> std::optional<std::filesystem::path> {
        const auto executableDirectory = ExecutableDirectory();
        if (!executableDirectory) {
            return std::nullopt;
        }
        std::filesystem::path current = *executableDirectory;
        for (int level = 0; level <= kPortableSearchDepth; ++level) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(current / kPortableMarkerFileName, ec)) {
                return current;
            }
            const auto parent = current.parent_path();
            if (parent.empty() || parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }();
    return root;
}

// Six-character disc ID from <root>/sys/boot.bin, or empty when absent/unreadable.
inline std::string DiscGameId(const std::filesystem::path& root) {
    std::ifstream file(root / "sys" / "boot.bin", std::ios::binary);
    char id[6] = {};
    if (!file || !file.read(id, sizeof id)) return {};
    return std::string(id, sizeof id);
}

// True when the folder holds Sin & Punishment: Star Successor game-partition data (any region;
// dvd.cpp enforces the exact region). The disc's UPDATE partition carries the same R2Vxxx ID but a
// tiny foreign FST, so a real game file (the stage module list) is required as well.
inline bool IsThisGamesDiscRoot(const std::filesystem::path& root) {
    std::error_code ec;
    return DiscGameId(root).compare(0, 3, "R2V") == 0 &&
           (std::filesystem::is_regular_file(root / "files" / "gs2.sel", ec) ||
            std::filesystem::is_regular_file(root / "files" / "AppWiiRelRso.sel", ec));
}

// One-time move of this game's data out of the shared pre-1.0.10 "WiiCompiled" folder. Only
// happens when that folder demonstrably belongs to this game (its Game/ or configured dvd_root
// is an R2V disc); another port's data there is left alone. Never throws: a failed step just
// means the player is asked for the disc again.
inline void MigrateLegacyApplicationData(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path legacy = directory.parent_path() / kLegacyApplicationDirectoryName;
    if (directory.empty() || fs::exists(directory, ec) || !fs::is_directory(legacy, ec)) return;

    const fs::path legacyGame = legacy / "Game";
    bool ours = IsThisGamesDiscRoot(legacyGame);
    if (!ours) {
        // An extracted folder configured elsewhere: [paths] dvd_root.
        try {
            std::ifstream stream(legacy / kConfigFileName, std::ios::binary);
            if (stream) {
                const auto data = toml::parse(stream, PathToUtf8(legacy / kConfigFileName));
                const auto text = toml::find<std::string>(data, "paths", "dvd_root");
                if (!text.empty()) {
                    fs::path root = PathFromUtf8(text);
                    if (root.is_relative()) root = legacy / root;
                    ours = IsThisGamesDiscRoot(root);
                }
            }
        } catch (...) {
        }
    }
    if (!ours) return;

    try {
        fs::create_directories(directory);
        for (const char* file : {kConfigFileName, "imgui.ini", "gamecontrollerdb.txt"}) {
            if (fs::is_regular_file(legacy / file, ec)) fs::copy_file(legacy / file, directory / file, ec);
        }
        for (const char* folder : {"NAND", "texture_replacements"}) {
            if (fs::is_directory(legacy / folder, ec)) {
                fs::copy(legacy / folder, directory / folder, fs::copy_options::recursive, ec);
            }
        }
        // The extracted disc is gigabytes and is only this game's: move it (same volume) instead of copying.
        if (IsThisGamesDiscRoot(legacyGame)) {
            fs::rename(legacyGame, directory / "Game", ec);
            if (!ec && fs::is_regular_file(legacy / "prepared.txt", ec)) {
                fs::rename(legacy / "prepared.txt", directory / "prepared.txt", ec);
            }
        }
    } catch (...) {
    }
}

inline std::filesystem::path DefaultApplicationDataDirectory();

inline std::filesystem::path ApplicationDataDirectory() {
    if (const auto& override = ApplicationDataDirectoryOverride()) {
        return *override;
    }
    if (const auto processOverride = ProcessArgumentDataDirectory()) {
        return processOverride->is_relative()
            ? std::filesystem::absolute(*processOverride).lexically_normal()
            : processOverride->lexically_normal();
    }
    // RecompLauncher supplies this for both Play and its setup step. A command-line --data
    // override is installed before the first path lookup and therefore takes precedence.
    if (const char* launcherData = std::getenv("RECOMPLAUNCHER_DATA");
        launcherData && *launcherData) {
        return PathFromUtf8(launcherData).lexically_normal();
    }
    if (const auto& portableRoot = PortableRootDirectory()) {
        return *portableRoot / kPortableUserDataDirectoryName;
    }
    static const std::filesystem::path directory = [] {
        std::filesystem::path path = DefaultApplicationDataDirectory();
        MigrateLegacyApplicationData(path);
        return path;
    }();
    return directory;
}

inline std::filesystem::path DefaultApplicationDataDirectory() {
#ifdef _WIN32
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath) {
        const std::filesystem::path directory = std::filesystem::path(rawPath) / kApplicationDirectoryName;
        CoTaskMemFree(rawPath);
        return directory;
    }
#elif defined(__APPLE__)
    return RuntimePlatform::ApplicationDataDirectory(kApplicationDirectoryName);
#else
    // XDG Base Directory spec equivalent of FOLDERID_LocalAppData: $XDG_DATA_HOME if set and
    // non-empty, otherwise its default of $HOME/.local/share.
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome && *xdgDataHome) {
        const std::filesystem::path directory(xdgDataHome);
        if (directory.is_absolute()) return directory / kApplicationDirectoryName;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::filesystem::path directory(home);
        if (directory.is_absolute()) return directory / ".local" / "share" / kApplicationDirectoryName;
    }
#endif
    return std::filesystem::current_path() / kApplicationDirectoryName;
}

inline std::filesystem::path ResolveConfigPath() {
    return ApplicationDataDirectory() / kConfigFileName;
}

inline void EnsureConfigFile() {
    const std::filesystem::path path = ResolveConfigPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || std::filesystem::exists(path, ec)) {
        return;
    }

    std::ofstream output(path);
    if (!output) {
        return;
    }
    output << "# Sin & Punishment: Star Successor user configuration\n"
              "# Set paths.dvd_root to the extracted game directory (contains sys/ and files/).\n\n"
              "[video]\n"
              "widescreen = true\n"
              "force_aspect = false\n"
              "resolution_multiplier = 0.0\n"
              "# Presentation rate: \"original\" (60), \"display\" (match the monitor), \"uncapped\" or an fps\n"
              "# number such as 90/120/144/165/180/240. Game logic, physics and audio stay at 60 Hz.\n"
              "frame_rate = \"original\"\n"
              "# Hard safety cap for every rate above original (60..1000).\n"
              "frame_rate_cap = 1000\n"
              "# Variable refresh rate (G-SYNC / FreeSync): \"auto\" (on for a >60 Hz display with a frame rate above\n"
              "# original), \"on\" or \"off\". VRR bypasses VSync and limits presents to refresh - vrr_margin_fps.\n"
              "vrr = \"auto\"\n"
              "vrr_margin_fps = 3\n"
              "vrr_limiter = true\n"
              "# Upscaler: \"off\" or \"fsr1\" (AMD FSR 1.0 EASU + RCAS; renders at a lower internal resolution and\n"
              "# upscales to the window). fsr_quality: native, ultra_quality (1.3x), quality (1.5x), balanced (1.7x),\n"
              "# performance (2x), ultra_performance (3x). fsr_sharpness: 0-100 (0 = no RCAS sharpening).\n"
              "upscaler = \"off\"\n"
              "fsr_quality = \"quality\"\n"
              "fsr_sharpness = 80\n"
              "# NVIDIA DLSS (upscaler = \"dlss\"; Windows, Direct3D 12, RTX GPU, nvngx_dlss.dll next to the exe). dlss_mode:\n"
              "# ultra_performance (33%), performance (50%), balanced (58%), quality (67%), dlaa (100%, anti-aliasing only).\n"
              "# dlss_motion_vectors: use image estimation when DLSS has no GX velocity buffer.\n"
              "dlss_mode = \"quality\"\n"
              "dlss_sharpness = 0\n"
              "dlss_motion_vectors = false\n"
              "display_mode = \"windowed\"\n"
              "graphics_api = \"auto\"\n"
              "skip_unready_pipelines = true\n"
              "vsync = false\n"
              "# Runs GX command processing on its own thread. Read once, at startup.\n"
              "gx_worker = true\n"
              "disable_copy_filter = true\n"
              "show_fps = true\n"
              "# Dolphin-style custom textures. When enabled, the renderer indexes\n"
              "# texture_replacements/ next to this file at startup and substitutes\n"
              "# any tex1_<W>x<H>_<hash>[_<tlut hash>]_<format>.dds or .png it finds\n"
              "# there for the matching game texture. texture_dumps writes every\n"
              "# unmatched texture to cache/texture_dumps under the name a\n"
              "# replacement would need. Both are read once, at startup.\n"
              "texture_replacements = false\n"
              "texture_dumps = false\n\n"
              "[audio]\n"
              "volume = 1.0\n"
              "muted = false\n"
              "# Runs the AX/DSP voice mix on its own thread, joined before the\n"
              "# guest can observe it. Set to false to mix inline on the guest\n"
              "# thread exactly as the runtime did before.\n"
              "mix_worker = true\n\n"
              "[controller]\n"
              "steam_input_override = true\n"
              "# steam_input_app_id = 480\n\n"
              "[paths]\n"
              "# dvd_root = \"D:\\\\Sin\\\\orig\\\\R2VE01\"\n"
              "# nand_root = \"D:\\\\WiiNand\"\n";
}

template <typename T>
inline std::optional<T> FindConfigValue(
    const toml::value& document, std::string_view section, std::string_view key) {
    try {
        return toml::find<T>(document, std::string(section), std::string(key));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::optional<uint32_t> FindConfigUint(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < 0 || static_cast<uint64_t>(*value) > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

inline std::optional<int32_t> FindConfigInt(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < INT32_MIN || *value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(*value);
}

inline std::optional<float> FindConfigFloat(
    const toml::value& document, std::string_view section, std::string_view key) {
    std::optional<double> value = FindConfigValue<double>(document, section, key);
    if (!value) {
        if (const auto integer = FindConfigValue<int64_t>(document, section, key)) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) ||
        *value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        *value > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

// Reads every supported setting out of a parsed Config.toml document.
inline RuntimeUserConfig ParseConfigDocument(const toml::value& document) {
    RuntimeUserConfig config;

    static constexpr std::array<std::string_view, 12> buttonKeys = {
        "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
    };
    for (size_t index = 0; index < buttonKeys.size(); ++index) {
        config.controllerButtons[index] =
            FindConfigValue<std::string>(document, "controller", buttonKeys[index]);
    }

    config.enhancedCoop = FindConfigValue<bool>(document, "controller", "enhanced_coop");
    config.secondRemote = FindConfigValue<int64_t>(document, "controller", "second_remote");
    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");
    config.motionTrick = FindConfigValue<bool>(document, "controller", "motion_trick");
    if (const auto value = FindConfigInt(document, "controller", "motion_trick_sensitivity"); value && *value >= 1 && *value <= 5) {
        config.motionTrickSensitivity = *value;
    }
    if (auto value = FindConfigInt(document, "audio", "mute_key")) {
        config.muteHotkey = *value;
    }

    if (const auto* section = document.contains("controller") ? &document.at("controller") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (key.rfind("expr_", 0) == 0 && value.is_string()) {
                config.controllerExpressions[key] = value.as_string();
                continue;
            }
            // "<button>_<port + 1>" overrides of the shared [controller] bindings. No button
            // name contains an underscore, so the last underscore always splits off the port.
            const auto split = key.find_last_of('_');
            if (split == std::string::npos || split == 0 || split + 2 != key.size() ||
                key[split + 1] < '1' || key[split + 1] > '4' || !value.is_string()) {
                continue;
            }
            const std::string button(key.substr(0, split));
            for (size_t index = 0; index < buttonKeys.size(); ++index) {
                if (button == buttonKeys[index]) {
                    config.controllerPortButtons[key[split + 1] - '1'][index] = value.as_string();
                    break;
                }
            }
        }
    }

    config.widescreen = FindConfigValue<bool>(document, "video", "widescreen");
    config.forceAspect = FindConfigValue<bool>(document, "video", "force_aspect");
    if (!config.forceAspect) {
        // Legacy key; it always forced a 16:9 game.
        config.forceAspect = FindConfigValue<bool>(document, "video", "force_16_9");
        config.legacyForceAspect = config.forceAspect.has_value();
        if (config.forceAspect.value_or(false)) {
            config.widescreen = true;
        }
    }
    config.windowPosX = FindConfigInt(document, "video", "window_x");
    config.windowPosY = FindConfigInt(document, "video", "window_y");
    if (auto value = FindConfigUint(document, "video", "window_width"); value && *value >= 160 && *value <= 16384) {
        config.windowWidth = *value;
    }
    if (auto value = FindConfigUint(document, "video", "window_height"); value && *value >= 120 && *value <= 16384) {
        config.windowHeight = *value;
    }
    if (auto value = FindConfigFloat(document, "video", "resolution_multiplier");
        value && IsSupportedResolutionMultiplier(*value)) {
        config.resolutionMultiplier = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "graphics_api")) {
        if (IsSupportedGraphicsApi(*value)) {
            config.graphicsApi = *value;
        } else {
            std::cerr << "[runtime] Unknown video.graphics_api=\"" << *value
                      << "\", using the automatic backend" << std::endl;
        }
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "display_mode");
        value && IsSupportedDisplayMode(*value)) {
        config.displayMode = *value;
    }
    if (auto value = FindConfigUint(document, "video", "frame_interpolation_fps")) {
        if (IsSupportedFrameInterpolationFps(*value)) {
            config.frameInterpolationFps = *value;
        }
    }
    {
        FrameRateSetting parsed;
        std::optional<std::string> text = FindConfigValue<std::string>(document, "video", "frame_rate");
        if (!text) {
            if (const auto number = FindConfigValue<int64_t>(document, "video", "frame_rate")) {
                text = std::to_string(*number);
            }
        }
        if (text) {
            if (ParseFrameRate(*text, parsed)) {
                config.frameRate = FormatFrameRate(parsed);
            } else {
                std::cerr << "[runtime] Unknown video.frame_rate=\"" << *text
                          << "\", using the original frame rate" << std::endl;
            }
        }
    }
    if (auto value = FindConfigUint(document, "video", "frame_rate_cap")) {
        config.frameRateCap = std::clamp<uint32_t>(*value, 60u, kFrameRateHardCap);
    }
    {
        std::optional<std::string> text = FindConfigValue<std::string>(document, "video", "vrr");
        if (!text) {
            if (const auto flag = FindConfigValue<bool>(document, "video", "vrr")) {
                text = *flag ? "on" : "off";
            }
        }
        if (text) {
            std::string t;
            for (const char c : *text) {
                if (!std::isspace(static_cast<unsigned char>(c)) && c != '"') {
                    t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                }
            }
            if (t == "auto" || t == "on" || t == "off") {
                config.vrr = t;
            } else if (t == "true" || t == "1" || t == "yes") {
                config.vrr = "on";
            } else if (t == "false" || t == "0" || t == "no") {
                config.vrr = "off";
            } else {
                std::cerr << "[runtime] Unknown video.vrr=\"" << *text << "\", using auto" << std::endl;
            }
        }
    }
    if (auto value = FindConfigUint(document, "video", "vrr_margin_fps")) {
        config.vrrMarginFps = std::min<uint32_t>(*value, 30u);
    }
    config.vrrLimiter = FindConfigValue<bool>(document, "video", "vrr_limiter");
    config.metalFxSpatialUpscaling =
        FindConfigValue<bool>(document, "video", "metalfx_spatial_upscaling");
    {
        std::optional<std::string> text = FindConfigValue<std::string>(document, "video", "upscaler");
        if (!text) {
            if (const auto flag = FindConfigValue<bool>(document, "video", "upscaler")) {
                text = *flag ? "fsr1" : "off";
            }
        }
        if (text) {
            if (const auto parsed = ParseUpscaler(*text)) {
                config.upscaler = *parsed;
            } else {
                std::cerr << "[runtime] Unknown video.upscaler=\"" << *text << "\", using off" << std::endl;
            }
        }
        if (const auto quality = FindConfigValue<std::string>(document, "video", "fsr_quality")) {
            if (const auto parsed = ParseFsrQuality(*quality)) {
                config.fsrQuality = *parsed;
            } else {
                std::cerr << "[runtime] Unknown video.fsr_quality=\"" << *quality << "\", using quality" << std::endl;
            }
        }
        if (const auto sharpness = FindConfigFloat(document, "video", "fsr_sharpness")) {
            config.fsrSharpness =
                static_cast<uint32_t>(std::lround(std::clamp(*sharpness, 0.0f, 100.0f)));
        }
        if (const auto mode = FindConfigValue<std::string>(document, "video", "dlss_mode")) {
            if (const auto parsed = ParseDlssMode(*mode)) {
                config.dlssMode = *parsed;
            } else {
                std::cerr << "[runtime] Unknown video.dlss_mode=\"" << *mode << "\", using quality" << std::endl;
            }
        }
        if (const auto sharpness = FindConfigFloat(document, "video", "dlss_sharpness")) {
            config.dlssSharpness =
                static_cast<uint32_t>(std::lround(std::clamp(*sharpness, 0.0f, 100.0f)));
        }
        config.frameGeneration = FindConfigValue<bool>(document, "video", "dlss_frame_generation");
        config.reflex = FindConfigValue<bool>(document, "video", "reflex");
        config.dlssMotionVectors = FindConfigValue<bool>(document, "video", "dlss_motion_vectors");
    }
    config.skipUnreadyPipelines = FindConfigValue<bool>(document, "video", "skip_unready_pipelines");
    config.vsync = FindConfigValue<bool>(document, "video", "vsync");
    config.gxWorker = FindConfigValue<bool>(document, "video", "gx_worker");
    config.disableCopyFilter = FindConfigValue<bool>(document, "video", "disable_copy_filter");
    config.showFps = FindConfigValue<bool>(document, "video", "show_fps");
    config.enhancementsEnabled = FindConfigValue<bool>(document, "video", "enhancements_enabled");
    config.sharpeningEnabled = FindConfigValue<bool>(document, "video", "sharpening_enabled");
    config.ambientOcclusion = FindConfigValue<bool>(document, "video", "ambient_occlusion");
    config.globalIllumination = FindConfigValue<bool>(document, "video", "global_illumination");
    config.rayTracedGi = FindConfigValue<bool>(document, "video", "ray_traced_gi");
    config.retainCulledGi = FindConfigValue<bool>(document, "video", "retain_culled_gi");
    config.strongBounce = FindConfigValue<bool>(document, "video", "strong_bounce");
    config.projectedCaustics = FindConfigValue<bool>(document, "video", "projected_caustics");
    config.surfaceHighlights = FindConfigValue<bool>(document, "video", "surface_highlights");
    config.probeMaterials = FindConfigValue<bool>(document, "video", "probe_materials");
    config.combatLight = FindConfigValue<bool>(document, "video", "combat_light");
    config.contactShadow = FindConfigValue<bool>(document, "video", "contact_shadow");
    config.stageAtmosphere = FindConfigValue<bool>(document, "video", "stage_atmosphere");
    config.pixelLighting = FindConfigValue<bool>(document, "video", "per_pixel_lighting");
    config.hdHud = FindConfigValue<bool>(document, "video", "hd_hud");
    config.hdWorld = FindConfigValue<bool>(document, "video", "hd_world");
    config.sceneGlow = FindConfigValue<bool>(document, "video", "scene_glow");
    config.enhancedBloom = FindConfigValue<bool>(document, "video", "enhanced_bloom");
    config.generateMips = FindConfigValue<bool>(document, "video", "generate_mips");
    config.textureReplacements = FindConfigValue<bool>(document, "video", "texture_replacements");
    config.textureDumps = FindConfigValue<bool>(document, "video", "texture_dumps");
    if (auto value = FindConfigUint(document, "video", "disabled_post_processing_paths");
        value && (*value & ~0x10u) == 0) {
        config.disabledPostProcessingPaths = *value & 0x10u;
    }

    auto readVolume = [&](std::string_view key) -> std::optional<float> {
        auto value = FindConfigFloat(document, "audio", key);
        return value && *value >= 0.0f && *value <= 1.0f ? value : std::nullopt;
    };
    config.audioVolume = readVolume("volume");
    config.audioMusicVolume = readVolume("music_volume");
    config.audioSoundEffectsVolume = readVolume("sound_effects_volume");
    config.audioUiVolume = readVolume("ui_volume");
    config.audioVoicesVolume = readVolume("voices_volume");
    config.audioMuted = FindConfigValue<bool>(document, "audio", "muted");
    config.audioMixWorker = FindConfigValue<bool>(document, "audio", "mix_worker");
    config.attenuateMusicWhenMediaPlays =
        FindConfigValue<bool>(document, "audio", "attenuate_music_when_media_plays");
    config.wiiRemotes = FindConfigValue<bool>(document, "controller", "wii_remotes");
    config.wiiAutoPair = FindConfigValue<bool>(document, "controller", "wii_auto_pair");
    if (const auto bar = FindConfigValue<std::string>(document, "controller", "wii_sensor_bar")) {
        if (*bar == "above" || *bar == "below") config.wiiSensorBarAbove = *bar == "above";
    }
    if (const auto value = FindConfigInt(document, "controller", "wii_ir_sensitivity"); value && *value >= 1 && *value <= 5) {
        config.wiiIrSensitivity = *value;
    }
    if (const auto value = FindConfigInt(document, "controller", "wii_speaker_volume"); value && *value >= 0 && *value <= 127) {
        config.wiiSpeakerVolume = *value;
    }
    config.wiiRumble = FindConfigValue<bool>(document, "controller", "wii_rumble");
    config.steamInputOverride = FindConfigValue<bool>(document, "controller", "steam_input_override");
    if (const auto value = FindConfigUint(document, "controller", "steam_input_app_id"); value && *value != 0) {
        config.steamInputAppId = *value;
    }

    config.nandRoot = FindConfigValue<std::string>(document, "paths", "nand_root");
    config.dvdRoot = FindConfigValue<std::string>(document, "paths", "dvd_root");
    return config;
}

inline RuntimeUserConfig ParseConfig(std::istream& input, std::string sourceName = "Config.toml") {
    try {
        return ParseConfigDocument(toml::parse(input, std::move(sourceName)));
    } catch (const std::exception& exception) {
        std::cerr << "[runtime-config] Invalid TOML; using built-in defaults: "
                  << exception.what() << std::endl;
        return {};
    }
}

inline bool WriteSetting(std::string_view section, std::string_view key, std::string_view value);

// A Config.toml that is not valid TOML (hand-edited, truncated by a crash or a full disk) would otherwise turn every
// later settings write into a silent no-op ("Not updating invalid ..."). Keep the broken file as Config.toml.invalid,
// start a fresh default file and carry the two path keys over, so the game still finds its data.
inline void RecoverInvalidConfigFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    auto keep = path;
    keep += ".invalid";
    std::filesystem::copy_file(path, keep, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(path, ec);
    EnsureConfigFile();
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        for (const std::string_view key : {std::string_view("dvd_root"), std::string_view("nand_root")}) {
            if (line.compare(start, key.size(), key) != 0) {
                continue;
            }
            const size_t eq = line.find('=', start + key.size());
            if (eq == std::string::npos || line.find_first_not_of(" \t", start + key.size()) != eq) {
                continue;
            }
            size_t end = line.find_last_not_of(" \t\r");
            const std::string literal = line.substr(eq + 1, end == std::string::npos ? 0 : end - eq);
            WriteSetting("paths", key, literal);  // ignored when the literal does not parse
        }
    }
    std::cerr << "[runtime-config] " << PathToUtf8(path) << " was not valid TOML. It was kept as "
              << PathToUtf8(keep) << " and a fresh default file was written." << std::endl;
}

inline RuntimeUserConfig LoadConfigFile() {
    EnsureConfigFile();
    const std::filesystem::path path = ResolveConfigPath();
    std::string text;
    if (std::ifstream input(path, std::ios::binary); input) {
        std::ostringstream buffer;
        buffer << input.rdbuf();
        text = buffer.str();
    } else {
        return RuntimeUserConfig{};
    }
    try {
        std::istringstream probe(text);
        (void)toml::parse(probe, PathToUtf8(path));
    } catch (const std::exception& exception) {
        std::cerr << "[runtime-config] Invalid TOML: " << exception.what() << std::endl;
        RecoverInvalidConfigFile(path, text);
        std::ifstream fresh(path, std::ios::binary);
        return fresh ? ParseConfig(fresh, PathToUtf8(path)) : RuntimeUserConfig{};
    }
    std::istringstream file(text);
    return ParseConfig(file, PathToUtf8(path));
}

inline const RuntimeUserConfig& Get() {
    static RuntimeUserConfig config = LoadConfigFile();
    return config;
}

inline RuntimeUserConfig& Mutable() {
    return const_cast<RuntimeUserConfig&>(Get());
}

inline constexpr std::array<std::string_view, 12> kControllerButtonKeys = {
    "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
};

inline const std::optional<std::string>& ControllerButton(size_t index) {
    static const std::optional<std::string> empty;
    return index < Get().controllerButtons.size() ? Get().controllerButtons[index] : empty;
}

// Sets section.key (section may be a dotted table path) to a TOML literal, keeping the rest of the
// document's keys, order and comments. Returns nullopt when the document or value does not parse.
inline std::optional<std::string> UpdateSettingText(const std::string& text, std::string_view section,
                                                    std::string_view key, std::string_view value) {
    try {
        auto root = toml::parse_str<toml::ordered_type_config>(text);
        auto literal = toml::parse_str<toml::ordered_type_config>("v = " + std::string(value));
        auto* table = &root;
        std::istringstream path{std::string(section)};
        for (std::string part; std::getline(path, part, '.');) {
            auto& parent = table->as_table();
            if (!parent.contains(part)) {
                parent[part] = toml::ordered_value(toml::ordered_table{});
            }
            table = &parent.at(part);
            if (!table->is_table()) {
                return std::nullopt;
            }
        }
        if (table->as_table_fmt().fmt == toml::table_format::implicit) {
            table->as_table_fmt().fmt = toml::table_format::multiline;
        }
        auto& slot = table->as_table()[std::string(key)];
        auto& updated = literal.as_table().at("v");
        updated.comments() = slot.comments();
        slot = std::move(updated);
        return toml::format(root);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Update one TOML value without discarding unrelated settings or user-specific paths. This is
// used by the in-game F10 settings bar. An unparseable file is left untouched.
inline bool WriteSetting(std::string_view section, std::string_view key, std::string_view value) {
    const auto path = ResolveConfigPath();
    std::ostringstream existing;
    if (std::ifstream input(path, std::ios::binary); input) {
        existing << input.rdbuf();
    }
    const auto updated = UpdateSettingText(existing.str(), section, key, value);
    if (!updated) {
        std::cerr << "[runtime-config] Not updating invalid " << PathToUtf8(path) << std::endl;
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "[runtime-config] Unable to write " << PathToUtf8(path) << std::endl;
        return false;
    }
    output << *updated;
    return static_cast<bool>(output);
}

inline std::string FormatString(std::string_view value) {
    return toml::format(toml::value(std::string(value)));
}

inline bool SetResolutionMultiplier(float value) {
    Mutable().resolutionMultiplier = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("video", "resolution_multiplier", formatted.str());
}

inline bool SetWindowSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    Mutable().windowWidth = width;
    Mutable().windowHeight = height;
    const bool wroteWidth = WriteSetting("video", "window_width", std::to_string(width));
    const bool wroteHeight = WriteSetting("video", "window_height", std::to_string(height));
    return wroteWidth && wroteHeight;
}

inline bool SetWindowPosition(int32_t x, int32_t y) {
    Mutable().windowPosX = x;
    Mutable().windowPosY = y;
    const bool wroteX = WriteSetting("video", "window_x", std::to_string(x));
    const bool wroteY = WriteSetting("video", "window_y", std::to_string(y));
    return wroteX && wroteY;
}

inline bool SetFrameInterpolationFps(uint32_t value) {
    if (!IsSupportedFrameInterpolationFps(value)) {
        return false;
    }
    Mutable().frameInterpolationFps = value;
    return WriteSetting("video", "frame_interpolation_fps", std::to_string(value));
}

inline bool SetFrameRate(const FrameRateSetting& value) {
    Mutable().frameRate = FormatFrameRate(value);
    return WriteSetting("video", "frame_rate", FormatString(*Mutable().frameRate));
}

inline bool SetFrameRateCap(uint32_t value) {
    value = std::clamp<uint32_t>(value, 60u, kFrameRateHardCap);
    Mutable().frameRateCap = value;
    return WriteSetting("video", "frame_rate_cap", std::to_string(value));
}

inline bool SetMetalFxSpatialUpscaling(bool value) {
    Mutable().metalFxSpatialUpscaling = value;
    return WriteSetting("video", "metalfx_spatial_upscaling", value ? "true" : "false");
}

inline bool SetUpscaler(const std::string& value) {
    const auto parsed = ParseUpscaler(value);
    if (!parsed) {
        return false;
    }
    Mutable().upscaler = *parsed;
    return WriteSetting("video", "upscaler", FormatString(*parsed));
}

inline bool SetFsrQuality(const std::string& value) {
    const auto parsed = ParseFsrQuality(value);
    if (!parsed) {
        return false;
    }
    Mutable().fsrQuality = *parsed;
    return WriteSetting("video", "fsr_quality", FormatString(*parsed));
}

inline bool SetFsrSharpness(uint32_t value) {
    value = std::min<uint32_t>(value, 100u);
    Mutable().fsrSharpness = value;
    return WriteSetting("video", "fsr_sharpness", std::to_string(value));
}

inline bool SetDlssMode(const std::string& value) {
    const auto parsed = ParseDlssMode(value);
    if (!parsed) {
        return false;
    }
    Mutable().dlssMode = *parsed;
    return WriteSetting("video", "dlss_mode", FormatString(*parsed));
}

inline bool SetDlssSharpness(uint32_t value) {
    value = std::min<uint32_t>(value, 100u);
    Mutable().dlssSharpness = value;
    return WriteSetting("video", "dlss_sharpness", std::to_string(value));
}

inline bool SetFrameGeneration(bool value) {
    Mutable().frameGeneration = value;
    return WriteSetting("video", "dlss_frame_generation", value ? "true" : "false");
}

inline bool SetReflex(bool value) {
    Mutable().reflex = value;
    return WriteSetting("video", "reflex", value ? "true" : "false");
}
inline bool SetDlssMotionVectors(bool value) {
    Mutable().dlssMotionVectors = value;
    return WriteSetting("video", "dlss_motion_vectors", value ? "true" : "false");
}

inline bool SetDisplayMode(std::string value) {
    if (!IsSupportedDisplayMode(value)) {
        return false;
    }
    Mutable().displayMode = value;
    return WriteSetting("video", "display_mode", FormatString(value));
}

inline bool SetSkipUnreadyPipelines(bool value) {
    Mutable().skipUnreadyPipelines = value;
    return WriteSetting("video", "skip_unready_pipelines", value ? "true" : "false");
}

inline bool SetVrr(std::string_view value) {
    Mutable().vrr = std::string(value);
    return WriteSetting("video", "vrr", FormatString(value));
}

inline bool SetVrrMarginFps(uint32_t value) {
    value = std::min<uint32_t>(value, 30u);
    Mutable().vrrMarginFps = value;
    return WriteSetting("video", "vrr_margin_fps", std::to_string(value));
}

inline bool SetVrrLimiter(bool value) {
    Mutable().vrrLimiter = value;
    return WriteSetting("video", "vrr_limiter", value ? "true" : "false");
}

inline bool SetVsync(bool value) {
    Mutable().vsync = value;
    return WriteSetting("video", "vsync", value ? "true" : "false");
}

inline bool SetDisableCopyFilter(bool value) {
    Mutable().disableCopyFilter = value;
    return WriteSetting("video", "disable_copy_filter", value ? "true" : "false");
}

inline bool SetShowFps(bool value) {
    Mutable().showFps = value;
    return WriteSetting("video", "show_fps", value ? "true" : "false");
}

inline bool SetDisabledPostProcessingPaths(uint32_t value) {
    Mutable().disabledPostProcessingPaths = value;
    std::ostringstream formatted;
    formatted << "0x" << std::hex << std::uppercase << value;
    return WriteSetting("video", "disabled_post_processing_paths", formatted.str());
}

inline bool SetControllerButton(size_t index, std::string value) {
    if (index >= kControllerButtonKeys.size()) {
        return false;
    }
    Mutable().controllerButtons[index] = value;
    return WriteSetting("controller", kControllerButtonKeys[index], FormatString(value));
}

// Per-port bindings are stored as "[controller] <key>_<port + 1>" (a_1 .. right_4). The shared
// [controller] bindings remain the fallback for ports that were never rebound individually.
inline constexpr size_t kControllerPortCount = 4;

inline std::string PortControllerButtonKey(uint32_t port, size_t index) {
    if (port >= kControllerPortCount || index >= kControllerButtonKeys.size()) {
        return {};
    }
    return std::string(kControllerButtonKeys[index]) + "_" + std::to_string(port + 1);
}

// The port's own override when set, else the shared binding that every port used before
// per-port bindings existed.
inline const std::optional<std::string>& PortControllerButton(uint32_t port, size_t index) {
    static const std::optional<std::string> empty;
    if (port < kControllerPortCount && index < kControllerButtonKeys.size()) {
        const auto& portValue = Get().controllerPortButtons[port][index];
        if (portValue) {
            return portValue;
        }
    }
    return ControllerButton(index);
}

inline bool SetPortControllerButton(uint32_t port, size_t index, std::string value) {
    const std::string key = PortControllerButtonKey(port, index);
    if (key.empty()) {
        return false;
    }
    Mutable().controllerPortButtons[port][index] = value;
    return WriteSetting("controller", key, FormatString(value));
}

inline std::string ControllerExpression(const std::string& key) {
    const auto it = Get().controllerExpressions.find(key);
    return it == Get().controllerExpressions.end() ? std::string() : it->second;
}

inline bool SetControllerExpression(const std::string& key, const std::string& value) {
    Mutable().controllerExpressions[key] = value;
    return WriteSetting("controller", key, FormatString(value));
}

inline bool EnhancedCoopSupportedForGame() {
    // The enhanced co-op patchset is translated only for R2VE01. Keep the
    // shared configuration value intact, but never activate partial hooks in
    // the Japanese build. Standalone runtime tests do not define a translated
    // region; they exercise the default USA behavior.
#if defined(MKW_TRANSLATED_GUEST_REGION)
    return MKW_TRANSLATED_GUEST_REGION != 2;
#else
    return true;
#endif
}
inline bool EnhancedCoopAtLaunch() {
    static const bool enabled = EnhancedCoopSupportedForGame() && Get().enhancedCoop.value_or(false);
    return enabled;
}
inline bool SetEnhancedCoop(bool enabled) {
    if (!WriteSetting("controller", "enhanced_coop", enabled ? "true" : "false")) return false;
    Mutable().enhancedCoop = enabled;
    return true;
}

inline int SecondRemoteSetting() {
    const auto value = Get().secondRemote.value_or(0);
    // Older profiles can explicitly select the raw GameCube input path.
    // Enhanced co-op must run SDL pads through the same settings on both seats.
    if (EnhancedCoopSupportedForGame() && Get().enhancedCoop.value_or(false)) return value == 2 ? 2 : 1;
    return value == 1 || value == 2 ? static_cast<int>(value) : 0;
}

// Keep ownership stable for the lifetime of the guest Bluetooth session.
inline int SecondRemoteAtLaunch() {
    static const int value = SecondRemoteSetting();
    return value;
}

inline bool SetSecondRemote(int value) {
    if (value < 0 || value > 2 || !WriteSetting("controller", "second_remote", std::to_string(value))) return false;
    Mutable().secondRemote = value;
    return true;
}

inline bool RumbleEnabled(bool fallback = true) {
    return Get().rumbleEnabled.value_or(fallback);
}

inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}

inline bool MotionTrickEnabled(bool fallback = false) {
    return Get().motionTrick.value_or(fallback);
}

inline bool SetMotionTrickEnabled(bool value) {
    Mutable().motionTrick = value;
    return WriteSetting("controller", "motion_trick", value ? "true" : "false");
}

inline int32_t MotionTrickSensitivity(int32_t fallback = 3) {
    return Get().motionTrickSensitivity.value_or(fallback);
}

inline bool SetMotionTrickSensitivity(int32_t value) {
    Mutable().motionTrickSensitivity = value;
    return WriteSetting("controller", "motion_trick_sensitivity", std::to_string(value));
}

inline int32_t MuteHotkey(int32_t fallback) {
    return Get().muteHotkey.value_or(fallback);
}

inline bool SetMuteHotkey(int32_t value) {
    Mutable().muteHotkey = value;
    return WriteSetting("audio", "mute_key", std::to_string(value));
}

inline bool SetAudioVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "volume", formatted.str());
}

inline bool SetMusicVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioMusicVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "music_volume", formatted.str());
}

inline bool SetSoundEffectsVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioSoundEffectsVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "sound_effects_volume", formatted.str());
}

inline bool SetUiVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioUiVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "ui_volume", formatted.str());
}

inline bool SetVoicesVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVoicesVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "voices_volume", formatted.str());
}

inline bool SetAudioMuted(bool value) {
    Mutable().audioMuted = value;
    return WriteSetting("audio", "muted", value ? "true" : "false");
}

inline bool SetAudioMixWorker(bool value) {
    Mutable().audioMixWorker = value;
    return WriteSetting("audio", "mix_worker", value ? "true" : "false");
}

inline bool SetAttenuateMusicWhenMediaPlays(bool value) {
    Mutable().attenuateMusicWhenMediaPlays = value;
    return WriteSetting("audio", "attenuate_music_when_media_plays", value ? "true" : "false");
}

inline bool WidescreenEnabled(bool fallback = false) {
    return Get().widescreen.value_or(fallback);
}

inline bool ForceAspectEnabled(bool fallback = false) {
    return Get().forceAspect.value_or(fallback);
}

inline bool SetForceAspect(bool value) {
    Mutable().forceAspect = value;
    return WriteSetting("video", "force_aspect", value ? "true" : "false");
}

// The in-memory value stays as booted; SCGetAspectRatio must not change mid-session.
inline bool SetWidescreen(bool value) {
    return WriteSetting("video", "widescreen", value ? "true" : "false");
}

// Rewrites a legacy force_16_9 as force_aspect plus the widescreen it implied.
inline void MigrateLegacyForceAspect() {
    if (!Get().legacyForceAspect) {
        return;
    }
    const bool force = ForceAspectEnabled();
    if ((!force || WriteSetting("video", "widescreen", "true")) &&
        WriteSetting("video", "force_aspect", force ? "true" : "false")) {
        Mutable().legacyForceAspect = false;
    }
}

inline bool WindowPosition(int32_t& x, int32_t& y) {
    if (!Get().windowPosX || !Get().windowPosY) {
        return false;
    }
    x = *Get().windowPosX;
    y = *Get().windowPosY;
    return true;
}

inline uint32_t WindowWidth(uint32_t fallback) {
    return Get().windowWidth.value_or(fallback);
}

inline uint32_t WindowHeight(uint32_t fallback) {
    return Get().windowHeight.value_or(fallback);
}

inline float ResolutionMultiplier(float fallback = 0.0f) {
    return std::max(0.0f, Get().resolutionMultiplier.value_or(fallback));
}

inline bool MetalFxSpatialUpscaling(bool fallback = false) {
    return Get().metalFxSpatialUpscaling.value_or(fallback);
}

inline std::string Upscaler(std::string fallback = "off") {
    return Get().upscaler.value_or(std::move(fallback));
}

inline std::string FsrQuality(std::string fallback = "quality") {
    return Get().fsrQuality.value_or(std::move(fallback));
}

inline uint32_t FsrSharpness(uint32_t fallback = kDefaultFsrSharpness) {
    return std::min<uint32_t>(Get().fsrSharpness.value_or(fallback), 100u);
}

inline std::string DlssMode(std::string fallback = kDlssModes[kDefaultDlssModeIndex].name) {
    return Get().dlssMode.value_or(std::move(fallback));
}

inline uint32_t DlssSharpness(uint32_t fallback = kDefaultDlssSharpness) {
    return std::min<uint32_t>(Get().dlssSharpness.value_or(fallback), 100u);
}

inline bool DlssMotionVectors(bool fallback = false) {
    return Get().dlssMotionVectors.value_or(fallback);
}

inline float AudioVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float MusicVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioMusicVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float SoundEffectsVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioSoundEffectsVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float UiVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioUiVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float VoicesVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVoicesVolume.value_or(fallback), 0.0f, 1.0f);
}

inline bool AudioMuted(bool fallback = false) {
    return Get().audioMuted.value_or(fallback);
}

// Off-thread AX/DSP mix. Default on; false restores the fully synchronous mix.
inline bool AudioMixWorkerEnabled(bool fallback = true) {
    return Get().audioMixWorker.value_or(fallback);
}

inline bool GxWorkerEnabled(bool fallback = true) {
    return Get().gxWorker.value_or(fallback);
}

// Whether background music should duck automatically for other media playback.
inline bool AttenuateMusicWhenMediaPlays(bool fallback = false) {
    return Get().attenuateMusicWhenMediaPlays.value_or(fallback);
}

// Bluetooth Wii Remotes. Read once before SDL's joystick subsystem comes up, so a
// change only takes effect on the next launch.
// On by default: a physical Wii Remote + Nunchuk is passed straight to the game
// with its original controls. Turn it off only to use a Wii U Pro Controller as a gamepad.
inline bool WiiRemotesEnabled(bool fallback = true) {
    return Get().wiiRemotes.value_or(fallback);
}

inline bool SetWiiRemotesEnabled(bool value) {
    if (!WriteSetting("controller", "wii_remotes", value ? "true" : "false")) return false;
    Mutable().wiiRemotes = value;
    return true;
}

inline bool SteamInputOverrideEnabled(bool fallback = true) {
    return Get().steamInputOverride.value_or(fallback);
}

// Spacewar, Valve's Steamworks sample app, available to every Steam account.
inline uint32_t SteamInputAppId(uint32_t fallback = 480) {
    return Get().steamInputAppId.value_or(fallback);
}

// Off by default: continuous Bluetooth inquiry disturbs headphones for players without
// a Wii Remote. Paired remotes reconnect with any button; Pair searches on request.
inline bool WiiAutoPairEnabled(bool fallback = false) {
    return Get().wiiAutoPair.value_or(fallback);
}

inline bool SetWiiAutoPairEnabled(bool value) {
    Mutable().wiiAutoPair = value;
    return WriteSetting("controller", "wii_auto_pair", value ? "true" : "false");
}

// Defaults are the console's.
inline bool WiiSensorBarAbove(bool fallback = false) {
    return Get().wiiSensorBarAbove.value_or(fallback);
}

inline bool SetWiiSensorBarAbove(bool value) {
    Mutable().wiiSensorBarAbove = value;
    return WriteSetting("controller", "wii_sensor_bar", value ? "\"above\"" : "\"below\"");
}

inline int32_t WiiIrSensitivity(int32_t fallback = 3) {
    return Get().wiiIrSensitivity.value_or(fallback);
}

inline bool SetWiiIrSensitivity(int32_t value) {
    Mutable().wiiIrSensitivity = value;
    return WriteSetting("controller", "wii_ir_sensitivity", std::to_string(value));
}

inline int32_t WiiSpeakerVolume(int32_t fallback = 0x59) {
    return Get().wiiSpeakerVolume.value_or(fallback);
}

inline bool SetWiiSpeakerVolume(int32_t value) {
    Mutable().wiiSpeakerVolume = value;
    return WriteSetting("controller", "wii_speaker_volume", std::to_string(value));
}

inline bool WiiRumbleEnabled(bool fallback = true) {
    return Get().wiiRumble.value_or(fallback);
}

inline bool SetWiiRumbleEnabled(bool value) {
    Mutable().wiiRumble = value;
    return WriteSetting("controller", "wii_rumble", value ? "true" : "false");
}

// Target frame rate for frame interpolation, or 0 to disable it.
inline uint32_t FrameInterpolationFps(uint32_t fallback = 0) {
    return Get().frameInterpolationFps.value_or(fallback);
}

// Configured presentation rate. video.frame_rate wins; otherwise the legacy
// video.frame_interpolation_fps value is migrated (0 -> original, N -> fixed N).
inline FrameRateSetting FrameRate() {
    const RuntimeUserConfig& config = Get();
    FrameRateSetting parsed;
    if (config.frameRate && ParseFrameRate(*config.frameRate, parsed)) {
        return parsed;
    }
    if (config.frameInterpolationFps && *config.frameInterpolationFps > 60) {
        return {FrameRateKind::Fixed, *config.frameInterpolationFps};
    }
    return {};
}

inline uint32_t FrameRateCap(uint32_t fallback = kFrameRateHardCap) {
    return Get().frameRateCap.value_or(fallback);
}

// Whether to skip draws whose graphics pipeline has not finished compiling yet.
inline bool SkipUnreadyPipelines(bool fallback = true) {
    return Get().skipUnreadyPipelines.value_or(fallback);
}

// "auto", "on" or "off".
inline std::string Vrr() {
    return Get().vrr.value_or("auto");
}

inline uint32_t VrrMarginFps(uint32_t fallback = 3) {
    return Get().vrrMarginFps.value_or(fallback);
}

inline bool VrrLimiter(bool fallback = true) {
    return Get().vrrLimiter.value_or(fallback);
}

inline bool Vsync(bool fallback = false) {
    return Get().vsync.value_or(fallback);
}

inline bool DisableCopyFilter(bool fallback = true) {
    return Get().disableCopyFilter.value_or(fallback);
}

inline bool ShowFps(bool fallback = true) {
    return Get().showFps.value_or(fallback);
}

inline bool EnhancementsEnabled() { return Get().enhancementsEnabled.value_or(true); }
inline bool SharpeningEnabled() { return Get().sharpeningEnabled.value_or(true); }
inline bool AmbientOcclusion() { return Get().ambientOcclusion.value_or(false); }
inline bool GlobalIllumination() { return Get().globalIllumination.value_or(false); }
inline bool RayTracedGi() { return Get().rayTracedGi.value_or(false); }
inline bool RetainCulledGi() { return Get().retainCulledGi.value_or(false); }
inline bool StrongBounce() { return Get().strongBounce.value_or(true); }
inline bool ProjectedCaustics() { return Get().projectedCaustics.value_or(false); }
inline bool SurfaceHighlights() { return Get().surfaceHighlights.value_or(false); }
inline bool ProbeMaterials() { return Get().probeMaterials.value_or(false); }
inline bool CombatLight() { return Get().combatLight.value_or(false); }
inline bool ContactShadow() { return Get().contactShadow.value_or(false); }
inline bool StageAtmosphere() { return Get().stageAtmosphere.value_or(false); }
inline bool PixelLighting() { return Get().pixelLighting.value_or(false); }
// Built-in high-resolution HUD/GUI textures from hd_hud/ beside the executable (default on).
inline bool HdHud() { return Get().hdHud.value_or(true); }
// Built-in HD environment, object and character textures from hd_world/ beside the executable (default on).
inline bool HdWorld() { return Get().hdWorld.value_or(true); }
inline bool SceneGlow() { return Get().sceneGlow.value_or(true); }
inline bool EnhancedBloom() { return Get().enhancedBloom.value_or(false); }
inline bool GenerateMips() { return Get().generateMips.value_or(false); }

inline bool TextureReplacements(bool fallback = false) {
    return Get().textureReplacements.value_or(fallback);
}

// Dumping only produces the names a replacement would need, so main.cpp
// gates it on TextureReplacements() as well.
inline bool TextureDumps(bool fallback = false) {
    return Get().textureDumps.value_or(fallback);
}

inline uint32_t DisabledPostProcessingPaths(uint32_t fallback = 0) {
    return Get().disabledPostProcessingPaths.value_or(fallback) & 0x10u;
}

inline std::string GraphicsApi(std::string fallback = "auto") {
    return Get().graphicsApi.value_or(std::move(fallback));
}

inline std::string DisplayMode(std::string fallback = "windowed") {
    return Get().displayMode.value_or(std::move(fallback));
}

inline std::string NandRoot(std::string fallback = "") {
    return Get().nandRoot.value_or(std::move(fallback));
}

inline std::string DvdRoot(std::string fallback = "") {
    return Get().dvdRoot.value_or(std::move(fallback));
}

// The one resolver for configured paths. A relative value means the same thing
// everywhere it can be configured: relative to the config file that named it,
// never to the process working directory (docs/WHEELWIZARD_CONTRACT.md).
inline std::filesystem::path ResolveRelativeTo(const std::filesystem::path& base,
                                               const std::string& value) {
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal();
}

inline std::filesystem::path ResolveRelativeToConfig(const std::string& value) {
    return ResolveRelativeTo(ResolveConfigPath().parent_path(), value);
}

// The extracted DATA directory. Empty when nothing is configured.
inline std::filesystem::path ResolvedDvdRoot() {
    const std::string configured = DvdRoot();
    return configured.empty() ? std::filesystem::path{} : ResolveRelativeToConfig(configured);
}

inline void LogLoadedConfig() {
    static const bool logged = [] {
        const auto& config = Get();
        const auto configPath = ResolveConfigPath();
        std::cout << "[runtime-config] " << PathToUtf8(configPath);
        if (!std::filesystem::exists(configPath)) {
            std::cout << " not found; using built-in defaults";
        } else {
            std::cout << " loaded";
            if (config.widescreen) {
                std::cout << " widescreen=" << (*config.widescreen ? "true" : "false");
            }
            if (config.windowWidth || config.windowHeight) {
                std::cout << " window=" << config.windowWidth.value_or(0) << "x"
                          << config.windowHeight.value_or(0);
            }
            if (config.resolutionMultiplier) {
                std::cout << " resolution_multiplier=" << *config.resolutionMultiplier;
            }
            if (config.dvdRoot) {
                std::cout << " dvd_root=" << *config.dvdRoot;
            }
            if (config.graphicsApi) {
                std::cout << " graphics_api=" << *config.graphicsApi;
            }
            if (config.frameInterpolationFps) {
                std::cout << " frame_interpolation_fps=" << *config.frameInterpolationFps;
            }
            if (config.frameRate) {
                std::cout << " frame_rate=" << *config.frameRate;
            }
            if (config.frameRateCap) {
                std::cout << " frame_rate_cap=" << *config.frameRateCap;
            }
            if (config.vrr) {
                std::cout << " vrr=" << *config.vrr;
            }
            if (config.vrrMarginFps) {
                std::cout << " vrr_margin_fps=" << *config.vrrMarginFps;
            }
            if (config.vrrLimiter) {
                std::cout << " vrr_limiter=" << (*config.vrrLimiter ? "true" : "false");
            }
            if (config.skipUnreadyPipelines) {
                std::cout << " skip_unready_pipelines=" << (*config.skipUnreadyPipelines ? "true" : "false");
            }
            if (config.vsync) {
                std::cout << " vsync=" << (*config.vsync ? "true" : "false");
            }
            if (config.disableCopyFilter) {
                std::cout << " disable_copy_filter=" << (*config.disableCopyFilter ? "true" : "false");
            }
            if (config.showFps) {
                std::cout << " show_fps=" << (*config.showFps ? "true" : "false");
            }
            if (config.textureReplacements) {
                std::cout << " texture_replacements=" << (*config.textureReplacements ? "true" : "false");
            }
            if (config.textureDumps) {
                std::cout << " texture_dumps=" << (*config.textureDumps ? "true" : "false");
            }
            if (config.audioVolume) {
                std::cout << " audio_volume=" << *config.audioVolume;
            }
            if (config.audioMuted) {
                std::cout << " audio_muted=" << (*config.audioMuted ? "true" : "false");
            }
            if (config.nandRoot) {
                std::cout << " nand_root=" << *config.nandRoot;
            }
        }
        std::cout << std::endl;
        return true;
    }();
    (void)logged;
}

} // namespace RuntimeConfigFile
