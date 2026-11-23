// Internal readers for disc_import.cpp. Container layouts follow Dolphin's
// docs/WiaAndRvz.md and DiscIO/CompressedBlob.h. All offsets below are bytes.
using Bytes = std::vector<uint8_t>;

void CheckContainer(bool ok) {
    if (!ok) throw ImportError("The compressed disc image is damaged or uses an unsupported format.");
}

Bytes ReadBytes(FileStream& file, uint64_t off, uint64_t size) {
    CheckContainer(size <= (128u << 20) && off <= file.Size() && size <= file.Size() - off);
    Bytes bytes(size);
    CheckContainer(file.Read(off, bytes.data(), bytes.size()));
    return bytes;
}

uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

// GCZ metadata is little-endian, as are all supported Windows targets.
class GczImage final : public RawImage {
    FileStream file;
    uint64_t discSize, storedSize, dataStart;
    uint32_t blockSize;
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> hashes;
    Bytes block;
    uint32_t cached = UINT32_MAX;
public:
    explicit GczImage(const fs::path& path) : file(path) {
        uint32_t magic, count;
        CheckContainer(file.Read(0, &magic, 4) && magic == 0xb10bc001 &&
            file.Read(8, &storedSize, 8) && file.Read(16, &discSize, 8) &&
            file.Read(24, &blockSize, 4) && file.Read(28, &count, 4));
        dataStart = 32 + uint64_t(count) * 12;
        CheckContainer(blockSize && blockSize <= (128u << 20) && discSize &&
            count && count <= (8u << 20) && (discSize - 1) / blockSize == count - 1 &&
            dataStart <= file.Size() && storedSize <= file.Size() - dataStart);
        offsets.resize(count + 1);
        hashes.resize(count);
        CheckContainer(file.Read(32, offsets.data(), uint64_t(count) * 8) &&
            file.Read(32 + uint64_t(count) * 8, hashes.data(), uint64_t(count) * 4));
        offsets.back() = storedSize;
        block.resize(blockSize);
    }
    uint64_t Size() const override { return discSize; }
    bool Read(uint64_t off, void* dst, size_t size) override {
        if (off > discSize || size > discSize - off) return false;
        auto* out = static_cast<uint8_t*>(dst);
        while (size) {
            const uint32_t index = off / blockSize;
            if (cached != index) {
                constexpr uint64_t flag = uint64_t(1) << 63;
                const auto start = offsets[index] & ~flag, end = offsets[index + 1] & ~flag;
                CheckContainer(start < end && end <= storedSize && end - start <= blockSize);
                auto input = ReadBytes(file, dataStart + start, end - start);
                CheckContainer(adler32(1, input.data(), input.size()) == hashes[index]);
                if (offsets[index] & flag) {
                    CheckContainer(input.size() == blockSize);
                    block.swap(input);
                } else {
                    unsigned long n = blockSize;
                    CheckContainer(uncompress(block.data(), &n, input.data(), input.size()) == Z_OK && n == blockSize);
                }
                cached = index;
            }
            const size_t within = off % blockSize, n = std::min<size_t>(size, blockSize - within);
            std::memcpy(out, block.data() + within, n);
            off += n;
            out += n;
            size -= n;
        }
        return true;
    }
};

// Reproduce the disc's padding stream when an RVZ packed run refers to a seed.
void ExpandPadding(const uint8_t* seed, size_t skip, uint8_t* out, size_t size) {
    uint32_t words[521];
    for (size_t i = 0; i < 17; ++i) words[i] = Be32(seed + i * 4);
    for (size_t i = 17; i < 521; ++i)
        words[i] = (words[i-17] << 23) ^ (words[i-16] >> 9) ^ words[i-1];
    auto advance = [&] {
        for (size_t i = 0; i < 521; ++i) words[i] ^= words[(i + 489) % 521];
    };
    for (int i = 0; i < 4; ++i) advance();
    while (skip >= sizeof words) { advance(); skip -= sizeof words; }
    constexpr unsigned shifts[] = {24, 18, 8, 0};
    while (size--) {
        *out++ = uint8_t(words[skip / 4] >> shifts[skip % 4]);
        if (++skip == sizeof words) { advance(); skip = 0; }
    }
}

class RvzImage final : public RawImage {
    struct Range { uint64_t start, length; uint32_t first, count; };
    FileStream file;
    uint64_t discSize;
    uint32_t codec, chunkSize;
    Bytes header, parts, groups, block;
    std::vector<bool> badSector;
    uint32_t partStride;
    std::vector<Range> raw, partition;
    const Range* cachedRange = nullptr;
    uint32_t cachedIndex = UINT32_MAX;

    Bytes Decode(Bytes input, uint32_t method, size_t limit) const {
        CheckContainer(limit <= (128u << 20));
        if (!method) {
            CheckContainer(input.size() <= limit);
            return input;
        }
        Bytes out(std::max<size_t>(limit, 1));
        size_t written = 0;
        if (method == 5) {
            written = ZSTD_decompress(out.data(), out.size(), input.data(), input.size());
            CheckContainer(!ZSTD_isError(written));
        } else if (method == 2) {
            unsigned int n = out.size();
            CheckContainer(BZ2_bzBuffToBuffDecompress(reinterpret_cast<char*>(out.data()), &n,
                reinterpret_cast<char*>(input.data()), input.size(), 0, 0) == BZ_OK);
            written = n;
        } else {
            CheckContainer(method == 3 || method == 4);
            lzma_filter filters[] = {{method == 3 ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, nullptr},
                                     {LZMA_VLI_UNKNOWN, nullptr}};
            CheckContainer(lzma_properties_decode(filters, nullptr, header.data() + 0xd5, header[0xd4]) == LZMA_OK);
            size_t consumed = 0;
            bool ok = lzma_raw_decoder_memusage(filters) <= (256ull << 20) &&
                lzma_raw_buffer_decode(filters, nullptr, input.data(), &consumed, input.size(),
                                       out.data(), &written, out.size()) == LZMA_OK;
            lzma_filters_free(filters, nullptr);
            CheckContainer(ok && consumed == input.size());
        }
        CheckContainer(written <= limit);
        out.resize(written);
        return out;
    }
    Bytes Table(size_t field, size_t stride) {
        const auto* p = header.data() + field;
        const uint64_t length = uint64_t(Be32(p)) * stride;
        auto table = Decode(ReadBytes(file, Be64(p+4), Be32(p+12)), codec, length);
        CheckContainer(table.size() == length);
        return table;
    }
    void CheckHash(const uint8_t* data, size_t size, const uint8_t* expected) {
        uint8_t actual[20];
        Sha1().Digest(data, size, actual);
        CheckContainer(!std::memcmp(actual, expected, sizeof actual));
    }
    void AddRange(std::vector<Range>& ranges, Range range, uint64_t unit) {
        if (!range.length) return;
        CheckContainer(range.start <= UINT64_MAX - range.length &&
            (range.length - 1) / unit + 1 == range.count &&
            uint64_t(range.first) + range.count <= groups.size() / 12);
        ranges.push_back(range);
    }
    void Load(const Range& range, uint32_t relative, size_t unit, bool plain) {
        const auto* g = groups.data() + size_t(range.first + relative) * 12;
        const uint32_t flags = Be32(g+4), stored = flags & 0x7fffffff, packed = Be32(g+8);
        const uint64_t pos = uint64_t(relative) * unit;
        const size_t expected = std::min<uint64_t>(unit, range.length - pos);
        block.resize(expected);
        badSector.assign(plain ? expected / kClusterDataBytes : 0, false);
        if (!stored) {
            std::fill(block.begin(), block.end(), 0);
            return;
        }
        const unsigned lists = plain ? std::max(1u, chunkSize / 0x200000) : 0;
        const uint32_t method = flags >> 31 ? codec : 0;
        auto input = Decode(ReadBytes(file, uint64_t(Be32(g)) * 4, stored), method,
                            uint64_t(packed ? packed : expected) + lists * (3328 * 22 + 4));
        size_t i = 0;
        for (unsigned list = 0; list < lists; ++list) {
            CheckContainer(i + 2 <= input.size());
            const unsigned count = Be16(input.data() + i);
            i += 2;
            CheckContainer(count <= 3328 && i + count * 22 <= input.size());
            for (unsigned j = 0; j < count; ++j, i += 22) {
                // Writers record exceptions for H0 digests that differ from
                // the data. Do not reject unrelated sectors in the same group.
                const unsigned offset = Be16(input.data() + i);
                const size_t sector = list * 64 + offset / kClusterHashBytes;
                if (offset % kClusterHashBytes < 31 * 20 && sector < badSector.size())
                    badSector[sector] = true;
            }
        }
        if (!method) i = (i + 3) & ~size_t(3);
        CheckContainer(i <= input.size() && input.size() - i == (packed ? packed : expected));
        if (!packed) { std::memcpy(block.data(), input.data() + i, expected); return; }
        size_t produced = 0;
        while (i < input.size()) {
            CheckContainer(input.size() - i >= 4);
            const uint32_t run = Be32(input.data() + i), n = run & 0x7fffffff;
            i += 4;
            const size_t consumed = run >> 31 ? 68 : n;
            CheckContainer(n <= expected - produced && consumed <= input.size() - i);
            if (run >> 31)
                ExpandPadding(input.data() + i, (range.start + pos + produced) % 0x8000, block.data() + produced, n);
            else
                std::memcpy(block.data() + produced, input.data() + i, n);
            produced += n;
            i += consumed;
        }
        CheckContainer(produced == expected);
    }
    bool ReadRanges(const std::vector<Range>& ranges, uint64_t off, uint8_t* out, size_t size, bool plain) {
        const size_t unit = plain ? size_t(chunkSize / kClusterSize) * kClusterDataBytes : chunkSize;
        while (size) {
            auto r = std::find_if(ranges.begin(), ranges.end(), [off](const Range& range) {
                return off >= range.start && off - range.start < range.length;
            });
            if (r == ranges.end()) return false;
            const uint32_t index = (off - r->start) / unit;
            if (cachedRange != &*r || cachedIndex != index) {
                Load(*r, index, unit, plain);
                cachedRange = &*r;
                cachedIndex = index;
            }
            const size_t within = (off - r->start) % unit, n = std::min(size, block.size() - within);
            if (plain) {
                for (size_t s = within / kClusterDataBytes; s <= (within + n - 1) / kClusterDataBytes; ++s)
                    if (badSector[s]) throw ImportError("An RVZ game data sector failed its integrity check.");
            }
            std::memcpy(out, block.data() + within, n);
            off += n;
            out += n;
            size -= n;
        }
        return true;
    }
public:
    explicit RvzImage(const fs::path& path) : file(path) {
        auto first = ReadBytes(file, 0, 0x48);
        CheckContainer(!std::memcmp(first.data(), "RVZ\1", 4) && Be32(first.data()+8) <= 0x01000000 &&
                       Be64(first.data()+0x2c) == file.Size());
        CheckHash(first.data(), 0x34, first.data()+0x34);
        discSize = Be64(first.data()+0x24);
        header = ReadBytes(file, 0x48, Be32(first.data()+12));
        CheckContainer(header.size() >= 0xd5 && header[0xd4] <= 7 && 0xd5u + header[0xd4] <= header.size());
        CheckHash(header.data(), header.size(), first.data()+16);
        codec = Be32(header.data()+4);
        chunkSize = Be32(header.data()+12);
        CheckContainer(codec != 1 && codec <= 5 && chunkSize >= kClusterSize && chunkSize <= (64u << 20) &&
            (chunkSize < 0x200000 ? (chunkSize & (chunkSize - 1)) == 0 : chunkSize % 0x200000 == 0));
        partStride = Be32(header.data()+0x94);
        const uint32_t count = Be32(header.data()+0x90);
        CheckContainer(count == 0 || partStride >= 16);
        parts = ReadBytes(file, Be64(header.data()+0x98), uint64_t(count) * partStride);
        CheckHash(parts.data(), parts.size(), header.data()+0xa0);
        groups = Table(0xc4, 12);
        auto entries = Table(0xb4, 24);
        for (size_t i = 0; i < entries.size(); i += 24) {
            const auto* p = entries.data() + i;
            const uint64_t start = Be64(p), length = Be64(p+8), pad = start % kClusterSize;
            CheckContainer(start <= discSize && length <= discSize - start);
            AddRange(raw, {start-pad, length+pad, Be32(p+16), Be32(p+20)}, chunkSize);
        }
    }
    uint64_t Size() const override { return discSize; }
    bool Read(uint64_t off, void* dst, size_t size) override {
        auto* out = static_cast<uint8_t*>(dst);
        if (off < 0x80) {
            size_t n = std::min<size_t>(0x80 - off, size);
            std::memcpy(out, header.data()+16+off, n);
            off += n; out += n; size -= n;
        }
        return ReadRanges(raw, off, out, size, false);
    }
    bool SelectPartition(uint64_t start, uint64_t clusters, const uint8_t* key) override {
        for (size_t i = 0; i < parts.size(); i += partStride) {
            std::array<uint8_t, 48> entry{};
            std::memcpy(entry.data(), parts.data() + i, std::min<size_t>(partStride, entry.size()));
            uint64_t total = 0;
            std::vector<Range> ranges;
            for (unsigned j : {16, 32}) {
                const auto* p = entry.data() + j;
                const uint32_t sectors = Be32(p+4);
                if (!sectors) continue;
                if (uint64_t(Be32(p)) * kClusterSize != start + total * kClusterSize) break;
                AddRange(ranges, {total * kClusterDataBytes, uint64_t(sectors) * kClusterDataBytes,
                    Be32(p+8), Be32(p+12)}, size_t(chunkSize / kClusterSize) * kClusterDataBytes);
                total += sectors;
            }
            if (total && total == clusters) {
                if (key && std::memcmp(entry.data(), key, 16)) throw ImportError("The Wii key does not match this RVZ partition.");
                partition = std::move(ranges);
                return true;
            }
        }
        return false; // Partition may instead be present as ordinary encrypted raw data.
    }
    void ReadPartition(uint64_t off, void* dst, size_t size) override {
        CheckContainer(ReadRanges(partition, off, static_cast<uint8_t*>(dst), size, true));
    }
};
