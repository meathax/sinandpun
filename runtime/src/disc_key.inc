// Included in the Windows importer. Finds the player's Wii key by content and
// proves it against the disc before use. Key bytes stay in memory, are wiped
// after use and are never logged or written.
//
// Accepted sources (any file name or extension): raw 16-byte keys, BootMii
// keys.bin, nand.bin with BootMii keys appended, Wii or Wii U otp.bin, text
// lists (hex strings or byte lists, Dolphin/Cemu/any labels), arbitrary small
// binary dumps (scanned at every offset), 7z/zip/rar archives of those, and
// RVZ/WIA images of the same disc (their partition entries store the title key).

struct KeyPayload {
    std::vector<char> bytes;
    ~KeyPayload() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
};

constexpr size_t kMaxKeyPayload = 16u << 20;

struct KeyRequired {
    std::string message;
};

// ---- single-block AES for fast candidate checks -------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define SIN_AES_TARGET
#else
#include <cpuid.h>
#define SIN_AES_TARGET __attribute__((target("aes,sse2")))
#endif

bool CpuHasAes() {
#if defined(_MSC_VER) && !defined(__clang__)
    int info[4] = {};
    __cpuid(info, 1);
    return (info[2] & (1 << 25)) != 0;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    return __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 25)) != 0;
#endif
}

SIN_AES_TARGET inline __m128i AesExpandStep(__m128i key, __m128i assist) {
    assist = _mm_shuffle_epi32(assist, 0xff);
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    return _mm_xor_si128(key, assist);
}

SIN_AES_TARGET void AesNiCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t blocks) {
    __m128i rk[11];
    rk[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key));
    rk[1] = AesExpandStep(rk[0], _mm_aeskeygenassist_si128(rk[0], 0x01));
    rk[2] = AesExpandStep(rk[1], _mm_aeskeygenassist_si128(rk[1], 0x02));
    rk[3] = AesExpandStep(rk[2], _mm_aeskeygenassist_si128(rk[2], 0x04));
    rk[4] = AesExpandStep(rk[3], _mm_aeskeygenassist_si128(rk[3], 0x08));
    rk[5] = AesExpandStep(rk[4], _mm_aeskeygenassist_si128(rk[4], 0x10));
    rk[6] = AesExpandStep(rk[5], _mm_aeskeygenassist_si128(rk[5], 0x20));
    rk[7] = AesExpandStep(rk[6], _mm_aeskeygenassist_si128(rk[6], 0x40));
    rk[8] = AesExpandStep(rk[7], _mm_aeskeygenassist_si128(rk[7], 0x80));
    rk[9] = AesExpandStep(rk[8], _mm_aeskeygenassist_si128(rk[8], 0x1b));
    rk[10] = AesExpandStep(rk[9], _mm_aeskeygenassist_si128(rk[9], 0x36));
    __m128i dk[11];
    dk[0] = rk[10];
    for (int i = 1; i < 10; ++i) dk[i] = _mm_aesimc_si128(rk[10 - i]);
    dk[10] = rk[0];
    __m128i prev = _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv));
    for (size_t b = 0; b < blocks; ++b) {
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + b * 16));
        __m128i x = _mm_xor_si128(c, dk[0]);
        for (int r = 1; r < 10; ++r) x = _mm_aesdec_si128(x, dk[r]);
        x = _mm_xor_si128(_mm_aesdeclast_si128(x, dk[10]), prev);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + b * 16), x);
        prev = c;
    }
    SecureZeroMemory(rk, sizeof rk);
    SecureZeroMemory(dk, sizeof dk);
}
#else
bool CpuHasAes() { return false; }
void AesNiCbcDecrypt(const uint8_t*, const uint8_t*, const uint8_t*, uint8_t*, size_t) {}
#endif

// Tests candidate keys against the disc's own ticket and first data cluster.
class KeyMatcher {
public:
    KeyMatcher(const uint8_t* ticket, const uint8_t* cluster0, const uint8_t gameId[6]) : m_aesNi(CpuHasAes()) {
        std::memcpy(m_encryptedTitle, ticket + 0x1BF, 16);
        std::memcpy(m_titleIv, ticket + 0x1DC, 8);
        std::memcpy(m_dataIv, cluster0 + 0x3D0, 16);
        std::memcpy(m_dataHead, cluster0 + kClusterHashBytes, sizeof m_dataHead);
        std::memcpy(m_gameId, gameId, 6);
    }
    ~KeyMatcher() {
        SecureZeroMemory(m_found, sizeof m_found);
        SecureZeroMemory(m_last, sizeof m_last);
    }
    KeyMatcher(const KeyMatcher&) = delete;
    KeyMatcher& operator=(const KeyMatcher&) = delete;

    // Accepts the candidate as a Wii common key (decrypting the ticket's title
    // key) or directly as this disc's title key.
    bool Try(const uint8_t key[16]) {
        if (m_matched) return true;
        if (m_haveLast && std::memcmp(key, m_last, 16) == 0) return false;
        std::memcpy(m_last, key, 16);
        m_haveLast = true;
        ++m_tried;
        uint8_t title[16];
        Decrypt(key, m_titleIv, m_encryptedTitle, title, 1);
        const bool asCommon = Unlocks(title);
        if (asCommon || Unlocks(key)) {
            std::memcpy(m_found, asCommon ? title : key, 16);
            m_matched = true;
        }
        SecureZeroMemory(title, sizeof title);
        return m_matched;
    }
    bool Matched() const { return m_matched; }
    const uint8_t* TitleKey() const { return m_found; }
    uint64_t Tried() const { return m_tried; }

private:
    void Decrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t blocks) const {
        if (m_aesNi) AesNiCbcDecrypt(key, iv, in, out, blocks);
        else AesCbcDecryptor(key).Decrypt(iv, in, out, blocks * 16);
    }
    bool Unlocks(const uint8_t title[16]) const {
        uint8_t head[32];
        Decrypt(title, m_dataIv, m_dataHead, head, 2);
        const bool ok = std::memcmp(head, m_gameId, 6) == 0 && Be32(head + 0x18) == kWiiMagic;
        SecureZeroMemory(head, sizeof head);
        return ok;
    }

    bool m_aesNi;
    uint8_t m_encryptedTitle[16], m_titleIv[16]{}, m_dataIv[16], m_dataHead[32], m_gameId[6];
    uint8_t m_found[16]{}, m_last[16]{};
    bool m_matched = false, m_haveLast = false;
    uint64_t m_tried = 0;
};

// ---- candidate extraction ----------------------------------------------------------------------------------

struct KeyReport {
    std::vector<std::string> notes;  // one human-readable line per file that did not help
    unsigned files = 0;
    bool wiiUList = false;
};

std::string Thousands(uint64_t n) {
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool LooksLikeText(std::string_view data) {
    if (data.empty() || data.find('\0') != data.npos) return false;
    size_t printable = 0;
    for (unsigned char c : data) printable += (c >= 0x20 || c == '\n' || c == '\r' || c == '\t') ? 1 : 0;
    return printable * 100 >= data.size() * 97;
}

// Every 32-digit hex run, and every list of sixteen 2-digit bytes ("0xEB, 0xE4, ..."
// or "eb e4 ..."), is a candidate. Labels do not matter; the disc decides.
void ScanTextKeys(std::string_view text, KeyMatcher& matcher) {
    uint8_t key[16];
    // Contiguous runs (a 64-digit run is checked as both halves and every 16-byte window).
    for (size_t i = 0; i < text.size() && !matcher.Matched();) {
        if (HexNibble(text[i]) < 0) { ++i; continue; }
        size_t end = i;
        while (end < text.size() && HexNibble(text[end]) >= 0) ++end;
        const size_t run = end - i;
        if (run >= 32) {
            for (size_t at = i; at + 32 <= end && !matcher.Matched(); at += 2) {
                for (size_t b = 0; b < 16; ++b)
                    key[b] = static_cast<uint8_t>((HexNibble(text[at + 2 * b]) << 4) | HexNibble(text[at + 2 * b + 1]));
                matcher.Try(key);
            }
        }
        i = end;
    }
    // Byte lists: sixteen consecutive 1-2 digit hex tokens ("0xEB, 0xE4, ..." or "eb e4 ..."), across lines too.
    std::vector<uint8_t> tokens;
    auto flush = [&] {
        for (size_t at = 0; at + 16 <= tokens.size() && !matcher.Matched(); ++at) matcher.Try(tokens.data() + at);
        SecureZeroMemory(tokens.data(), tokens.size());
        tokens.clear();
    };
    auto separator = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';' || c == ':' ||
               c == '{' || c == '}' || c == '(' || c == ')' || c == '[' || c == ']' || c == '-';
    };
    for (size_t i = 0; i < text.size() && !matcher.Matched();) {
        if (separator(text[i])) { ++i; continue; }
        size_t end = i;
        while (end < text.size() && !separator(text[end])) ++end;
        std::string_view token = text.substr(i, end - i);
        if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) token.remove_prefix(2);
        const bool byte = (token.size() == 1 || token.size() == 2) && HexNibble(token[0]) >= 0 && HexNibble(token.back()) >= 0;
        if (byte) {
            tokens.push_back(static_cast<uint8_t>(token.size() == 2 ? (HexNibble(token[0]) << 4) | HexNibble(token[1]) : HexNibble(token[0])));
        } else if (!tokens.empty()) {
            if (tokens.size() >= 16) flush();
            SecureZeroMemory(tokens.data(), tokens.size());
            tokens.clear();
        }
        i = end;
    }
    if (tokens.size() >= 16) flush();
    SecureZeroMemory(key, sizeof key);
}

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

void ScanBinaryKeys(std::string_view data, KeyMatcher& matcher);

// Base64 (standard or URL-safe) runs, e.g. a key pasted from a hex editor or stored in JSON/INI.
void ScanBase64Keys(std::string_view text, KeyMatcher& matcher) {
    for (size_t i = 0; i < text.size() && !matcher.Matched();) {
        if (Base64Value(text[i]) < 0) { ++i; continue; }
        size_t end = i;
        while (end < text.size() && Base64Value(text[end]) >= 0) ++end;
        size_t used = end;
        while (used < text.size() && used < end + 2 && text[used] == '=') ++used;
        const size_t run = end - i;
        if (run >= 22 && run <= (4u << 20)) {
            std::vector<uint8_t> decoded;
            decoded.reserve(run * 3 / 4 + 3);
            uint32_t accumulator = 0;
            int bits = 0;
            for (size_t j = i; j < end; ++j) {
                accumulator = (accumulator << 6) | static_cast<uint32_t>(Base64Value(text[j]));
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    decoded.push_back(static_cast<uint8_t>(accumulator >> bits));
                }
            }
            ScanBinaryKeys(std::string_view(reinterpret_cast<const char*>(decoded.data()), decoded.size()), matcher);
            SecureZeroMemory(decoded.data(), decoded.size());
        }
        i = std::max(used, i + 1);
    }
}

// Decimal byte lists such as "235, 228, 30, ..." (C/Python arrays, JSON).
void ScanDecimalKeys(std::string_view text, KeyMatcher& matcher) {
    std::vector<uint8_t> values;
    auto flush = [&] {
        for (size_t at = 0; at + 16 <= values.size() && !matcher.Matched(); ++at) matcher.Try(values.data() + at);
        SecureZeroMemory(values.data(), values.size());
        values.clear();
    };
    for (size_t i = 0; i < text.size() && !matcher.Matched();) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            size_t end = i;
            unsigned value = 0;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9' && end - i < 4) value = value * 10 + unsigned(text[end++] - '0');
            const bool glued = end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '.');
            if (value <= 255 && end - i <= 3 && !glued) values.push_back(static_cast<uint8_t>(value));
            else if (!values.empty()) { if (values.size() >= 16) flush(); values.clear(); }
            i = end;
            continue;
        }
        if (c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}' || c == '[' || c == ']' || c == '(' ||
            c == ')') {
            ++i;
            continue;
        }
        if (!values.empty()) { if (values.size() >= 16) flush(); values.clear(); }
        ++i;
    }
    if (values.size() >= 16) flush();
    SecureZeroMemory(values.data(), values.size());
}

void ScanBinaryKeys(std::string_view data, KeyMatcher& matcher) {
    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    for (size_t at = 0; at + 16 <= data.size() && !matcher.Matched(); ++at) {
        // Flat runs (erased flash, zero padding) cannot be keys.
        if (std::memcmp(p + at, p + at + 1, 15) == 0) continue;
        matcher.Try(p + at);
    }
}

// Returns a note describing the content when it does not unlock the disc.
std::string ScanKeyBytes(std::string_view data, KeyMatcher& matcher, KeyReport& report) {
    const uint64_t before = matcher.Tried();
    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    if (data.size() == 16) {
        if (matcher.Try(p)) return {};
        return "16-byte key, but it is not the Wii common key or this disc's title key";
    }
    // UTF-16 text (Notepad "Unicode"): narrow ASCII and scan as text.
    const bool le16 = data.size() >= 4 && data.size() % 2 == 0 && uint8_t(data[0]) == 0xFF && uint8_t(data[1]) == 0xFE;
    const bool be16 = data.size() >= 4 && data.size() % 2 == 0 && uint8_t(data[0]) == 0xFE && uint8_t(data[1]) == 0xFF;
    if (le16 || be16) {
        KeyPayload narrow;
        for (size_t i = 2; i + 1 < data.size(); i += 2) {
            const char hi = data[i + (le16 ? 1 : 0)], lo = data[i + (le16 ? 0 : 1)];
            narrow.bytes.push_back(hi == 0 && lo != 0 ? lo : '?');
        }
        return ScanKeyBytes(std::string_view(narrow.bytes.data(), narrow.bytes.size()), matcher, report);
    }
    // BootMii keys.bin (alone or appended to a nand.bin): common key at 0x114.
    const auto bootMiiAt = [&](size_t base) {
        return data.size() >= base + 0x400 && data.substr(base, 9) == "BackupMii";
    };
    for (size_t base : {size_t(0), data.size() >= 0x400 ? data.size() - 0x400 : size_t(0)}) {
        if (bootMiiAt(base) && matcher.Try(p + base + 0x114)) return {};
    }
    if (bootMiiAt(0) || (data.size() > 0x400 && bootMiiAt(data.size() - 0x400)))
        return "BootMii key file, but its common key does not unlock this disc (dump from a different console type?)";
    // Wii OTP (0x80) or Wii U OTP (0x400, Wii bank first): common key at 0x14.
    if ((data.size() == 0x80 || data.size() == 0x400) && matcher.Try(p + 0x14)) return {};

    if (LooksLikeText(data)) {
        std::string_view text = data;
        if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
        ScanTextKeys(text, matcher);
        if (!matcher.Matched()) ScanDecimalKeys(text, matcher);
        if (!matcher.Matched()) ScanBase64Keys(text, matcher);
        if (matcher.Matched()) return {};
        const bool cemu = text.find("WUD/WUX") != text.npos || text.find("Wii U Common Key") != text.npos ||
                          text.find("Wii U common key") != text.npos;
        if (cemu) report.wiiUList = true;
        const uint64_t tried = matcher.Tried() - before;
        if (tried == 0) return "text file without any 32-digit hex key";
        return Thousands(tried) + (tried == 1 ? " key checked" : " keys checked") +
               (cemu ? "; this is a Cemu/Wii U key list, and Wii U keys cannot unlock Wii discs" : ", none unlocks this disc");
    }
    ScanBinaryKeys(data, matcher);
    if (matcher.Matched()) return {};
    if (data.size() == 0x108000)
        return "Wii NAND boot-area dump; the common key is not stored there (use BootMii keys.bin or otp.bin)";
    char text[160];
    std::snprintf(text, sizeof text, "%s of binary data scanned at every offset, no key found",
                  data.size() >= 1024 ? (std::to_string(data.size() / 1024) + " KiB").c_str() : (std::to_string(data.size()) + " bytes").c_str());
    return text;
}

// ---- archives ----------------------------------------------------------------------------------------------

// 7z headers are checked here so encrypted and damaged archives get a precise message.
std::string Inspect7z(const fs::path& path) {
    FileStream file(path);
    uint8_t start[32];
    if (!file.Read(0, start, sizeof start) || std::memcmp(start, "7z\xBC\xAF\x27\x1C", 6) != 0) return "not a valid 7z archive";
    uint32_t startCrc = 0;
    std::memcpy(&startCrc, start + 8, 4);
    if (crc32(0, start + 12, 20) != startCrc) return "damaged 7z archive (start header checksum fails)";
    uint64_t offset = 0, size = 0;
    uint32_t headerCrc = 0;
    std::memcpy(&offset, start + 12, 8);
    std::memcpy(&size, start + 20, 8);
    std::memcpy(&headerCrc, start + 28, 4);
    if (size == 0 || size > (16u << 20) || offset > file.Size() || 32 + offset + size > file.Size())
        return "damaged or incomplete 7z archive (header lies past the end of the file)";
    std::vector<uint8_t> header(static_cast<size_t>(size));
    if (!file.Read(32 + offset, header.data(), header.size())) return "unreadable 7z archive";
    const bool crcOk = crc32(0, header.data(), static_cast<uInt>(header.size())) == headerCrc;
    static const uint8_t kAes[] = {0x06, 0xF1, 0x07, 0x01};
    const bool encrypted = !header.empty() && header[0] == 0x17 &&
                           std::search(header.begin(), header.end(), std::begin(kAes), std::end(kAes)) != header.end();
    if (encrypted)
        return std::string("password-protected 7z archive") + (crcOk ? "" : " (its header checksum also fails, so it may be damaged)") +
               "; extract it with 7-Zip and its password, then use the extracted file";
    if (!crcOk) return "damaged 7z archive (header checksum fails); download or re-create it";
    return {};
}

template<class Function>
Function ArchiveSymbol(HMODULE library, const char* name, bool required = true) {
    auto symbol = GetProcAddress(library, name);
    if (!symbol && required) throw ImportError("This Windows version cannot read archives. Extract the key file and select it directly.");
    return reinterpret_cast<Function>(symbol);
}

// Scans every small file inside an archive with the libarchive reader shipped with Windows tar.
std::string ScanArchiveKeys(const fs::path& path, KeyMatcher& matcher, KeyReport& report) {
    if (ExtensionOf(path) == ".7z") {
        const std::string problem = Inspect7z(path);
        if (!problem.empty()) return problem;
    }
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> library(
        LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary);
    if (!library) return "archive could not be opened (the Windows archive reader is unavailable); extract it first";
    auto create = ArchiveSymbol<void*(*)()>(library.get(), "archive_read_new");
    auto release = ArchiveSymbol<int(*)(void*)>(library.get(), "archive_read_free");
    auto all = ArchiveSymbol<int(*)(void*)>(library.get(), "archive_read_support_format_all", false);
    auto filters = ArchiveSymbol<int(*)(void*)>(library.get(), "archive_read_support_filter_all", false);
    auto sevenZip = ArchiveSymbol<int(*)(void*)>(library.get(), "archive_read_support_format_7zip");
    auto open = ArchiveSymbol<int(*)(void*, const wchar_t*, size_t)>(library.get(), "archive_read_open_filename_w");
    auto next = ArchiveSymbol<int(*)(void*, void**)>(library.get(), "archive_read_next_header");
    auto type = ArchiveSymbol<unsigned short(*)(void*)>(library.get(), "archive_entry_filetype");
    auto size = ArchiveSymbol<int64_t(*)(void*)>(library.get(), "archive_entry_size");
    auto read = ArchiveSymbol<intptr_t(*)(void*, void*, size_t)>(library.get(), "archive_read_data");
    auto encryptedEntry = ArchiveSymbol<int(*)(void*)>(library.get(), "archive_entry_is_encrypted", false);
    std::unique_ptr<void, decltype(release)> archive(create(), release);
    if (!archive) return "archive could not be opened";
    if (all) all(archive.get()); else sevenZip(archive.get());
    if (filters) filters(archive.get());
    // A lone .gz/.xz/.bz2/.zst key file has no container format; read it as raw.
    if (auto raw = ArchiveSymbol<int(*)(void*)>(library.get(), "archive_read_support_format_raw", false)) raw(archive.get());
    if (open(archive.get(), path.c_str(), 65536) != 0) return "archive could not be opened (damaged or unsupported)";
    void* entry = nullptr;
    int status;
    unsigned entries = 0, scanned = 0, encrypted = 0;
    uint64_t total = 0;
    const uint64_t before = matcher.Tried();
    while ((status = next(archive.get(), &entry)) == 0 && !matcher.Matched()) {
        if (++entries > 4096) return "archive has too many files";
        if (type(entry) != 0100000) continue;
        if (encryptedEntry && encryptedEntry(entry)) { ++encrypted; continue; }
        const auto length = size(entry);
        if (length > int64_t(kMaxKeyPayload) || total + uint64_t(std::max<int64_t>(length, 0)) > (64u << 20)) continue;
        KeyPayload payload;
        if (length < 1) {
            // A bare .gz/.xz/.bz2/.zst stream does not record its size: read to the end (bounded).
            char chunk[65536];
            for (;;) {
                const auto n = read(archive.get(), chunk, sizeof chunk);
                if (n < 0) return "compressed file could not be read (damaged)";
                if (n == 0) break;
                if (payload.bytes.size() + static_cast<size_t>(n) > kMaxKeyPayload) { payload.bytes.clear(); break; }
                payload.bytes.insert(payload.bytes.end(), chunk, chunk + n);
            }
            SecureZeroMemory(chunk, sizeof chunk);
            if (payload.bytes.empty()) continue;
        } else {
            payload.bytes.resize(static_cast<size_t>(length));
            for (size_t at = 0; at < payload.bytes.size();) {
                const auto n = read(archive.get(), payload.bytes.data() + at, payload.bytes.size() - at);
                if (n <= 0) return encrypted || n < 0 ? "archive member could not be read (password-protected or damaged)" : "damaged archive";
                at += static_cast<size_t>(n);
            }
        }
        total += payload.bytes.size();
        ++scanned;
        ScanKeyBytes(std::string_view(payload.bytes.data(), payload.bytes.size()), matcher, report);
    }
    if (matcher.Matched()) return {};
    if (status != 0 && status != 1) return scanned ? "archive is partly damaged and none of its readable files unlocks this disc"
                                                   : "archive could not be read (password-protected or damaged)";
    if (encrypted && !scanned) return "password-protected archive; extract it with its password first";
    if (!scanned) return "archive contains no small files that could hold a key";
    return std::to_string(scanned) + (scanned == 1 ? " file" : " files") + " inside, " + Thousands(matcher.Tried() - before) +
           " candidate keys checked, none unlocks this disc";
}

// RVZ/WIA partition entries begin with the decrypted title key. Reading them
// lets a WBFS/ISO be imported next to an RVZ of the same game.
std::string ScanStoredTitleKeys(const fs::path& path, KeyMatcher& matcher) {
    FileStream file(path);
    uint8_t first[0x48];
    if (!file.Read(0, first, sizeof first) || (std::memcmp(first, "RVZ\1", 4) != 0 && std::memcmp(first, "WIA\1", 4) != 0))
        return "not an RVZ or WIA image";
    const uint32_t headerSize = Be32(first + 12);
    if (headerSize < 0xA0 || headerSize > 0x10000) return "damaged RVZ/WIA header";
    std::vector<uint8_t> header(headerSize);
    if (!file.Read(0x48, header.data(), header.size())) return "damaged RVZ/WIA header";
    const uint32_t count = Be32(header.data() + 0x90), stride = Be32(header.data() + 0x94);
    if (count > 64 || (count && stride < 16) || stride > 0x1000) return "damaged RVZ/WIA partition table";
    std::vector<uint8_t> entries(size_t(count) * stride);
    if (!file.Read(uint64_t(Be32(header.data() + 0x98)) << 32 | Be32(header.data() + 0x9C), entries.data(), entries.size()))
        return "damaged RVZ/WIA partition table";
    for (uint32_t i = 0; i < count && !matcher.Matched(); ++i) matcher.Try(entries.data() + size_t(i) * stride);
    SecureZeroMemory(entries.data(), entries.size());
    if (matcher.Matched()) return {};
    return "a different game's RVZ/WIA image";
}

// Scans one file. Returns an empty note when the matcher found the key.
std::string ScanKeyFile(const fs::path& path, KeyMatcher& matcher, KeyReport& report) {
    ++report.files;
    const auto ext = ExtensionOf(path);
    try {
        if (ext == ".rvz" || ext == ".wia") return ScanStoredTitleKeys(path, matcher);
        if (ext == ".7z" || ext == ".zip" || ext == ".rar" || ext == ".gz" || ext == ".xz" || ext == ".bz2" || ext == ".zst" ||
            ext == ".tar" || ext == ".tgz" || ext == ".cab")
            return ScanArchiveKeys(path, matcher, report);
        KeyPayload input;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (!file) return "could not be opened";
        if (size <= 0) return "empty file";
        if (size > std::streamoff(kMaxKeyPayload)) return "too large to be a key file";
        input.bytes.resize(static_cast<size_t>(size));
        file.seekg(0);
        if (!file.read(input.bytes.data(), size)) return "could not be read completely";
        const auto* head = reinterpret_cast<const uint8_t*>(input.bytes.data());
        const size_t n = input.bytes.size();
        const bool archive = (n >= 6 && std::memcmp(head, "7z\xBC\xAF\x27\x1C", 6) == 0) || (n >= 4 && std::memcmp(head, "PK\x03\x04", 4) == 0) ||
                             (n >= 4 && std::memcmp(head, "Rar!", 4) == 0) || (n >= 2 && head[0] == 0x1F && head[1] == 0x8B) ||
                             (n >= 6 && std::memcmp(head, "\xFD" "7zXZ", 5) == 0) || (n >= 3 && std::memcmp(head, "BZh", 3) == 0) ||
                             (n >= 4 && std::memcmp(head, "\x28\xB5\x2F\xFD", 4) == 0) || (n >= 4 && std::memcmp(head, "MSCF", 4) == 0);
        if (archive) {
            input.bytes.clear();
            return ScanArchiveKeys(path, matcher, report);
        }
        return ScanKeyBytes(std::string_view(input.bytes.data(), input.bytes.size()), matcher, report);
    } catch (const ImportError& e) {
        return e.what();
    }
}

fs::path ExecutableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) return fs::path(buffer.substr(0, n)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
}

// Files near the disc image (and next to the executable) that may hold a key.
std::vector<fs::path> NearbyKeyFiles(const fs::path& image) {
    std::vector<fs::path> dirs, files;
    auto addDir = [&](const fs::path& dir) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return;
        const auto canonical = fs::weakly_canonical(dir, ec);
        for (const auto& known : dirs)
            if (known == canonical) return;
        dirs.push_back(canonical);
    };
    const fs::path imageDir = fs::absolute(image).parent_path();
    for (const fs::path& base : {imageDir, ExecutableDirectory()}) {
        addDir(base);
        for (const char* sub : {"keys", "Keys", "key", "wii", "Wii"}) addDir(base / sub);
    }
    std::error_code ec;
    const auto self = fs::weakly_canonical(image, ec);
    for (const auto& dir : dirs) {
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc)) continue;
            const fs::path& p = it->path();
            if (fs::weakly_canonical(p, fileEc) == self) continue;
            const auto ext = ExtensionOf(p);
            const auto name = Lower(p.filename().string());
            const auto size = it->file_size(fileEc);
            if (fileEc || size == 0) continue;
            static const char* const kKeyExtensions[] = {".bin", ".key", ".keys", ".txt", ".hex", ".dat", ".7z", ".zip", ".rar", ".gz",
                                                          ".xz", ".bz2", ".zst", ".tar", ".tgz", ".json", ".ini", ".cfg", ".conf", ".xml",
                                                          ".csv", ".log", ".md", ".h", ".c", ".py", ".inc", ".yml", ".yaml", ".toml", ".otp"};
            bool keyish = ext.empty() || name.find("key") != name.npos || name.find("otp") != name.npos ||
                          name.find("nand") != name.npos || name.find("common") != name.npos;
            for (const char* known : kKeyExtensions) keyish = keyish || ext == known;
            if ((ext == ".rvz" || ext == ".wia") || (keyish && size <= kMaxKeyPayload)) files.push_back(p);
            if (files.size() >= 64) return files;
        }
    }
    // Likely key files first, then RVZ/WIA images, then the rest.
    auto rank = [](const fs::path& p) {
        const auto name = Lower(p.filename().string());
        const auto ext = ExtensionOf(p);
        if (name.find("key") != name.npos || name.find("otp") != name.npos) return 0;
        if (ext == ".rvz" || ext == ".wia") return 1;
        return 2;
    };
    std::stable_sort(files.begin(), files.end(), [&](const fs::path& a, const fs::path& b) { return rank(a) < rank(b); });
    return files;
}

std::string KeyFailureMessage(const std::vector<std::pair<fs::path, std::string>>& notes, const KeyReport& report, bool explicitFile) {
    std::string message = explicitFile ? "That file does not unlock this disc." : "This disc is encrypted and no key that unlocks it was found";
    if (!explicitFile) message += notes.empty() ? " next to the disc image." : ".";
    size_t shown = 0;
    for (const auto& [path, note] : notes) {
        if (note.empty()) continue;
        if (shown++ == 4) {
            message += "\n(" + std::to_string(notes.size() - 4) + " more files checked)";
            break;
        }
        message += "\n- " + path.filename().string() + ": " + note;
    }
    message += "\nSelect BootMii keys.bin, otp.bin, a 16-byte common key, or any text file containing the Wii common key or the R2VE01 title key."
               " An RVZ of this disc also works: it needs no key.";
    (void)report;
    return message;
}
