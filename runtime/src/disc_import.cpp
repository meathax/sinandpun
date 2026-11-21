// First-run disc import. See disc_import.h.
//
// Wii disc format (wiibrew "Wii disc" / Dolphin VolumeWii):
//   0x00000  disc header (game id at 0, Wii magic 0x5D1C9EA3 at 0x18)
//   0x40000  partition table: 4 groups of {count, tableOffset>>2}; each entry {partitionOffset>>2, type}
//   partition: ticket (0x2A4 bytes; encrypted title key at +0x1BF, title id at +0x1DC, common-key index at +0x1F1),
//     data offset>>2 at +0x2B8, data size>>2 at +0x2BC. Data is 0x8000-byte clusters: 0x400 bytes of hashes
//     (AES-CBC, IV 0) + 0x7C00 bytes of data (AES-CBC, IV = bytes 0x3D0..0x3DF of the *encrypted* hash block).
//   decrypted partition data starts with boot.bin (0x440), bi2.bin (0x2000), apploader; boot.bin holds the
//   DOL / FST offsets (>>2) and the FST size (>>2).

#include "disc_import.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <string_view>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;

namespace DiscImport {

void Progress::SetStage(std::string stage, std::string detail) {
    std::lock_guard<std::mutex> lock(m_lock);
    m_stage = std::move(stage);
    m_detail = std::move(detail);
}

void Progress::Get(std::string& stage, std::string& detail) const {
    std::lock_guard<std::mutex> lock(m_lock);
    stage = m_stage;
    detail = m_detail;
}

namespace {

struct ImportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ImportCancelled {};

uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

constexpr uint32_t kWiiMagic = 0x5D1C9EA3u;
constexpr uint32_t kGcMagic = 0xC2339F3Du;
constexpr uint32_t kClusterSize = 0x8000;
constexpr uint32_t kClusterHashBytes = 0x400;
constexpr uint32_t kClusterDataBytes = 0x7C00;
constexpr uint64_t kSpaceMargin = 64ull << 20;
constexpr uint64_t kModuleProgressWeight = 16ull << 20;


// ---- CNG (bcrypt.dll, resolved at run time so the build needs no extra import library) -----------------------

struct Cng {
    decltype(&BCryptOpenAlgorithmProvider) openAlg = nullptr;
    decltype(&BCryptCloseAlgorithmProvider) closeAlg = nullptr;
    decltype(&BCryptSetProperty) setProperty = nullptr;
    decltype(&BCryptGenerateSymmetricKey) generateKey = nullptr;
    decltype(&BCryptDestroyKey) destroyKey = nullptr;
    decltype(&BCryptDecrypt) decrypt = nullptr;
    decltype(&BCryptCreateHash) createHash = nullptr;
    decltype(&BCryptHashData) hashData = nullptr;
    decltype(&BCryptFinishHash) finishHash = nullptr;
    decltype(&BCryptDestroyHash) destroyHash = nullptr;

    static const Cng& Get() {
        static const Cng cng = [] {
            Cng c;
            HMODULE dll = LoadLibraryW(L"bcrypt.dll");
            auto sym = [&](const char* name) { return dll ? reinterpret_cast<void*>(GetProcAddress(dll, name)) : nullptr; };
#define CNG_BIND(field, name) c.field = reinterpret_cast<decltype(c.field)>(sym(name))
            CNG_BIND(openAlg, "BCryptOpenAlgorithmProvider");
            CNG_BIND(closeAlg, "BCryptCloseAlgorithmProvider");
            CNG_BIND(setProperty, "BCryptSetProperty");
            CNG_BIND(generateKey, "BCryptGenerateSymmetricKey");
            CNG_BIND(destroyKey, "BCryptDestroyKey");
            CNG_BIND(decrypt, "BCryptDecrypt");
            CNG_BIND(createHash, "BCryptCreateHash");
            CNG_BIND(hashData, "BCryptHashData");
            CNG_BIND(finishHash, "BCryptFinishHash");
            CNG_BIND(destroyHash, "BCryptDestroyHash");
#undef CNG_BIND
            if (!c.openAlg || !c.closeAlg || !c.setProperty || !c.generateKey || !c.destroyKey || !c.decrypt || !c.createHash ||
                !c.hashData || !c.finishHash || !c.destroyHash) {
                throw ImportError("Windows cryptography (bcrypt.dll) is unavailable.");
            }
            return c;
        }();
        return cng;
    }
};

class AesCbcDecryptor {
public:
    explicit AesCbcDecryptor(const uint8_t key[16]) : m_cng(Cng::Get()) {
        if (!BCRYPT_SUCCESS(m_cng.openAlg(&m_alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) ||
            !BCRYPT_SUCCESS(m_cng.setProperty(m_alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                                              sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) ||
            !BCRYPT_SUCCESS(m_cng.generateKey(m_alg, &m_key, nullptr, 0, const_cast<PUCHAR>(key), 16, 0))) {
            Close();
            throw ImportError("Windows cryptography (AES) is unavailable.");
        }
    }
    ~AesCbcDecryptor() { Close(); }
    AesCbcDecryptor(const AesCbcDecryptor&) = delete;
    AesCbcDecryptor& operator=(const AesCbcDecryptor&) = delete;

    void Decrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t size) const {
        uint8_t ivCopy[16];
        std::memcpy(ivCopy, iv, 16);
        ULONG written = 0;
        if (!BCRYPT_SUCCESS(m_cng.decrypt(m_key, const_cast<PUCHAR>(in), static_cast<ULONG>(size), nullptr, ivCopy, 16, out,
                                          static_cast<ULONG>(size), &written, 0)) ||
            written != size) {
            throw ImportError("AES decryption failed.");
        }
    }

private:
    void Close() {
        if (m_key) m_cng.destroyKey(m_key);
        if (m_alg) m_cng.closeAlg(m_alg, 0);
        m_key = nullptr;
        m_alg = nullptr;
    }
    const Cng& m_cng;
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_KEY_HANDLE m_key = nullptr;
};

class Sha1 {
public:
    Sha1() : m_cng(Cng::Get()) {
        if (!BCRYPT_SUCCESS(m_cng.openAlg(&m_alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) {
            throw ImportError("Windows cryptography (SHA-1) is unavailable.");
        }
    }
    ~Sha1() {
        if (m_alg) m_cng.closeAlg(m_alg, 0);
    }
    Sha1(const Sha1&) = delete;
    Sha1& operator=(const Sha1&) = delete;
    void Digest(const uint8_t* data, size_t size, uint8_t out[20]) const {
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (!BCRYPT_SUCCESS(m_cng.createHash(m_alg, &hash, nullptr, 0, nullptr, 0, 0))) throw ImportError("SHA-1 failed.");
        const bool ok = BCRYPT_SUCCESS(m_cng.hashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0)) &&
                        BCRYPT_SUCCESS(m_cng.finishHash(hash, out, 20, 0));
        m_cng.destroyHash(hash);
        if (!ok) throw ImportError("SHA-1 failed.");
    }

private:
    const Cng& m_cng;
    BCRYPT_ALG_HANDLE m_alg = nullptr;
};
// ---- raw disc image containers --------------------------------------------------------------------------

class RawImage {
public:
    virtual ~RawImage() = default;
    // Fills `dst` with `size` bytes at logical disc offset `off`. Returns false when the range is beyond the image.
    virtual bool Read(uint64_t off, void* dst, size_t size) = 0;
    virtual uint64_t Size() const = 0;
    // RVZ can store the logical partition payload directly.
    virtual bool SelectPartition(uint64_t, uint64_t, const uint8_t*) { return false; }
    virtual void ReadPartition(uint64_t, void*, size_t) { throw ImportError("Missing RVZ partition."); }
};

class FileStream {
public:
    explicit FileStream(const fs::path& path) : m_file(path, std::ios::binary) {
        if (!m_file) throw ImportError("The disc image could not be opened: " + path.filename().string());
        m_file.seekg(0, std::ios::end);
        m_size = static_cast<uint64_t>(m_file.tellg());
        m_file.seekg(0);
    }
    uint64_t Size() const { return m_size; }
    bool Read(uint64_t off, void* dst, size_t size) {
        if (size == 0) return true;
        if (off > m_size || size > m_size - off) return false;
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(off));
        m_file.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
        return static_cast<size_t>(m_file.gcount()) == size;
    }

private:
    std::ifstream m_file;
    uint64_t m_size = 0;
};

class IsoImage : public RawImage {
public:
    explicit IsoImage(const fs::path& path) : m_stream(path) {}
    bool Read(uint64_t off, void* dst, size_t size) override { return m_stream.Read(off, dst, size); }
    uint64_t Size() const override { return m_stream.Size(); }

private:
    FileStream m_stream;
};

// A .bin of 2352-byte (or 2048-byte) sectors; `dataOffset` is the user-data offset inside a raw sector.
class BinImage : public RawImage {
public:
    BinImage(const fs::path& path, uint32_t stride, uint32_t dataOffset) : m_stream(path), m_stride(stride), m_dataOffset(dataOffset) {}
    bool Read(uint64_t off, void* dst, size_t size) override {
        auto* out = static_cast<uint8_t*>(dst);
        while (size) {
            const uint64_t sector = off / 2048;
            const size_t within = static_cast<size_t>(off % 2048);
            const size_t chunk = std::min<size_t>(size, 2048 - within);
            if (!m_stream.Read(sector * m_stride + m_dataOffset + within, out, chunk)) return false;
            out += chunk;
            off += chunk;
            size -= chunk;
        }
        return true;
    }
    uint64_t Size() const override { return m_stream.Size() / m_stride * 2048; }

private:
    FileStream m_stream;
    uint32_t m_stride, m_dataOffset;
};

class WbfsImage : public RawImage {
public:
    explicit WbfsImage(const fs::path& path) : m_stream(path) {
        uint8_t head[16];
        if (!m_stream.Read(0, head, sizeof head) || std::memcmp(head, "WBFS", 4) != 0) throw ImportError("Not a WBFS file.");
        const uint32_t hdSecShift = head[8];
        m_wbfsShift = head[9];
        if (hdSecShift < 9 || hdSecShift > 12 || m_wbfsShift < 15 || m_wbfsShift > 30) throw ImportError("The WBFS file header is damaged.");
        if (head[0x0C] == 0) throw ImportError("The WBFS file does not contain a disc.");
        const uint64_t hdSector = 1ull << hdSecShift;
        // libwbfs: n_wbfs_sec_per_disc = (143432 * 2) >> (wbfs_sec_sz_s - 15); disc info (header copy + wlba table) is in LBA 1.
        const uint64_t entries = (143432ull * 2) >> (m_wbfsShift - 15);
        std::vector<uint8_t> raw(static_cast<size_t>(entries * 2));
        if (!m_stream.Read(hdSector + 0x100, raw.data(), raw.size())) throw ImportError("The WBFS file is truncated.");
        m_table.resize(static_cast<size_t>(entries));
        for (size_t i = 0; i < m_table.size(); ++i) m_table[i] = Be16(raw.data() + i * 2);
    }
    bool Read(uint64_t off, void* dst, size_t size) override {
        auto* out = static_cast<uint8_t*>(dst);
        const uint64_t secSize = 1ull << m_wbfsShift;
        while (size) {
            const uint64_t index = off >> m_wbfsShift;
            const size_t within = static_cast<size_t>(off & (secSize - 1));
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(size, secSize - within));
            if (index >= m_table.size()) return false;
            if (m_table[static_cast<size_t>(index)] == 0) {
                std::memset(out, 0, chunk);
            } else if (!m_stream.Read((static_cast<uint64_t>(m_table[static_cast<size_t>(index)]) << m_wbfsShift) + within, out, chunk)) {
                return false;
            }
            out += chunk;
            off += chunk;
            size -= chunk;
        }
        return true;
    }
    uint64_t Size() const override { return static_cast<uint64_t>(m_table.size()) << m_wbfsShift; }

private:
    FileStream m_stream;
    uint32_t m_wbfsShift = 0;
    std::vector<uint16_t> m_table;
};

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

fs::path Utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string ExtensionOf(const fs::path& path) { return Lower(path.extension().string()); }

// Returns the .bin a .cue refers to, or empty when none is found.
fs::path BinFromCue(const fs::path& cue) {
    std::ifstream in(cue);
    std::string line;
    while (std::getline(in, line)) {
        const size_t file = Lower(line).find("file");
        if (file == std::string::npos) continue;
        const size_t q1 = line.find('"', file);
        const size_t q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) continue;
        return cue.parent_path() / Utf8Path(line.substr(q1 + 1, q2 - q1 - 1));
    }
    return {};
}

#include "compressed_disc.inc"

std::unique_ptr<RawImage> OpenDiscImage(const fs::path& chosen) {
    const std::string ext = ExtensionOf(chosen);
    if (ext == ".rvz") return std::make_unique<RvzImage>(chosen);
    if (ext == ".gcz") return std::make_unique<GczImage>(chosen);
    if (ext == ".wia" || ext == ".ciso" || ext == ".nfs" || ext == ".wad") {
        throw ImportError("Compressed or container formats (" + ext + ") are not supported. Convert the disc to a plain .iso (Dolphin: right-click the game, Convert) and select that instead.");
    }
    fs::path image = chosen;
    {
        std::error_code dirEc;
        if (fs::is_directory(chosen, dirEc)) {
            throw ImportError("That is a folder. Select the disc image file itself (.iso, .wbfs or .cue/.bin).");
        }
    }
    if (ext == ".cue") {
        image = BinFromCue(chosen);
        std::error_code ec;
        if (image.empty() || !fs::is_regular_file(image, ec)) {
            throw ImportError("The .cue sheet does not point to an existing .bin file next to it.");
        }
    }
    {
        std::error_code ec;
        if (!fs::is_regular_file(image, ec)) throw ImportError("The selected file does not exist.");
    }
    uint8_t head[4] = {};
    {
        FileStream probe(image);
        if (probe.Size() < 0x50000) throw ImportError("The selected file is too small to be a Wii disc image.");
        probe.Read(0, head, 4);
    }
    if (std::memcmp(head, "WBFS", 4) == 0) return std::make_unique<WbfsImage>(image);

    // Plain ISO first, then raw-sector .bin layouts (MODE1/2352, MODE2/2352), told apart by the Wii/GC magic.
    struct Layout {
        uint32_t stride, dataOffset;
    };
    static constexpr Layout kLayouts[] = {{2048, 0}, {2352, 16}, {2352, 24}};
    for (const Layout& layout : kLayouts) {
        uint8_t magic[8] = {};
        FileStream probe(image);
        if (!probe.Read(layout.dataOffset + 0x18, magic, 8)) continue;
        if (Be32(magic) == kWiiMagic || Be32(magic + 4) == kGcMagic) {
            if (layout.stride == 2048) return std::make_unique<IsoImage>(image);
            return std::make_unique<BinImage>(image, layout.stride, layout.dataOffset);
        }
    }
    uint8_t nk[4] = {};
    FileStream probe(image);
    if (probe.Read(0x200, nk, 4) && std::memcmp(nk, "NKIT", 4) == 0) {
        throw ImportError("NKit-compressed images are not supported. Use a regular .iso dump (or convert it back with NKit's Recover function).");
    }
    throw ImportError("This does not look like a Wii disc image. Select the .iso, .wbfs or .cue/.bin of your Sin & Punishment: Star Successor disc.");
}

// ---- Wii partition ----------------------------------------------------------------------------------------

class WiiPartition {
public:
    WiiPartition(RawImage& raw, uint64_t partitionOffset, const Sha1& sha, std::atomic<bool>& cancel,
                 const fs::path& keyFile)
        : m_raw(raw), m_sha(sha), m_cancel(cancel) {
        uint8_t ticket[0x2C0];
        if (!raw.Read(partitionOffset, ticket, sizeof ticket)) throw ImportError("The disc image is truncated (partition header).");
        const uint8_t commonIndex = ticket[0x1F1];
        if (commonIndex > 2) throw ImportError("The disc uses an unknown common key; the image may be damaged.");
        struct Key {
            uint8_t bytes[16]{};
            ~Key() { SecureZeroMemory(bytes, sizeof bytes); }
        } commonKey, titleKey;
        if (keyFile.empty()) {
            throw ImportError("Select your own 16-byte Wii common key file, or use an extracted game folder.");
        }
        std::ifstream keyInput(keyFile, std::ios::binary);
        if (!keyInput.read(reinterpret_cast<char*>(commonKey.bytes), sizeof commonKey.bytes) ||
            keyInput.peek() != std::char_traits<char>::eof() || keyInput.bad()) {
            throw ImportError("The Wii common key file must contain exactly 16 binary bytes.");
        }
        uint8_t iv[16] = {};
        std::memcpy(iv, ticket + 0x1DC, 8);
        AesCbcDecryptor(commonKey.bytes).Decrypt(iv, ticket + 0x1BF, titleKey.bytes, 16);
        m_dataStart = partitionOffset + (static_cast<uint64_t>(Be32(ticket + 0x2B8)) << 2);
        const uint64_t encryptedSize = static_cast<uint64_t>(Be32(ticket + 0x2BC)) << 2;
        m_clusters = encryptedSize / kClusterSize;
        if (m_clusters == 0) throw ImportError("The disc image has an empty data partition.");
        m_plainPartition = raw.SelectPartition(m_dataStart, m_clusters, titleKey.bytes);
        if (!m_plainPartition) m_aes = std::make_unique<AesCbcDecryptor>(titleKey.bytes);
        m_raw_buf.resize(size_t(kChunkClusters) * kClusterSize);
        m_plain.resize(size_t(kChunkClusters) * kClusterDataBytes);
    }

    uint64_t DataSize() const { return m_clusters * kClusterDataBytes; }

    void Read(uint64_t off, void* dst, size_t size) {
        auto* out = static_cast<uint8_t*>(dst);
        if (off + size > DataSize()) throw ImportError("The disc image refers to data past the end of its partition (damaged image).");
        while (size) {
            const uint64_t cluster = off / kClusterDataBytes;
            const uint64_t chunk = cluster / kChunkClusters;
            if (chunk != m_cachedChunk) LoadChunk(chunk);
            const uint64_t base = chunk * kChunkClusters * kClusterDataBytes;
            const size_t within = static_cast<size_t>(off - base);
            const size_t avail = static_cast<size_t>(m_cachedBytes) - within;
            const size_t n = std::min(size, avail);
            std::memcpy(out, m_plain.data() + within, n);
            out += n;
            off += n;
            size -= n;
        }
    }

private:
    static constexpr uint32_t kChunkClusters = 16;

    void LoadChunk(uint64_t chunk) {
        if (m_cancel.load(std::memory_order_relaxed)) throw ImportCancelled{};
        const uint64_t first = chunk * kChunkClusters;
        const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(kChunkClusters, m_clusters - first));
        if (m_plainPartition) {
            m_cachedBytes = uint64_t(count) * kClusterDataBytes;
            m_raw.ReadPartition(first * kClusterDataBytes, m_plain.data(), m_cachedBytes);
            m_cachedChunk = chunk;
            return;
        }
        if (!m_raw.Read(m_dataStart + first * kClusterSize, m_raw_buf.data(), size_t(count) * kClusterSize)) {
            throw ImportError("The disc image is truncated or unreadable (it ends before all game data).");
        }
        static const uint8_t zeroIv[16] = {};
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* in = m_raw_buf.data() + size_t(i) * kClusterSize;
            uint8_t* out = m_plain.data() + size_t(i) * kClusterDataBytes;
            uint8_t hashes[kClusterHashBytes];
            m_aes->Decrypt(zeroIv, in, hashes, kClusterHashBytes);
            m_aes->Decrypt(in + 0x3D0, in + kClusterHashBytes, out, kClusterDataBytes);
            // H0: SHA-1 of each of the 31 0x400-byte blocks is stored in the (decrypted) hash block.
            for (uint32_t b = 0; b < 31; ++b) {
                uint8_t digest[20];
                m_sha.Digest(out + size_t(b) * 0x400, 0x400, digest);
                if (std::memcmp(digest, hashes + b * 20, 20) != 0) {
                    char text[160];
                    std::snprintf(text, sizeof text,
                                  "The disc image is damaged: sector %llu failed its integrity check. Re-dump the disc or download it again.",
                                  static_cast<unsigned long long>(first + i));
                    throw ImportError(text);
                }
            }
        }
        m_cachedChunk = chunk;
        m_cachedBytes = uint64_t(count) * kClusterDataBytes;
    }

    bool m_plainPartition = false;
    RawImage& m_raw;
    const Sha1& m_sha;
    std::atomic<bool>& m_cancel;
    std::unique_ptr<AesCbcDecryptor> m_aes;
    uint64_t m_dataStart = 0;
    uint64_t m_clusters = 0;
    std::vector<uint8_t> m_raw_buf, m_plain;
    uint64_t m_cachedChunk = UINT64_MAX;
    uint64_t m_cachedBytes = 0;
};

struct FileItem {
    fs::path relative;
    uint64_t offset;
    uint32_t size;
};

std::string CleanName(const std::string& raw) {
    std::string name;
    for (char c : raw) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos) c = '_';
        name.push_back(c);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty() || name == "." || name == "..") throw ImportError("The disc image has an invalid file name table (damaged image).");
    return name;
}

std::vector<FileItem> ParseFst(const std::vector<uint8_t>& fst, uint64_t dataSize) {
    if (fst.size() < 12 || fst[0] != 1) throw ImportError("The disc image has an invalid file table (damaged image).");
    const uint32_t count = Be32(&fst[8]);
    if (count < 1 || count > 0x200000 || size_t(count) * 12 > fst.size()) throw ImportError("The disc image has an invalid file table (damaged image).");
    const size_t namesBase = size_t(count) * 12;
    auto nameAt = [&](uint32_t offset24) {
        const size_t at = namesBase + offset24;
        if (at >= fst.size()) throw ImportError("The disc image has an invalid file name table (damaged image).");
        const auto* end = static_cast<const uint8_t*>(std::memchr(&fst[at], 0, fst.size() - at));
        if (!end) throw ImportError("The disc image has an invalid file name table (damaged image).");
        return std::string(reinterpret_cast<const char*>(&fst[at]), reinterpret_cast<const char*>(end));
    };
    struct Dir {
        uint32_t end;
        fs::path path;
    };
    std::vector<Dir> stack{{count, fs::path{}}};
    std::vector<FileItem> files;
    for (uint32_t i = 1; i < count; ++i) {
        while (stack.size() > 1 && i >= stack.back().end) stack.pop_back();
        const uint8_t* e = &fst[size_t(i) * 12];
        const bool dir = e[0] != 0;
        const uint32_t nameOff = Be32(e) & 0x00FFFFFFu;
        const uint32_t a = Be32(e + 4), b = Be32(e + 8);
        fs::path path = stack.back().path / Utf8Path(CleanName(nameAt(nameOff)));
        if (dir) {
            if (b > count || b <= i) throw ImportError("The disc image has an invalid directory table (damaged image).");
            stack.push_back({b, std::move(path)});
        } else {
            const uint64_t off = static_cast<uint64_t>(a) << 2;
            if (off + b > dataSize) throw ImportError("The disc image lists a file past the end of the data (damaged image).");
            files.push_back({std::move(path), off, b});
        }
    }
    return files;
}

uint32_t DolSize(const uint8_t* header) {
    uint64_t end = 0x100;
    for (int i = 0; i < 18; ++i) {
        const uint32_t off = Be32(header + 4 * i), size = Be32(header + 0x90 + 4 * i);
        if (size) end = std::max<uint64_t>(end, uint64_t(off) + size);
    }
    if (end > (64u << 20)) throw ImportError("The disc image has an invalid main executable (damaged image).");
    return static_cast<uint32_t>(end);
}

std::string UpperId(const char* id6) {
    std::string s(id6, 6);
    for (char& c : s) {
        if (c < 0x20 || c > 0x7E) c = '?';
    }
    return s;
}

void WriteFile(const fs::path& path, const void* data, size_t size, Progress& progress) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.flush();
    if (!out) throw ImportError("Writing to disk failed (is the disk full?): " + path.filename().string());
    progress.done += size;
}

}  // namespace

bool IsSupportedDiscExtension(const fs::path& path) {
    const std::string ext = ExtensionOf(path);
    return ext == ".rvz" || ext == ".gcz" || ext == ".iso" || ext == ".wbfs" || ext == ".bin" || ext == ".cue" || ext == ".gcm";
}

bool IsGameDirectoryComplete(const fs::path& gameDir) {
    std::error_code ec;
    return fs::is_directory(gameDir / "files", ec) && fs::is_regular_file(gameDir / "sys" / "boot.bin", ec) &&
           fs::is_regular_file(gameDir / "sys" / "fst.bin", ec) && fs::is_regular_file(gameDir / "sys" / "main.dol", ec);
}

Result ImportDisc(const fs::path& image, const fs::path& gameDir, Progress& progress, Summary& summary, std::string& error,
                  const fs::path& requestedKeyFile) {
    const auto started = std::chrono::steady_clock::now();
    const fs::path staging = fs::path(gameDir).concat(".importing");
    auto cleanup = [&] {
        std::error_code ec;
        fs::remove_all(staging, ec);
    };
    try {
        progress.done = 0;
        progress.total = 1;
        progress.SetStage("Reading disc image");
        auto keyFile = requestedKeyFile;
        if (keyFile.empty()) {
            if (const wchar_t* configured = _wgetenv(L"SIN_WII_KEY_FILE")) keyFile = configured;
        }
        if (keyFile.empty()) {
            throw ImportError("Disc import needs your own 16-byte Wii common key file. Pass --keys or choose an extracted game folder.");
        }
        std::error_code keyError;
        if (fs::file_size(keyFile, keyError) != 16 || keyError) {
            throw ImportError("The Wii common key file must contain exactly 16 binary bytes.");
        }
        auto raw = OpenDiscImage(image);
        uint8_t header[0x60];
        if (!raw->Read(0, header, sizeof header)) throw ImportError("The disc image is truncated.");
        const uint32_t wiiMagic = Be32(header + 0x18), gcMagic = Be32(header + 0x1C);
        const std::string id = UpperId(reinterpret_cast<const char*>(header));
        if (wiiMagic != kWiiMagic) {
            if (gcMagic == kGcMagic) throw ImportError("This is a GameCube disc (" + id + "). Sin & Punishment: Star Successor is a Wii game.");
            throw ImportError("This does not look like a Wii disc image.");
        }
        if (id != kGameId) {
            if (id.compare(0, 3, "R2V") == 0 && id.size() == 6) {
                const char region = id[3];
                const char* name = region == 'P' ? "European (PAL)" : region == 'J' ? "Japanese" : region == 'K' ? "Korean" : "another region's";
                throw ImportError(std::string("This is the ") + name + " version of Sin & Punishment: Star Successor (" + id +
                                  "). Only the North American version (" + kGameId + ") is supported.");
            }
            throw ImportError("This disc image is \"" + id + "\", not Sin & Punishment: Star Successor (" + kGameId + ").");
        }
        summary.gameId = id;

        // Find the data partition (type 0).
        uint8_t groups[32];
        if (!raw->Read(0x40000, groups, sizeof groups)) throw ImportError("The disc image is truncated (partition table).");
        uint64_t partitionOffset = 0;
        for (int g = 0; g < 4 && !partitionOffset; ++g) {
            const uint32_t count = Be32(groups + g * 8);
            const uint64_t table = static_cast<uint64_t>(Be32(groups + g * 8 + 4)) << 2;
            if (count == 0 || count > 16 || table == 0) continue;
            for (uint32_t i = 0; i < count; ++i) {
                uint8_t entry[8];
                if (!raw->Read(table + i * 8, entry, 8)) throw ImportError("The disc image is truncated (partition table).");
                if (Be32(entry + 4) == 0) {
                    partitionOffset = static_cast<uint64_t>(Be32(entry)) << 2;
                    break;
                }
            }
        }
        if (partitionOffset == 0) throw ImportError("The disc image has no game partition (damaged or scrubbed image).");

        Sha1 sha;
        WiiPartition part(*raw, partitionOffset, sha, progress.cancel, keyFile);

        uint8_t bootBin[0x440];
        part.Read(0, bootBin, sizeof bootBin);
        if (std::memcmp(bootBin, kGameId, 6) != 0) throw ImportError("The disc's game partition belongs to a different game (damaged image).");
        std::vector<uint8_t> bi2(0x2000);
        part.Read(0x440, bi2.data(), bi2.size());
        uint8_t loaderHeader[0x20];
        part.Read(0x2440, loaderHeader, sizeof loaderHeader);
        const uint64_t loaderSize = 0x20ull + Be32(loaderHeader + 0x14) + Be32(loaderHeader + 0x18);
        if (loaderSize > (4u << 20)) throw ImportError("The disc image has an invalid apploader (damaged image).");
        std::vector<uint8_t> apploader(static_cast<size_t>(loaderSize));
        part.Read(0x2440, apploader.data(), apploader.size());

        const uint64_t dolOffset = static_cast<uint64_t>(Be32(bootBin + 0x420)) << 2;
        const uint64_t fstOffset = static_cast<uint64_t>(Be32(bootBin + 0x424)) << 2;
        const uint64_t fstSize = static_cast<uint64_t>(Be32(bootBin + 0x428)) << 2;
        if (fstSize < 12 || fstSize > (64u << 20)) throw ImportError("The disc image has an invalid file table (damaged image).");
        uint8_t dolHeader[0x100];
        part.Read(dolOffset, dolHeader, sizeof dolHeader);
        const uint32_t dolSize = DolSize(dolHeader);
        std::vector<uint8_t> dol(dolSize);
        part.Read(dolOffset, dol.data(), dol.size());
        std::vector<uint8_t> fst(static_cast<size_t>(fstSize));
        part.Read(fstOffset, fst.data(), fst.size());
        const std::vector<FileItem> files = ParseFst(fst, part.DataSize());

        uint64_t bytes = sizeof bootBin + bi2.size() + apploader.size() + dol.size() + fst.size();
        for (const FileItem& f : files) bytes += f.size;
        progress.total = bytes + kModuleProgressWeight;

        std::error_code ec;
        fs::create_directories(gameDir.parent_path(), ec);
        const auto space = fs::space(gameDir.parent_path(), ec);
        const uint64_t required = bytes + kModuleProgressWeight + kSpaceMargin;
        if (!ec && space.available < required) {
            char text[200];
            std::snprintf(text, sizeof text, "Not enough free disk space: %.1f GB needed, %.1f GB available.",
                          double(required) / (1024.0 * 1024.0 * 1024.0), double(space.available) / (1024.0 * 1024.0 * 1024.0));
            throw ImportError(text);
        }

        cleanup();
        progress.SetStage("Extracting game files");
        WriteFile(staging / "sys" / "boot.bin", bootBin, sizeof bootBin, progress);
        WriteFile(staging / "sys" / "bi2.bin", bi2.data(), bi2.size(), progress);
        WriteFile(staging / "sys" / "apploader.img", apploader.data(), apploader.size(), progress);
        WriteFile(staging / "sys" / "main.dol", dol.data(), dol.size(), progress);
        WriteFile(staging / "sys" / "fst.bin", fst.data(), fst.size(), progress);

        std::vector<uint8_t> buffer(1u << 20);
        for (const FileItem& f : files) {
            progress.SetStage("Extracting game files", f.relative.generic_string());
            const fs::path target = staging / "files" / f.relative;
            fs::create_directories(target.parent_path());
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) throw ImportError("Could not create " + target.filename().string() + " (check the folder permissions).");
            uint64_t remaining = f.size, at = f.offset;
            while (remaining) {
                if (progress.cancel.load(std::memory_order_relaxed)) throw ImportCancelled{};
                const size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                part.Read(at, buffer.data(), n);
                out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(n));
                if (!out) throw ImportError("Writing to disk failed (is the disk full?): " + target.filename().string());
                at += n;
                remaining -= n;
                progress.done += n;
            }
            out.close();
            ++summary.files;
        }

        // Everything derived from the disc: prelinked stage module images.
        progress.SetStage("Preparing stage modules");
        auto slurp = [](const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            if (!in) throw ImportError("A required game file is missing from the disc: " + p.filename().string());
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        const std::vector<uint8_t> sel = slurp(staging / "files" / "gs2.sel");
        for (int i = 0; i < kModuleCount; ++i) {
            if (progress.cancel.load(std::memory_order_relaxed)) throw ImportCancelled{};
            char name[32];
            std::snprintf(name, sizeof name, "stg%02d", i);
            progress.SetStage("Preparing stage modules", name);
            std::vector<uint8_t> image;
            std::string moduleError;
            if (!BuildModuleImage(slurp(staging / "files" / (std::string(name) + ".rso")), sel, kModuleBase, image, moduleError)) {
                throw ImportError(std::string("Could not link stage module ") + name + ": " + moduleError);
            }
            WriteFile(staging / "modules" / (std::string(name) + ".bin"), image.data(), image.size(), progress);
            ++summary.modules;
        }
        progress.done = progress.total.load();

        progress.SetStage("Finishing");
        const auto previous = fs::path(gameDir).concat(".previous");
        const auto moveDirectory = [](const fs::path& from, const fs::path& to) {
            DWORD failure = ERROR_SUCCESS;
            for (int attempt = 0; attempt != 20; ++attempt) {
                if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return;
                failure = GetLastError();
                if (failure != ERROR_ACCESS_DENIED && failure != ERROR_SHARING_VIOLATION) break;
                Sleep(50);
            }
            throw fs::filesystem_error("Could not move imported game folder", from, to,
                                       std::error_code(failure, std::system_category()));
        };
        if (fs::exists(previous)) {
            if (fs::exists(gameDir)) fs::remove_all(previous);
            else moveDirectory(previous, gameDir);
        }
        const bool replacing = fs::exists(gameDir);
        if (replacing) moveDirectory(gameDir, previous);
        try {
            moveDirectory(staging, gameDir);
        } catch (...) {
            if (replacing) moveDirectory(previous, gameDir);
            throw;
        }
        if (replacing) fs::remove_all(previous, ec);
        summary.bytes = bytes;
        summary.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return Result::Ok;
    } catch (const ImportCancelled&) {
        cleanup();
        error = "Import cancelled.";
        return Result::Cancelled;
    } catch (const ImportError& e) {
        cleanup();
        error = e.what();
        return Result::Failed;
    } catch (const fs::filesystem_error& e) {
        cleanup();
        error = std::string("File system error: ") + e.code().message() + " (" + e.path1().filename().string() + ")";
        return Result::Failed;
    } catch (const std::exception& e) {
        cleanup();
        error = std::string("Unexpected error: ") + e.what();
        return Result::Failed;
    }
}

// ---- RSO prelink (port of tools/rso_link.py) ---------------------------------------------------------------

namespace {

constexpr uint32_t kEti = 241, kAbs = 65521;
constexpr uint32_t R_NONE = 0, R_ADDR32 = 1, R_ADDR16_LO = 4, R_ADDR16_HI = 5, R_ADDR16_HA = 6, R_REL24 = 10, R_REL14 = 11, R_UADDR32 = 24;
constexpr uint32_t kEtiBase = 0x800174A0u;
constexpr uint32_t kPoison = 0xDEAD0000u;

// DOL section names by SEL section id; null = unused ids.
constexpr const char* kDolSectionNames[14] = {nullptr, ".init", ".text", ".ctors", ".dtors", ".rodata", ".data",
                                              ".bss", ".sdata", ".sdata2", nullptr, ".sbss", ".sbss2", nullptr};
struct DolBase {
    const char* name;
    uint32_t base;
};
constexpr DolBase kDolBases[] = {{".init", 0x80004000}, {".text", 0x8001F240}, {".ctors", 0x802D5100}, {".dtors", 0x802D5240},
                                 {".rodata", 0x802D5280}, {".data", 0x802F5260}, {".bss", 0x80334680}, {".sdata", 0x804AF700},
                                 {".sdata2", 0x804B2920}, {".sbss", 0x804B1960}, {".sbss2", 0x804B67E0}};

struct RsoSection {
    uint32_t off = 0;
    bool exec = false;
    uint32_t size = 0;
    uint32_t addr = 0;
};
struct Reloc {
    uint32_t off, idt, tgt;
};
struct ExportSym {
    std::string name;
    uint32_t section, offset;
};
struct Rso {
    const std::vector<uint8_t>* data = nullptr;
    std::vector<RsoSection> secs;
    std::vector<ExportSym> exports;
    std::vector<std::string> imports;
    std::vector<Reloc> irel, erel;
};

uint32_t U32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size()) throw std::runtime_error("read past end of file");
    return Be32(&d[o]);
}

std::string CStr(const std::vector<uint8_t>& d, size_t o) {
    if (o >= d.size()) throw std::runtime_error("string offset past end of file");
    const auto* end = static_cast<const uint8_t*>(std::memchr(&d[o], 0, d.size() - o));
    if (!end) throw std::runtime_error("unterminated string");
    return std::string(reinterpret_cast<const char*>(&d[o]), reinterpret_cast<const char*>(end));
}

Rso ParseRso(const std::vector<uint8_t>& d) {
    Rso m;
    m.data = &d;
    if (d.size() < 0x58) throw std::runtime_error("file too small for an RSO header");
    const uint32_t nsec = U32(d, 8), secOff = U32(d, 12), version = U32(d, 24);
    const uint32_t irelOff = U32(d, 0x30), irelSz = U32(d, 0x34), erelOff = U32(d, 0x38), erelSz = U32(d, 0x3C);
    const uint32_t exOff = U32(d, 0x40), exSz = U32(d, 0x44), exNm = U32(d, 0x48);
    const uint32_t imOff = U32(d, 0x4C), imSz = U32(d, 0x50), imNm = U32(d, 0x54);
    if (version != 1) throw std::runtime_error("unsupported RSO version");
    if (nsec > 4096) throw std::runtime_error("implausible section count");
    for (uint32_t i = 0; i < nsec; ++i) {
        RsoSection s;
        const uint32_t o = U32(d, secOff + 8 * size_t(i));
        s.size = U32(d, secOff + 8 * size_t(i) + 4);
        s.off = o & ~1u;
        s.exec = (o & 1) != 0;
        m.secs.push_back(s);
    }
    for (uint32_t i = 0; i < exSz / 16; ++i) {
        const size_t at = exOff + 16 * size_t(i);
        m.exports.push_back({CStr(d, exNm + size_t(U32(d, at))), U32(d, at + 8), U32(d, at + 4)});
    }
    for (uint32_t i = 0; i < imSz / 12; ++i) m.imports.push_back(CStr(d, imNm + size_t(U32(d, imOff + 12 * size_t(i)))));
    auto relocs = [&](uint32_t off, uint32_t size, std::vector<Reloc>& out) {
        for (uint32_t i = 0; i < size / 12; ++i) {
            const size_t at = off + 12 * size_t(i);
            out.push_back({U32(d, at), U32(d, at + 4), U32(d, at + 8)});
        }
    };
    relocs(irelOff, irelSz, m.irel);
    relocs(erelOff, erelSz, m.erel);
    return m;
}

}  // namespace

bool BuildModuleImage(const std::vector<uint8_t>& rsoBytes, const std::vector<uint8_t>& selBytes, uint32_t base,
                      std::vector<uint8_t>& img, std::string& error, int* unresolvedOut) {
    try {
        Rso m = ParseRso(rsoBytes);
        const Rso sel = ParseRso(selBytes);
        std::map<std::string, uint32_t> selSyms;
        for (const ExportSym& e : sel.exports) {
            uint32_t a = 0;
            if (e.section == kAbs) {
                a = e.offset;
            } else if (e.section == kEti) {
                a = kEtiBase + e.offset;
            } else {
                if (e.section >= 14 || !kDolSectionNames[e.section]) continue;
                const char* sn = kDolSectionNames[e.section];
                bool found = false;
                for (const DolBase& b : kDolBases) {
                    if (std::strcmp(b.name, sn) == 0) {
                        a = b.base + e.offset;
                        found = true;
                    }
                }
                if (!found) continue;
            }
            selSyms[e.name] = a;
        }

        // Layout: file-backed sections first, then bss; 32-byte aligned.
        uint32_t addr = base;
        std::map<uint32_t, uint32_t> secAddr;
        for (uint32_t i = 0; i < m.secs.size(); ++i) {
            RsoSection& s = m.secs[i];
            if (s.size && s.off) {
                addr = (addr + 31) & ~31u;
                secAddr[i] = s.addr = addr;
                addr += s.size;
            }
        }
        for (uint32_t i = 0; i < m.secs.size(); ++i) {
            RsoSection& s = m.secs[i];
            if (s.size && !s.off) {
                addr = (addr + 31) & ~31u;
                secAddr[i] = s.addr = addr;
                addr += s.size;
            }
        }
        const uint32_t end = addr;
        img.assign(end - base, 0);
        for (const RsoSection& s : m.secs) {
            if (s.size && s.off) {
                if (uint64_t(s.off) + s.size > rsoBytes.size()) throw std::runtime_error("section extends past end of file");
                std::memcpy(&img[s.addr - base], &rsoBytes[s.off], s.size);
            }
        }

        auto locate = [&](uint32_t fo) -> uint32_t {
            for (const RsoSection& s : m.secs) {
                if (s.size && s.off && s.off <= fo && fo < s.off + s.size) return s.addr + fo - s.off;
            }
            throw std::runtime_error("relocation outside sections");
        };

        std::vector<std::pair<uint32_t, uint32_t>> stubs;  // (target, stub address), insertion order
        const uint32_t stubBase = (end + 31) & ~31u;
        auto putU32 = [&](size_t o, uint32_t v) {
            img[o] = uint8_t(v >> 24);
            img[o + 1] = uint8_t(v >> 16);
            img[o + 2] = uint8_t(v >> 8);
            img[o + 3] = uint8_t(v);
        };
        auto farStub = [&](uint32_t target) -> uint32_t {
            for (const auto& s : stubs) {
                if (s.first == target) return s.second;
            }
            const uint32_t at = stubBase + 16 * uint32_t(stubs.size());
            stubs.emplace_back(target, at);
            const size_t need = size_t(at) + 16 - base;
            if (img.size() < need) img.resize(need, 0);
            putU32(at - base, 0x3C000000u | ((target >> 16) & 0xFFFF));
            putU32(at - base + 4, 0x60000000u | (target & 0xFFFF));
            putU32(at - base + 8, 0x7C0903A6u);
            putU32(at - base + 12, 0x4E800420u);
            return at;
        };
        auto apply = [&](uint32_t loc, uint32_t valIn, uint32_t ty) {
            const size_t o = loc - base;
            int64_t val = valIn;
            if (ty == R_REL24 && !(-0x2000000ll <= val - int64_t(loc) && val - int64_t(loc) < 0x2000000ll)) {
                val = farStub(static_cast<uint32_t>(val));
            }
            auto put16 = [&](uint32_t v) {
                img[o] = uint8_t(v >> 8);
                img[o + 1] = uint8_t(v);
            };
            const uint32_t delta = static_cast<uint32_t>(val - int64_t(loc));
            switch (ty) {
            case R_ADDR32:
            case R_UADDR32: putU32(o, static_cast<uint32_t>(val)); break;
            case R_ADDR16_LO: put16(static_cast<uint32_t>(val) & 0xFFFF); break;
            case R_ADDR16_HI: put16((static_cast<uint32_t>(val) >> 16) & 0xFFFF); break;
            case R_ADDR16_HA: put16(static_cast<uint32_t>((val + 0x8000) >> 16) & 0xFFFF); break;
            case R_REL24: putU32(o, (Be32(&img[o]) & 0xFC000003u) | (delta & 0x3FFFFFCu)); break;
            case R_REL14: putU32(o, (Be32(&img[o]) & 0xFFFF0003u) | (delta & 0xFFFCu)); break;
            default: throw std::runtime_error("unhandled relocation type " + std::to_string(ty));
            }
        };

        std::map<std::string, bool> unresolved;
        for (const Reloc& r : m.irel) {
            const uint32_t ty = r.idt & 0xFF;
            if (ty == R_NONE) continue;
            const uint32_t sid = r.idt >> 8;
            const auto it = secAddr.find(sid);
            if (it == secAddr.end()) {
                apply(locate(r.off), kPoison + r.tgt, ty);
                continue;
            }
            apply(locate(r.off), it->second + r.tgt, ty);
        }
        for (const Reloc& r : m.erel) {
            const uint32_t ty = r.idt & 0xFF;
            if (ty == R_NONE) continue;
            const uint32_t index = r.idt >> 8;
            if (index >= m.imports.size()) throw std::runtime_error("import index out of range");
            const std::string& name = m.imports[index];
            const auto it = selSyms.find(name);
            if (it == selSyms.end()) {
                unresolved[name] = true;
                continue;
            }
            apply(locate(r.off), it->second, ty);
        }
        if (unresolvedOut) *unresolvedOut = static_cast<int>(unresolved.size());
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace DiscImport

#else  // !_WIN32: the importer relies on Windows CNG; other platforms keep the manual dvd_root workflow.

namespace DiscImport {
void Progress::SetStage(std::string, std::string) {}
void Progress::Get(std::string&, std::string&) const {}
Result ImportDisc(const std::filesystem::path&, const std::filesystem::path&, Progress&, Summary&, std::string& error,
                  const std::filesystem::path&) {
    error = "Disc import is only available on Windows.";
    return Result::Failed;
}
bool IsGameDirectoryComplete(const std::filesystem::path&) { return false; }
bool BuildModuleImage(const std::vector<uint8_t>&, const std::vector<uint8_t>&, uint32_t, std::vector<uint8_t>&, std::string& error, int*) {
    error = "unsupported platform";
    return false;
}
bool IsSupportedDiscExtension(const std::filesystem::path&) { return false; }
}  // namespace DiscImport

#endif
