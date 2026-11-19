#pragma once

// Persisted options of the in-game options menu that have no home in runtime_config.h:
//   [video]  scaling_filter = "bilinear" | "area"   (live)
//            msaa = 1 | 2 | 4 | 8                   (restart; AuroraConfig.msaa)
//            anisotropy = 1 | 2 | 4 | 8 | 16        (restart; AuroraConfig.maxTextureAnisotropy)
//   [system] config_version = 1
//            startup_cover = "auto" | "skip" | "button"   (next launch)
//            show_frame_time = false                      (live)
// Loading is tolerant: missing or unknown keys keep defaults and values are clamped.

#include "runtime_config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace SystemSettings {

inline constexpr int kConfigVersion = 1;

enum class StartupCover : uint8_t { Auto, Skip, Button };
inline constexpr std::array<const char*, 3> kStartupCoverKeys = {"auto", "skip", "button"};
inline constexpr std::array<const char*, 3> kStartupCoverLabels = {
    "Automatic (about 2 seconds)", "Skip", "Show until a button is pressed"};

inline constexpr std::array<const char*, 2> kScalingFilterKeys = {"bilinear", "area"};
inline constexpr std::array<const char*, 2> kScalingFilterLabels = {"Bilinear (sharper)", "Area (smoother)"};

inline constexpr std::array<int, 4> kMsaaChoices = {1, 2, 4, 8};
inline constexpr std::array<int, 5> kAnisotropyChoices = {1, 2, 4, 8, 16};

struct Values {
    int scalingFilter = 0;       // index into kScalingFilterKeys
    int msaa = 1;
    int anisotropy = 16;         // aurora's own default when the config leaves it unset
    StartupCover startupCover = StartupCover::Auto;
    bool showFrameTime = false;
};

inline int NearestChoice(int value, const int* choices, size_t count) {
    int best = choices[0];
    for (size_t i = 0; i < count; ++i) {
        if (std::abs(choices[i] - value) < std::abs(best - value)) best = choices[i];
    }
    return best;
}

inline void Sanitize(Values& v) {
    v.scalingFilter = std::clamp(v.scalingFilter, 0, static_cast<int>(kScalingFilterKeys.size()) - 1);
    v.msaa = NearestChoice(v.msaa, kMsaaChoices.data(), kMsaaChoices.size());
    v.anisotropy = NearestChoice(v.anisotropy, kAnisotropyChoices.data(), kAnisotropyChoices.size());
}

namespace detail {

struct State {
    std::mutex mutex;
    bool loaded = false;
    Values values;
};

inline State& Shared() {
    static State state;
    return state;
}

template <size_t N>
int IndexOf(const std::array<const char*, N>& keys, const std::string& value, int fallback) {
    for (size_t i = 0; i < N; ++i) if (value == keys[i]) return static_cast<int>(i);
    return fallback;
}

inline void Load(Values& v) {
    std::ifstream file(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
    if (!file) return;
    toml::value d;
    try {
        d = toml::parse(file, "Config.toml");
    } catch (const std::exception&) {
        return;
    }
    using RuntimeConfigFile::FindConfigInt;
    using RuntimeConfigFile::FindConfigValue;
    if (const auto s = FindConfigValue<std::string>(d, "video", "scaling_filter")) {
        v.scalingFilter = IndexOf(kScalingFilterKeys, *s, v.scalingFilter);
    }
    if (const auto n = FindConfigInt(d, "video", "msaa")) v.msaa = *n;
    if (const auto n = FindConfigInt(d, "video", "anisotropy")) v.anisotropy = *n;
    if (const auto s = FindConfigValue<std::string>(d, "system", "startup_cover")) {
        v.startupCover = static_cast<StartupCover>(IndexOf(kStartupCoverKeys, *s, static_cast<int>(v.startupCover)));
    }
    if (const auto b = FindConfigValue<bool>(d, "system", "show_frame_time")) v.showFrameTime = *b;
}

} // namespace detail

inline Values Get() {
    detail::State& state = detail::Shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.loaded) {
        state.loaded = true;
        detail::Load(state.values);
        Sanitize(state.values);
    }
    return state.values;
}

inline void Set(Values values) {
    Get();
    Sanitize(values);
    detail::State& state = detail::Shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.values = values;
}

inline bool Save() {
    const Values v = Get();
    bool ok = true;
    ok &= RuntimeConfigFile::WriteSetting("system", "config_version", std::to_string(kConfigVersion));
    ok &= RuntimeConfigFile::WriteSetting("video", "scaling_filter",
                                          RuntimeConfigFile::FormatString(kScalingFilterKeys[v.scalingFilter]));
    ok &= RuntimeConfigFile::WriteSetting("video", "msaa", std::to_string(v.msaa));
    ok &= RuntimeConfigFile::WriteSetting("video", "anisotropy", std::to_string(v.anisotropy));
    ok &= RuntimeConfigFile::WriteSetting("system", "startup_cover",
                                          RuntimeConfigFile::FormatString(kStartupCoverKeys[static_cast<size_t>(v.startupCover)]));
    ok &= RuntimeConfigFile::WriteSetting("system", "show_frame_time", v.showFrameTime ? "true" : "false");
    return ok;
}

// Stamps config_version (and the defaults of this header) into a Config.toml that predates it.
inline void EnsureVersionStamp() {
    std::ifstream file(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
    if (file) {
        try {
            const toml::value d = toml::parse(file, "Config.toml");
            if (RuntimeConfigFile::FindConfigInt(d, "system", "config_version")) return;
        } catch (const std::exception&) {
            return;  // never rewrite a file that does not parse
        }
    }
    Save();
}

} // namespace SystemSettings
