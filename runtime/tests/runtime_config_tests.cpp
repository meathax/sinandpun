#include "runtime_config.h"
#include "launcher_args.h"

#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {
bool Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}
std::optional<toml::ordered_value> Update(const std::string& original, const char* section, const char* key,
                                          const char* value, std::string* text = nullptr) {
    const auto updated = RuntimeConfigFile::UpdateSettingText(original, section, key, value);
    if (!updated) return std::nullopt;
    if (text) *text = *updated;
    return toml::parse_str<toml::ordered_type_config>(*updated);
}
bool SetsFps(const std::string& original, const char* message) {
    const auto document = Update(original, "video", "show_fps", "true");
    return Require(document && document->at("video").at("show_fps").as_boolean() &&
                       document->at("video").as_table().size() == 1,
                   message);
}
bool SetsOffset(const std::string& original, const char* message) {
    const auto document = Update(original, "controller.wii_accel_offsets", "x", "0.5");
    return Require(document && document->at("controller").at("wii_accel_offsets").at("x").as_floating() == 0.5,
                   message);
}
} // namespace

int main() {
#if defined(__linux__)
    // Do not let launcher overrides mask the default XDG path regression.
    unsetenv("RECOMPLAUNCHER_DATA");
    if (setenv("HOME", "/tmp/sin-config-home", 1) != 0 ||
        setenv("XDG_DATA_HOME", "relative-data", 1) != 0) return 1;
    if (!RuntimeConfigFile::PortableRootDirectory() &&
        !Require(RuntimeConfigFile::ApplicationDataDirectory() ==
                     std::filesystem::path("/tmp/sin-config-home/.local/share") /
                         RuntimeConfigFile::kApplicationDirectoryName,
                 "relative XDG_DATA_HOME must fall back to HOME")) return 1;
#elif defined(__APPLE__)
    unsetenv("RECOMPLAUNCHER_DATA");
    if (setenv("HOME", "/tmp/sin-config-home", 1) != 0) return 1;
    if (!RuntimeConfigFile::PortableRootDirectory() &&
        !Require(RuntimeConfigFile::ResolveConfigPath() ==
                     std::filesystem::path("/tmp/sin-config-home/Library/Application Support") /
                         RuntimeConfigFile::kApplicationDirectoryName / "Config.toml",
                 "macOS config must be under Application Support")) return 1;
#endif
    {
        const char* argv[] = {"WiiCompiled.exe", "--headless", nullptr};
        RecompLauncher::Arguments arguments;
        std::string error;
        if (!Require(RecompLauncher::ParseArguments(2, const_cast<char**>(argv), arguments, error) &&
                         arguments.headless && !arguments.prepare && !arguments.gamePath,
                     "headless arguments were not parsed")) return 1;
    }
    {
        const char* argv[] = {"WiiCompiled.exe", "--prepare", "--data=C:/Launcher Data",
                              "--game", "D:/Dumps/R2VE01.iso", nullptr};
        RecompLauncher::Arguments arguments;
        std::string error;
        if (!Require(RecompLauncher::ParseArguments(5, const_cast<char**>(argv), arguments, error) &&
                         arguments.headless && arguments.prepare &&
                         arguments.dataDirectory == std::filesystem::path("C:/Launcher Data") &&
                         arguments.gamePath == std::filesystem::path("D:/Dumps/R2VE01.iso"),
                     "launcher setup arguments were not parsed")) return 1;
    }
    {
        const char* argv[] = {"WiiCompiled.exe", "--prepare", "--data", "C:/Data", nullptr};
        RecompLauncher::Arguments arguments;
        std::string error;
        if (!Require(!RecompLauncher::ParseArguments(4, const_cast<char**>(argv), arguments, error) &&
                         error.find("--game") != std::string::npos,
                     "prepare without a game was accepted")) return 1;
    }
    {
        const char* argv[] = {"WiiCompiled.exe", "--unknown", nullptr};
        RecompLauncher::Arguments arguments;
        std::string error;
        if (!Require(!RecompLauncher::ParseArguments(2, const_cast<char**>(argv), arguments, error) &&
                         error.find("--unknown") != std::string::npos,
                     "unknown launcher arguments were accepted")) return 1;
    }
    {
        const char* argv[] = {"WiiCompiled.exe", "--data=first", "--data", "second", nullptr};
        RecompLauncher::Arguments arguments;
        std::string error;
        if (!Require(!RecompLauncher::ParseArguments(4, const_cast<char**>(argv), arguments, error) &&
                         error.find("only be specified once") != std::string::npos,
                     "duplicate data paths were accepted")) return 1;
    }
    {
        for (bool enabled : {false, true}) {
            std::istringstream input(enabled ? "[video]\ngenerate_mips = true\n" : "[video]\ngenerate_mips = false\n");
            const auto config = RuntimeConfigFile::ParseConfig(input, "mipmaps.toml");
            if (!Require(config.generateMips == std::optional<bool>{enabled}, "replacement mipmap setting mismatch")) return 1;
            const std::string value = enabled ? "true" : "false";
            std::istringstream settings("[video]\nenhancements_enabled = " + value + "\nenhanced_bloom = " + value +
                                        "\nper_pixel_lighting = " + value + "\nstage_atmosphere = " + value +
                                        "\nstrong_bounce = " + value + "\nretain_culled_gi = " + value + "\nray_traced_gi = " + value + "\nglobal_illumination = " + value + "\nambient_occlusion = " + value + "\nsharpening_enabled = " + value +
                                        "\nprojected_caustics = " + value + "\nsurface_highlights = " + value + "\nprobe_materials = " + value + "\ncombat_light = " + value + "\ncontact_shadow = " + value);
            const auto parsed = RuntimeConfigFile::ParseConfig(settings, "enhancements.toml");
            if (!Require(parsed.enhancementsEnabled == enabled && parsed.enhancedBloom == enabled &&
                         parsed.pixelLighting == enabled && parsed.stageAtmosphere == enabled &&
                         parsed.strongBounce == enabled && parsed.retainCulledGi == enabled && parsed.rayTracedGi == enabled && parsed.globalIllumination == enabled && parsed.ambientOcclusion == enabled && parsed.sharpeningEnabled == enabled &&
                         parsed.projectedCaustics == enabled && parsed.surfaceHighlights == enabled &&
                         parsed.probeMaterials == enabled && parsed.combatLight == enabled && parsed.contactShadow == enabled,
                         "enhancements switch mismatch")) return 1;
        }
    }
    {
        for (unsigned mask = 0; mask < 8; ++mask) {
            const auto flag = [&](unsigned bit) { return (mask & bit) ? "true" : "false"; };
            std::ostringstream text;
            text << "[video]\nprobe_materials = " << flag(1) << "\ncombat_light = " << flag(2)
                 << "\ncontact_shadow = " << flag(4);
            std::istringstream input(text.str());
            const auto parsed = RuntimeConfigFile::ParseConfig(input, "material-settings.toml");
            if (!Require(parsed.probeMaterials == bool(mask & 1) && parsed.combatLight == bool(mask & 2) &&
                         parsed.contactShadow == bool(mask & 4), "material toggles must be independent")) return 1;
        }
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "enabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{true},
                     "valid MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = false\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "disabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{false},
                     "false MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = \"yes\"\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "invalid.toml");
        if (!Require(!config.metalFxSpatialUpscaling, "invalid MetalFX setting was accepted")) return 1;
    }
    {
        std::istringstream input("[video]\nwidescreen = false\nforce_16_9 = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "legacy.toml");
        if (!Require(config.forceAspect == std::optional<bool>{true} &&
                         config.widescreen == std::optional<bool>{true} && config.legacyForceAspect,
                     "legacy force_16_9 did not force a 16:9 game")) return 1;
    }
    {
        std::istringstream input("[video]\nwidescreen = false\nforce_aspect = true\nforce_16_9 = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "migrated.toml");
        if (!Require(config.forceAspect == std::optional<bool>{true} &&
                         config.widescreen == std::optional<bool>{false} && !config.legacyForceAspect,
                     "legacy force_16_9 overrode force_aspect")) return 1;
    }
    {
        std::istringstream input("[controller]\nmotion_trick = true\nmotion_trick_sensitivity = 5\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "motion.toml");
        if (!Require(config.motionTrick == std::optional<bool>{true} &&
                         config.motionTrickSensitivity == std::optional<int32_t>{5},
                     "shake to trick settings were not loaded")) return 1;
    }
    {
        std::istringstream input("[controller]\nmotion_trick_sensitivity = 9\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "motion-range.toml");
        if (!Require(!config.motionTrick && !config.motionTrickSensitivity,
                     "out of range shake to trick sensitivity was accepted")) return 1;
    }
    {
        std::istringstream input("[controller]\nsteam_input_override = false\nsteam_input_app_id = 1234\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "steam.toml");
        if (!Require(config.steamInputOverride == std::optional<bool>{false} &&
                         config.steamInputAppId == std::optional<uint32_t>{1234},
                     "Steam Input settings were not loaded")) return 1;
    }
    {
        std::istringstream input("[controller]\nsteam_input_app_id = 0\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "steam-zero.toml");
        if (!Require(!config.steamInputAppId, "zero Steam Input app id was accepted")) return 1;
    }
    if (!SetsFps("[video]\nshow_fps = false\n", "bare key was not replaced")) return 1;
    if (!SetsFps("[video]\n\"show_fps\" = false\n", "basic-quoted key was not replaced")) return 1;
    if (!SetsFps("[video]\n'show_fps'=false\n", "literal-quoted key was not replaced")) return 1;
    if (!SetsFps("[video]\n\"show\\u005Ffps\" = false\n", "escaped key was not replaced")) return 1;
    if (!SetsFps("[\"video\"]\n  show_fps = false\n", "quoted table header was not matched")) return 1;
    if (!SetsFps("[video]\nshow_fps = [\n  1,\n]\n", "multi-line value was not replaced")) return 1;
    if (!SetsFps("[video]\n", "missing key was not created")) return 1;
    if (!SetsFps("", "missing table was not created")) return 1;
    {
        std::string text;
        const auto document = Update("# top\nfoo = 1\n\n[video]\nzzz = 2 # keep\nshow_fps = false # note\naaa = 3\n\n"
                                     "[audio]\nshow_fps = 1\n",
                                     "video", "show_fps", "true", &text);
        if (!Require(document && document->at("foo").as_integer() == 1 &&
                         document->at("audio").at("show_fps").as_integer() == 1 &&
                         document->at("video").at("zzz").as_integer() == 2 &&
                         document->at("video").at("aaa").as_integer() == 3 &&
                         document->at("video").at("show_fps").as_boolean(),
                     "unrelated settings were not preserved")) return 1;
        const size_t zzz = text.find("zzz"), fps = text.find("show_fps"), aaa = text.find("aaa");
        if (!Require(text.find("# top") != std::string::npos && text.find("# keep") != std::string::npos &&
                         text.find("# note") != std::string::npos && zzz < fps && fps < aaa,
                     "comments or key order were not preserved")) return 1;
    }
    if (!Require(!RuntimeConfigFile::UpdateSettingText("[video]\nshow_fps = 1\nshow_fps = 2\n", "video", "show_fps", "true"),
                 "a document with a duplicate key was rewritten")) return 1;
    if (!Require(!RuntimeConfigFile::UpdateSettingText("[video\nshow_fps = 1\n", "video", "show_fps", "true"),
                 "an unparseable document was rewritten")) return 1;
    if (!SetsOffset("[controller]\nrumble = true\n\n[controller.wii_accel_offsets]\nx = 0.1\ny = 2\n",
                    "key in an existing nested table was not replaced")) return 1;
    if (!SetsOffset("[controller.wii_accel_offsets]\ny = 2\n", "key was not added to a nested table")) return 1;
    if (!SetsOffset("[controller]\nrumble = true\n", "missing nested table was not created")) return 1;
    if (!SetsOffset("[controller.\"wii_accel_offsets\"]\nx = 0.1\n", "quoted nested header was not matched")) return 1;
    if (!SetsOffset("[controller]\nwii_accel_offsets.y = 2\n", "dotted-key table was not extended")) return 1;
    if (!SetsOffset("controller.wii_accel_offsets.y = 2\n", "root-level dotted table was not extended")) return 1;
    if (!Require(!RuntimeConfigFile::UpdateSettingText("controller = \"custom\"\n", "controller.wii_accel_offsets",
                                                       "x", "0.5"),
                 "a scalar was replaced by a table")) return 1;
    {
        const auto document = Update("[video.advanced]\nfoo = 1\n", "video", "show_fps", "true");
        if (!Require(document && document->at("video").at("show_fps").as_boolean() &&
                         document->at("video").at("advanced").at("foo").as_integer() == 1,
                     "key was not added to an implicit table")) return 1;
    }
    {
        std::istringstream input("[controller]\na_1 = \"south\"\nright_4 = \"east\"\nl_2 = \"west\"\n"
                                 "a_0 = \"north\"\na_5 = \"north\"\nnotabutton_1 = \"south\"\n"
                                 "a_x = \"south\"\na_3 = 1\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "ports.toml");
        if (!Require(config.controllerPortButtons[0][0] == std::optional<std::string>{"south"} &&
                         config.controllerPortButtons[3][11] == std::optional<std::string>{"east"} &&
                         config.controllerPortButtons[1][6] == std::optional<std::string>{"west"},
                     "valid per-port button overrides were not loaded")) return 1;
        size_t setCount = 0;
        for (const auto& portButtons : config.controllerPortButtons)
            for (const auto& button : portButtons)
                if (button) ++setCount;
        if (!Require(setCount == 3, "out-of-range, non-string, or unknown per-port keys were accepted")) return 1;
    }
    {
        std::istringstream input("[controller]\na = \"west\"\na_1 = \"south\"\nb_2 = \"east\"\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "ports-fallback.toml");
        if (!Require(config.controllerButtons[0] == std::optional<std::string>{"west"} &&
                         config.controllerPortButtons[0][0] == std::optional<std::string>{"south"} &&
                         config.controllerPortButtons[1][1] == std::optional<std::string>{"east"},
                     "shared and per-port button bindings were not kept apart")) return 1;
    }
    if (!Require(RuntimeConfigFile::PortControllerButtonKey(0, 0) == "a_1" &&
                     RuntimeConfigFile::PortControllerButtonKey(3, 11) == "right_4" &&
                     RuntimeConfigFile::PortControllerButtonKey(4, 0).empty() &&
                     RuntimeConfigFile::PortControllerButtonKey(0, 12).empty(),
                 "per-port button key layout is wrong")) return 1;
    return 0;
}
