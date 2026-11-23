#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

inline std::vector<uint8_t> ReadModuleFile(const std::filesystem::path& path,
                                           size_t limit = std::numeric_limits<uint32_t>::max()) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = input.tellg();
    if (size <= 0 || static_cast<uintmax_t>(size) > limit) {
        throw std::runtime_error("Missing, empty or oversized module file: " + path.filename().string());
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!input.seekg(0).read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Incomplete module read: " + path.filename().string());
    }
    return bytes;
}
