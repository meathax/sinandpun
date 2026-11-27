#pragma once


#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace RuntimeSysConf {

enum class ItemType : uint8_t {
    BigArray = 1,
    SmallArray = 2,
    Byte = 3,
    Short = 4,
    Long = 5,
    LongLong = 6,
    Bool = 7,
};

struct Item {
    ItemType type;
    std::string_view name;
    std::vector<uint8_t> data;
};

inline void Append16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value));
}

inline void Append32(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value >> 24));
    output.push_back(static_cast<uint8_t>(value >> 16));
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value));
}

inline std::vector<uint8_t> Bytes32(uint32_t value) {
    return {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
}

// Build a valid 16 KiB retail SYSCONF. The layout and default item set follow
// Dolphin's first-boot generation for a USA console (IPL.LNG English, IPL.SADR
// country United States). The trailing item-offset lookup table is zero;
// Nintendo's SC implementation falls back to a name lookup in that case.
inline std::vector<uint8_t> Build() {
    constexpr uint8_t language = 1u;  // English
    constexpr uint8_t country = 49u;  // IPL.SADR country: United States

    std::vector<Item> items;
    const auto add = [&](ItemType type, std::string_view name, std::vector<uint8_t> data) {
        items.push_back({type, name, std::move(data)});
    };

    // BT.DINF: four empty Wiimote records, as in Dolphin's first-boot NAND.
    std::vector<uint8_t> btDinf(0x460);
    btDinf[0] = 4;
    for (size_t index = 0; index < 4; ++index) {
        const uint8_t address[6] = {static_cast<uint8_t>(index), 0x00, 0x79, 0x19, 0x02, 0x11};
        std::copy(std::begin(address), std::end(address), btDinf.begin() + 1 + 70 * index);
        constexpr char kRemoteName[] = "Nintendo RVL-CNT-01";
        std::copy(kRemoteName, kRemoteName + 19, btDinf.begin() + 7 + 70 * index);
    }
    add(ItemType::BigArray, "BT.DINF", std::move(btDinf));
    add(ItemType::Long, "BT.SENS", Bytes32(3));

    // UTF-16BE console nickname. Keep it ASCII so both regions can display it.
    std::vector<uint8_t> nickname(0x15);
    constexpr char kNickname[] = "recomp";
    for (size_t index = 0; index < sizeof(kNickname) - 1; ++index) {
        nickname[index * 2 + 1] = static_cast<uint8_t>(kNickname[index]);
    }
    add(ItemType::SmallArray, "IPL.NIK", std::move(nickname));
    add(ItemType::Byte, "IPL.AR", {1});
    add(ItemType::Byte, "BT.BAR", {1});
    add(ItemType::Byte, "IPL.SSV", {0});
    add(ItemType::Byte, "IPL.LNG", {language});

    std::vector<uint8_t> simpleAddress(0x1007);
    simpleAddress[0] = country;
    add(ItemType::BigArray, "IPL.SADR", std::move(simpleAddress));

    add(ItemType::Long, "IPL.CB", {0x0f, 0x11, 0x14, 0xa6});
    add(ItemType::Byte, "BT.SPKV", {0x58});
    std::vector<uint8_t> parental(0x49);
    parental[1] = 0x04;
    parental[2] = 0x14;
    add(ItemType::SmallArray, "IPL.PC", std::move(parental));
    add(ItemType::Long, "NET.CTPC", Bytes32(0));
    add(ItemType::Bool, "WWW.RST", {0});
    add(ItemType::BigArray, "BT.CDIF", std::vector<uint8_t>(0x204));
    add(ItemType::Long, "IPL.INC", Bytes32(8));
    add(ItemType::Long, "IPL.FRC", Bytes32(0x28));
    add(ItemType::Bool, "IPL.CD", {1});
    add(ItemType::Bool, "IPL.CD2", {1});
    add(ItemType::Byte, "IPL.UPT", {2});
    add(ItemType::Byte, "IPL.PGS", {0});
    add(ItemType::Byte, "IPL.E60", {1});
    add(ItemType::Byte, "IPL.DH", {0});
    // WiiConnect24 is disabled: this is an offline retail console identity.
    add(ItemType::Long, "NET.WCFG", Bytes32(0));
    add(ItemType::SmallArray, "IPL.IDL", {1});
    add(ItemType::Bool, "IPL.EULA", {1});
    add(ItemType::Byte, "BT.MOT", {1});
    add(ItemType::Bool, "MPLS.MOVIE", {1});

    constexpr size_t kFileSize = 0x4000;
    std::vector<uint8_t> output;
    output.reserve(kFileSize);
    output.insert(output.end(), {'S', 'C', 'v', '0'});
    // Nintendo stores the final invalid/end offset as another table entry; the
    // count written here therefore equals the number of real items.
    Append16(output, static_cast<uint16_t>(items.size()));

    const uint16_t firstItemOffset = static_cast<uint16_t>(6 + (items.size() + 1) * 2);
    std::vector<uint16_t> offsets;
    offsets.reserve(items.size());
    uint16_t cursor = firstItemOffset;
    for (const Item& item : items) {
        offsets.push_back(cursor);
        uint32_t size = 1 + static_cast<uint32_t>(item.name.size()) + static_cast<uint32_t>(item.data.size());
        if (item.type == ItemType::BigArray) size += 3;       // u16 length + trailing NUL
        if (item.type == ItemType::SmallArray) size += 2;     // u8 length + trailing NUL
        cursor = static_cast<uint16_t>(cursor + size);
    }
    for (const uint16_t offset : offsets) Append16(output, offset);
    Append16(output, cursor);

    for (const Item& item : items) {
        const uint8_t description = static_cast<uint8_t>((static_cast<uint8_t>(item.type) << 5) |
                                                         (item.name.size() - 1));
        output.push_back(description);
        output.insert(output.end(), item.name.begin(), item.name.end());
        if (item.type == ItemType::BigArray) {
            Append16(output, static_cast<uint16_t>(item.data.size()));
            output.insert(output.end(), item.data.begin(), item.data.end());
            output.push_back(0);
        } else if (item.type == ItemType::SmallArray) {
            output.push_back(static_cast<uint8_t>(item.data.size()));
            output.insert(output.end(), item.data.begin(), item.data.end());
            output.push_back(0);
        } else {
            output.insert(output.end(), item.data.begin(), item.data.end());
        }
    }

    if (output.size() > kFileSize - 4) return {};
    output.resize(kFileSize - 4, 0);
    output.insert(output.end(), {'S', 'C', 'e', 'd'});
    return output;
}

inline bool Ensure(const std::filesystem::path& nandRoot) {
    const auto path = nandRoot / "shared2" / "sys" / "SYSCONF";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return !ec;
    }
    const auto image = Build();
    if (image.empty()) return false;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return static_cast<bool>(output);
}

} // namespace RuntimeSysConf
