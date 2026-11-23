// Included in the Windows importer. Private input stays in memory.
struct KeyPayload {
    std::vector<char> bytes;
    ~KeyPayload() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
};

constexpr size_t kMaxKeyPayload = 16u << 20;

template<class Function>
Function ArchiveSymbol(HMODULE library, const char* name) {
    auto symbol = GetProcAddress(library, name);
    if (!symbol) throw ImportError("This Windows version cannot read 7z files. Extract the key file and select it directly.");
    return reinterpret_cast<Function>(symbol);
}

void ReadArchivedKey(const fs::path& path, KeyPayload& payload) {
    // Use the libarchive reader shipped with Windows tar. Load only from System32.
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> library(
        LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary);
    if (!library) throw ImportError("The Windows archive reader is unavailable. Extract the key and select it directly.");
    auto create = ArchiveSymbol<void*(*)()>(library.get(), "archive_read_new");
    auto release = ArchiveSymbol<int(*)(void*)>(library.get(), "archive_read_free");
    auto enable = ArchiveSymbol<int(*)(void*)>(library.get(), "archive_read_support_format_7zip");
    auto open = ArchiveSymbol<int(*)(void*, const wchar_t*, size_t)>(library.get(), "archive_read_open_filename_w");
    auto next = ArchiveSymbol<int(*)(void*, void**)>(library.get(), "archive_read_next_header");
    auto name = ArchiveSymbol<const wchar_t*(*)(void*)>(library.get(), "archive_entry_pathname_w");
    auto type = ArchiveSymbol<unsigned short(*)(void*)>(library.get(), "archive_entry_filetype");
    auto size = ArchiveSymbol<int64_t(*)(void*)>(library.get(), "archive_entry_size");
    auto read = ArchiveSymbol<intptr_t(*)(void*, void*, size_t)>(library.get(), "archive_read_data");
    std::unique_ptr<void, decltype(release)> archive(create(), release);
    const char* damaged = "Cannot read this 7z archive. Select an intact, unencrypted archive or the extracted key file.";
    if (!archive || enable(archive.get()) != 0 || open(archive.get(), path.c_str(), 65536) != 0) throw ImportError(damaged);
    void* entry = nullptr;
    int status;
    unsigned entries = 0;
    while ((status = next(archive.get(), &entry)) == 0) {
        if (++entries > 4096) throw ImportError("Too many files in the key archive.");
        const wchar_t* member = name(entry);
        if (type(entry) != 0100000 || !member) continue;
        const auto filename = fs::path(member).filename();
        const auto ext = ExtensionOf(filename);
        if (ext != ".bin" && ext != ".key" && ext != ".txt" && ext != ".hex" && filename != L"common-key") continue;
        if (!payload.bytes.empty()) throw ImportError("This archive has multiple possible key files. Extract and select the intended one.");
        const auto length = size(entry);
        if (length < 1 || length > kMaxKeyPayload) throw ImportError("Invalid key file size inside the archive.");
        payload.bytes.resize(static_cast<size_t>(length));
        for (size_t at = 0; at < payload.bytes.size();) {
            const auto n = read(archive.get(), payload.bytes.data() + at, payload.bytes.size() - at);
            if (n <= 0) throw ImportError(damaged);
            at += static_cast<size_t>(n);
        }
        char extra{};
        if (read(archive.get(), &extra, 1) != 0) throw ImportError(damaged);
    }
    if (status != 1) throw ImportError(damaged); // ARCHIVE_EOF
    if (payload.bytes.empty()) throw ImportError("This archive contains no supported Wii key file.");
}

bool ReadDiscKey(const fs::path& path, uint8_t commonIndex, uint8_t (&key)[16]) {
    KeyPayload input;
    if (ExtensionOf(path) == ".7z") {
        ReadArchivedKey(path, input);
    } else {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (!file || size <= 0 || size > kMaxKeyPayload) throw ImportError("Cannot read the selected key file or its size is invalid.");
        input.bytes.resize(static_cast<size_t>(size));
        file.seekg(0);
        if (!file.read(input.bytes.data(), size)) throw ImportError("Could not read the complete Wii key file.");
    }
    std::string_view data(input.bytes.data(), input.bytes.size());
    if (data.size() == 16) {
        std::memcpy(key, data.data(), 16);
        return false;
    }
    if (data.size() == 1024 && data.substr(0, 9) == "BackupMii") {
        if (commonIndex != 0) throw ImportError("The disc requires a different common key from the standard BootMii key.");
        std::memcpy(key, data.data() + 0x114, 16);
        return false;
    }
    if (data.size() == 0x108000 && data.find('\0') != data.npos)
        throw ImportError("This file has the size of a Wii NAND boot-area dump. Select your Wii common key or BootMii keys.bin.");
    auto trim = [](std::string_view v) {
        const auto first = v.find_first_not_of(" \t\r\n");
        return first == v.npos ? std::string_view{} : v.substr(first, v.find_last_not_of(" \t\r\n") - first + 1);
    };
    if (data.substr(0, 3) == "\xEF\xBB\xBF") data.remove_prefix(3);
    std::string_view digits = trim(data);
    bool titleKey = false;
    if (data.find('#') != data.npos) {
        bool found = false;
        while (!data.empty()) {
            const auto end = data.find('\n');
            const auto line = data.substr(0, end);
            data.remove_prefix(end == data.npos ? data.size() : end + 1);
            const auto comment = line.find('#');
            if (comment == line.npos) continue;
            auto label = trim(line.substr(comment + 1));
            if (label.size() != 14 && label.size() != 16) continue;
            auto lower = Lower(std::string(label));
            if (lower != "wii common key" && lower != "r2ve01 title key") continue;
            if (found) throw ImportError("Multiple matching keys in this file. Select one key at a time.");
            titleKey = lower == "r2ve01 title key";
            digits = trim(line.substr(0, comment));
            found = true;
        }
        if (!found) throw ImportError("This list has no entry labeled Wii common key or R2VE01 title key.");
    }
    const char* formatError = "The Wii key must contain 16 binary bytes or 32 hexadecimal digits.";
    if (digits.size() != 32) throw ImportError(formatError);
    auto nibble = [&](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw ImportError(formatError);
    };
    for (size_t i = 0; i < 16; ++i)
        key[i] = static_cast<uint8_t>((nibble(digits[2 * i]) << 4) | nibble(digits[2 * i + 1]));
    return titleKey;
}
