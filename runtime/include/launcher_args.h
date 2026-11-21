#pragma once

#include "runtime_config.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RecompLauncher {

struct Arguments {
    std::filesystem::path keyFile;
    std::filesystem::path dolphinTool;
    bool headless = false;
    bool prepare = false;
    bool help = false;
    std::optional<std::filesystem::path> dataDirectory;
    std::optional<std::filesystem::path> gamePath;
};

inline bool IsLongOption(std::string_view argument, std::string_view name) {
    return argument == name ||
           (argument.size() > name.size() && argument.substr(0, name.size()) == name &&
            argument[name.size()] == '=');
}

inline std::optional<std::string> LongOptionValue(std::string_view argument,
                                                   std::string_view name) {
    if (argument == name) {
        return std::nullopt;
    }
    if (argument.size() > name.size() + 1 && argument.substr(0, name.size()) == name &&
        argument[name.size()] == '=') {
        return std::string(argument.substr(name.size() + 1));
    }
    return std::nullopt;
}

inline bool ParseArguments(int argc, char** argv, Arguments& parsed, std::string& error) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--headless") {
            parsed.headless = true;
            continue;
        }
        if (argument == "--prepare") {
            parsed.prepare = true;
            parsed.headless = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            parsed.help = true;
            continue;
        }
        if (IsLongOption(argument, "--keys") || IsLongOption(argument, "--dolphin-tool")) {
            const auto option = IsLongOption(argument, "--keys") ? "--keys" : "--dolphin-tool";
            auto value = LongOptionValue(argument, option);
            if (!value && argument == option && index + 1 < argc &&
                !std::string_view(argv[index + 1]).starts_with("--")) value = argv[++index];
            auto& path = IsLongOption(argument, "--keys") ? parsed.keyFile : parsed.dolphinTool;
            if (!value || value->empty() || !path.empty()) {
                error = std::string(option) + " requires one non-empty path";
                return false;
            }
            path = RuntimeConfigFile::PathFromUtf8(*value);
            continue;
        }
        if (IsLongOption(argument, "--data") || IsLongOption(argument, "--game")) {
            const bool isData = IsLongOption(argument, "--data");
            const std::string_view option = isData ? "--data" : "--game";
            auto value = LongOptionValue(argument, option);
            if (!value) {
                if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                    error = std::string(option) + " requires a path";
                    return false;
                }
                value = argv[index];
            }
            if (value->empty()) {
                error = std::string(option) + " requires a non-empty path";
                return false;
            }
            const auto path = RuntimeConfigFile::PathFromUtf8(*value);
            if (isData) {
                if (parsed.dataDirectory) {
                    error = "--data may only be specified once";
                    return false;
                }
                parsed.dataDirectory = path;
            } else {
                if (parsed.gamePath) {
                    error = "--game may only be specified once";
                    return false;
                }
                parsed.gamePath = path;
            }
            continue;
        }
        error = "Unknown command-line option '" + std::string(argument) + "'";
        return false;
    }

    if (parsed.prepare && !parsed.gamePath) {
        error = "--prepare requires --game=<disc image or extracted folder>";
        return false;
    }
    return true;
}

inline const char* Usage() {
    return "Usage: WiiCompiled.exe [--data <path>] [--game <path>] [--headless]\n"
           "       WiiCompiled.exe --prepare --data <path> --game <path> [--keys <file>] [--dolphin-tool <exe>]\n";
}

}  // namespace RecompLauncher
