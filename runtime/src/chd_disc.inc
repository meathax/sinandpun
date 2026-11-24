// CHD v5 reader for disc_import.cpp (DVD images made with `chdman createdvd`).
// Layout and codecs follow MAME src/lib/util/chd.cpp, chdcodec.cpp, huffman.cpp
// and flac.cpp. Every decompressed hunk is checked against its map CRC-16.

class ChdBits {
public:
    ChdBits(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
    uint32_t Peek(int bits) {
        while (m_count < bits) {
            m_buffer = (m_buffer << 8) | (m_at < m_size ? m_data[m_at] : 0);
            ++m_at;
            m_count += 8;
        }
        return bits ? static_cast<uint32_t>(m_buffer >> (m_count - bits)) & ((1u << bits) - 1u) : 0;
    }
    void Remove(int bits) { m_count -= bits; }
    uint32_t Read(int bits) {
        if (bits > 24) {
            const uint32_t high = Read(bits - 16);
            return (high << 16) | Read(16);
        }
        const uint32_t value = Peek(bits);
        Remove(bits);
        return value;
    }
    bool Overflow() const { return m_at * 8 - static_cast<size_t>(m_count) > m_size * 8; }
    void AlignByte() { Remove(m_count % 8); }

private:
    const uint8_t* m_data;
    size_t m_size, m_at = 0;
    uint64_t m_buffer = 0;
    int m_count = 0;
};

// Canonical Huffman decoder matching MAME's huffman_decoder<numcodes, maxbits>.
class ChdHuffman {
public:
    ChdHuffman(int codes, int maxBits) : m_codes(codes), m_maxBits(maxBits), m_lengths(codes), m_lookup(size_t(1) << maxBits) {}

    bool ImportRle(ChdBits& bits) {
        const int width = m_maxBits >= 16 ? 5 : m_maxBits >= 8 ? 4 : 3;
        for (int node = 0; node < m_codes;) {
            int length = static_cast<int>(bits.Read(width));
            if (length != 1) {
                m_lengths[node++] = static_cast<uint8_t>(length);
                continue;
            }
            length = static_cast<int>(bits.Read(width));
            if (length == 1) {
                m_lengths[node++] = 1;
                continue;
            }
            int repeat = static_cast<int>(bits.Read(width)) + 3;
            if (node + repeat > m_codes) return false;
            while (repeat--) m_lengths[node++] = static_cast<uint8_t>(length);
        }
        return Build() && !bits.Overflow();
    }

    bool ImportHuffman(ChdBits& bits) {
        ChdHuffman small(24, 6);
        small.m_lengths[0] = static_cast<uint8_t>(bits.Read(3));
        const int start = static_cast<int>(bits.Read(3)) + 1;
        int count = 0;
        for (int index = 1; index < 24; ++index) {
            if (index < start || count == 7) {
                small.m_lengths[index] = 0;
            } else {
                count = static_cast<int>(bits.Read(3));
                small.m_lengths[index] = static_cast<uint8_t>(count == 7 ? 0 : count);
            }
        }
        if (!small.Build()) return false;
        int rleBits = 0;
        for (uint32_t temp = uint32_t(m_codes - 9); temp; temp >>= 1) ++rleBits;
        int last = 0, code = 0;
        while (code < m_codes) {
            const int value = small.Decode(bits);
            if (value != 0) {
                m_lengths[code++] = static_cast<uint8_t>(last = value - 1);
            } else {
                int repeat = static_cast<int>(bits.Read(3)) + 2;
                if (repeat == 7 + 2) repeat += static_cast<int>(bits.Read(rleBits));
                for (; repeat && code < m_codes; --repeat) m_lengths[code++] = static_cast<uint8_t>(last);
            }
        }
        return code == m_codes && Build() && !bits.Overflow();
    }

    int Decode(ChdBits& bits) const {
        const uint32_t entry = m_lookup[bits.Peek(m_maxBits)];
        bits.Remove(static_cast<int>(entry & 0x1f));
        return static_cast<int>(entry >> 5);
    }

private:
    bool Build() {
        uint32_t histogram[33] = {};
        for (int i = 0; i < m_codes; ++i) {
            if (m_lengths[i] > m_maxBits) return false;
            ++histogram[m_lengths[i]];
        }
        uint32_t start = 0;
        for (int length = 32; length > 0; --length) {
            const uint32_t next = (start + histogram[length]) >> 1;
            if (length != 1 && next * 2 != start + histogram[length]) return false;
            histogram[length] = start;
            start = next;
        }
        std::fill(m_lookup.begin(), m_lookup.end(), 0u);
        for (int i = 0; i < m_codes; ++i) {
            const int length = m_lengths[i];
            if (!length) continue;
            const uint32_t code = histogram[length]++;
            const int shift = m_maxBits - length;
            const uint32_t entry = (uint32_t(i) << 5) | uint32_t(length);
            const size_t first = size_t(code) << shift, count = size_t(1) << shift;
            if (first + count > m_lookup.size()) return false;
            std::fill_n(m_lookup.begin() + first, count, entry);
        }
        return true;
    }

    int m_codes, m_maxBits;
    std::vector<uint8_t> m_lengths;
    std::vector<uint32_t> m_lookup;
};

// Minimal FLAC frame decoder for MAME's CHD "flac" codec: 16-bit stereo frames,
// no stream header, block size derived from the hunk size.
class ChdFlac {
public:
    // Decodes `samples` stereo frames into interleaved int16 values.
    static bool Decode(const uint8_t* data, size_t size, uint32_t samples, uint32_t blockSize, std::vector<int32_t>& out) {
        out.assign(size_t(samples) * 2, 0);
        ChdBits bits(data, size);
        std::vector<int32_t> channel[2] = {std::vector<int32_t>(65536), std::vector<int32_t>(65536)};
        uint32_t produced = 0;
        while (produced < samples) {
            if (bits.Read(15) != 0x7FFC) return false;  // sync (14 bits) + reserved 0
            bits.Read(1);                                // blocking strategy
            const uint32_t sizeCode = bits.Read(4), rateCode = bits.Read(4), assignment = bits.Read(4), depthCode = bits.Read(3);
            bits.Read(1);
            // UTF-8 style frame/sample number.
            uint32_t lead = bits.Read(8), extra = 0;
            if (lead >= 0xC0) {
                for (uint32_t mask = 0x40; lead & mask; mask >>= 1) ++extra;
                if (extra > 6) return false;
            }
            for (uint32_t i = 0; i < extra; ++i) bits.Read(8);
            uint32_t block = blockSize;
            if (sizeCode == 1) block = 192;
            else if (sizeCode >= 2 && sizeCode <= 5) block = 576u << (sizeCode - 2);
            else if (sizeCode == 6) block = bits.Read(8) + 1;
            else if (sizeCode == 7) block = bits.Read(16) + 1;
            else if (sizeCode >= 8) block = 256u << (sizeCode - 8);
            if (rateCode == 12) bits.Read(8);
            else if (rateCode == 13 || rateCode == 14) bits.Read(16);
            else if (rateCode == 15) return false;
            bits.Read(8);  // header CRC-8
            const int depth = depthCode == 0 ? 16 : depthCode == 1 ? 8 : depthCode == 2 ? 12 : depthCode == 4 ? 16 :
                              depthCode == 5 ? 20 : depthCode == 6 ? 24 : 0;
            if (!depth || block == 0 || block > 65535 || (assignment != 1 && (assignment < 8 || assignment > 10))) return false;
            for (int ch = 0; ch < 2; ++ch) {
                const bool side = (assignment == 8 && ch == 1) || (assignment == 9 && ch == 0) || (assignment == 10 && ch == 1);
                if (!Subframe(bits, channel[ch].data(), block, depth + (side ? 1 : 0))) return false;
            }
            int32_t* l = channel[0].data();
            int32_t* r = channel[1].data();
            for (uint32_t i = 0; i < block; ++i) {
                if (assignment == 8) r[i] = l[i] - r[i];
                else if (assignment == 9) l[i] += r[i];
                else if (assignment == 10) {
                    int32_t mid = l[i] * 2 | (r[i] & 1);
                    l[i] = (mid + r[i]) >> 1;
                    r[i] = (mid - r[i]) >> 1;
                }
            }
            bits.AlignByte();
            bits.Read(16);  // frame CRC-16
            const uint32_t n = std::min(block, samples - produced);
            for (uint32_t i = 0; i < n; ++i) {
                out[size_t(produced + i) * 2] = l[i];
                out[size_t(produced + i) * 2 + 1] = r[i];
            }
            produced += n;
            if (bits.Overflow()) return false;
        }
        return true;
    }

private:
    static int32_t Signed(uint32_t value, int bits) {
        return bits >= 32 ? int32_t(value) : int32_t(value << (32 - bits)) >> (32 - bits);
    }
    static bool Residual(ChdBits& bits, int32_t* out, uint32_t block, uint32_t order) {
        const uint32_t method = bits.Read(2);
        if (method > 1) return false;
        const int paramBits = method == 0 ? 4 : 5;
        const uint32_t escape = method == 0 ? 15 : 31;
        const uint32_t partitionOrder = bits.Read(4);
        const uint32_t partitions = 1u << partitionOrder;
        if ((block >> partitionOrder) < order || (block & (partitions - 1))) return false;
        uint32_t at = order;
        for (uint32_t p = 0; p < partitions; ++p) {
            const uint32_t count = (block >> partitionOrder) - (p == 0 ? order : 0);
            const uint32_t param = bits.Read(paramBits);
            if (param == escape) {
                const int raw = static_cast<int>(bits.Read(5));
                for (uint32_t i = 0; i < count; ++i) out[at++] = raw ? Signed(bits.Read(raw), raw) : 0;
                continue;
            }
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t quotient = 0;
                while (bits.Read(1) == 0) {
                    if (++quotient > (1u << 20) || bits.Overflow()) return false;
                }
                const uint32_t value = (quotient << param) | (param ? bits.Read(static_cast<int>(param)) : 0);
                out[at++] = int32_t(value >> 1) ^ -int32_t(value & 1);
            }
        }
        return true;
    }
    static bool Subframe(ChdBits& bits, int32_t* out, uint32_t block, int depth) {
        if (bits.Read(1)) return false;
        const uint32_t type = bits.Read(6);
        int wasted = 0;
        if (bits.Read(1)) {
            wasted = 1;
            while (bits.Read(1) == 0) {
                if (++wasted >= depth) return false;
            }
        }
        depth -= wasted;
        if (type == 0) {
            const int32_t value = Signed(bits.Read(depth), depth);
            std::fill_n(out, block, value);
        } else if (type == 1) {
            for (uint32_t i = 0; i < block; ++i) out[i] = Signed(bits.Read(depth), depth);
        } else if (type >= 8 && type <= 12) {
            const uint32_t order = type - 8;
            if (order > block) return false;
            for (uint32_t i = 0; i < order; ++i) out[i] = Signed(bits.Read(depth), depth);
            if (!Residual(bits, out, block, order)) return false;
            for (uint32_t i = order; i < block; ++i) {
                switch (order) {
                case 1: out[i] += out[i - 1]; break;
                case 2: out[i] += 2 * out[i - 1] - out[i - 2]; break;
                case 3: out[i] += 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]; break;
                case 4: out[i] += 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4]; break;
                default: break;
                }
            }
        } else if (type >= 32) {
            const uint32_t order = (type & 31) + 1;
            if (order > block) return false;
            for (uint32_t i = 0; i < order; ++i) out[i] = Signed(bits.Read(depth), depth);
            const int precision = static_cast<int>(bits.Read(4)) + 1;
            if (precision == 16) return false;
            const int shift = Signed(bits.Read(5), 5);
            if (shift < 0) return false;
            int32_t coefficients[32];
            for (uint32_t i = 0; i < order; ++i) coefficients[i] = Signed(bits.Read(precision), precision);
            if (!Residual(bits, out, block, order)) return false;
            for (uint32_t i = order; i < block; ++i) {
                int64_t sum = 0;
                for (uint32_t j = 0; j < order; ++j) sum += int64_t(coefficients[j]) * out[i - 1 - j];
                out[i] += static_cast<int32_t>(sum >> shift);
            }
        } else {
            return false;
        }
        if (wasted)
            for (uint32_t i = 0; i < block; ++i) out[i] = static_cast<int32_t>(static_cast<uint32_t>(out[i]) << wasted);
        return !bits.Overflow();
    }
};

uint16_t ChdCrc16(const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::array<uint16_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint16_t crc = static_cast<uint16_t>(i << 8);
            for (int b = 0; b < 8; ++b) crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
            t[i] = crc;
        }
        return t;
    }();
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) crc = static_cast<uint16_t>((crc << 8) ^ table[((crc >> 8) ^ data[i]) & 0xFF]);
    return crc;
}

class ChdImage final : public RawImage {
    enum : uint8_t { kType0 = 0, kType3 = 3, kNone = 4, kSelf = 5, kParent = 6, kRleSmall = 7, kRleLarge = 8,
                     kSelf0 = 9, kSelf1 = 10, kParentSelf = 11, kParent0 = 12, kParent1 = 13 };
    struct Entry {
        uint8_t type;
        uint32_t length;
        uint64_t offset;
        uint16_t crc;
    };
    FileStream file;
    uint64_t logical = 0;
    uint32_t hunkBytes = 0, unitBytes = 0, compressors[4] = {};
    std::vector<Entry> map;
    Bytes hunk, input;
    uint64_t cached = UINT64_MAX;

    static void Check(bool ok, const char* what = "The CHD file is damaged or uses an unsupported layout.") {
        if (!ok) throw ImportError(what);
    }

    void LoadCompressedMap(uint64_t mapOffset, uint64_t hunks) {
        uint8_t head[16];
        Check(file.Read(mapOffset, head, sizeof head));
        const uint32_t mapBytes = Be32(head);
        const uint64_t firstOffset = uint64_t(Be16(head + 4)) << 32 | Be32(head + 6);
        const uint16_t mapCrc = Be16(head + 10);
        const int lengthBits = head[12], selfBits = head[13], parentBits = head[14];
        Check(lengthBits <= 32 && selfBits <= 48 && parentBits <= 48);
        Bytes packed = ReadBytes(file, mapOffset + 16, mapBytes);
        ChdBits bits(packed.data(), packed.size());
        ChdHuffman types(16, 8);
        Check(types.ImportRle(bits));
        map.resize(static_cast<size_t>(hunks));
        uint8_t last = 0;
        uint32_t repeat = 0;
        for (auto& entry : map) {
            if (repeat) {
                entry.type = last;
                --repeat;
                continue;
            }
            const int value = types.Decode(bits);
            if (value == kRleSmall) {
                entry.type = last;
                repeat = 2 + static_cast<uint32_t>(types.Decode(bits));
            } else if (value == kRleLarge) {
                entry.type = last;
                repeat = 2 + 16 + (static_cast<uint32_t>(types.Decode(bits)) << 4);
                repeat += static_cast<uint32_t>(types.Decode(bits));
            } else {
                entry.type = last = static_cast<uint8_t>(value);
            }
        }
        uint64_t current = firstOffset, lastSelf = 0, lastParent = 0;
        auto readWide = [&](int width) -> uint64_t {
            return width > 32 ? (uint64_t(bits.Read(width - 32)) << 32) | bits.Read(32) : bits.Read(width);
        };
        for (size_t index = 0; index < map.size(); ++index) {
            Entry& entry = map[index];
            entry.offset = current;
            entry.length = 0;
            entry.crc = 0;
            switch (entry.type) {
            case kType0: case 1: case 2: case kType3:
                entry.length = bits.Read(lengthBits);
                current += entry.length;
                entry.crc = static_cast<uint16_t>(bits.Read(16));
                break;
            case kNone:
                entry.length = hunkBytes;
                current += hunkBytes;
                entry.crc = static_cast<uint16_t>(bits.Read(16));
                break;
            case kSelf:
                lastSelf = entry.offset = readWide(selfBits);
                break;
            case kParent:
                entry.offset = lastParent = readWide(parentBits);
                break;
            case kSelf1:
                ++lastSelf;
                [[fallthrough]];
            case kSelf0:
                entry.type = kSelf;
                entry.offset = lastSelf;
                break;
            case kParentSelf:
                entry.type = kParent;
                lastParent = entry.offset = uint64_t(index) * hunkBytes / unitBytes;
                break;
            case kParent1:
                lastParent += hunkBytes / unitBytes;
                [[fallthrough]];
            case kParent0:
                entry.type = kParent;
                entry.offset = lastParent;
                break;
            default:
                Check(false);
            }
        }
        Check(!bits.Overflow());
        // The stored CRC covers MAME's 12-byte raw map entries.
        Bytes raw(map.size() * 12);
        for (size_t i = 0; i < map.size(); ++i) {
            uint8_t* p = raw.data() + i * 12;
            p[0] = map[i].type;
            p[1] = uint8_t(map[i].length >> 16); p[2] = uint8_t(map[i].length >> 8); p[3] = uint8_t(map[i].length);
            for (int b = 0; b < 6; ++b) p[4 + b] = uint8_t(map[i].offset >> (40 - 8 * b));
            p[10] = uint8_t(map[i].crc >> 8); p[11] = uint8_t(map[i].crc);
        }
        Check(ChdCrc16(raw.data(), raw.size()) == mapCrc, "The CHD file's hunk map is damaged.");
    }

    void Decompress(uint32_t codec, const Bytes& in, uint8_t* out) {
        constexpr uint32_t kZlib = 0x7a6c6962, kLzma = 0x6c7a6d61, kHuff = 0x68756666, kFlac = 0x666c6163, kZstd = 0x7a737464;
        if (codec == kZlib) {
            z_stream z{};
            Check(inflateInit2(&z, -MAX_WBITS) == Z_OK);
            z.next_in = const_cast<Bytef*>(in.data());
            z.avail_in = static_cast<uInt>(in.size());
            z.next_out = out;
            z.avail_out = hunkBytes;
            const int status = inflate(&z, Z_FINISH);
            const bool ok = (status == Z_STREAM_END || status == Z_OK || status == Z_BUF_ERROR) && z.avail_out == 0;
            inflateEnd(&z);
            Check(ok);
        } else if (codec == kLzma) {
            // MAME writes a headerless LZMA stream without an end marker, encoded
            // at level 9 with the dictionary reduced to the hunk size.
            uint32_t dictionary = 1u << 26;
            for (int i = 11; i <= 30; ++i) {
                if (hunkBytes <= (2u << i)) { dictionary = 2u << i; break; }
                if (hunkBytes <= (3u << i)) { dictionary = 3u << i; break; }
            }
            Bytes alone(13 + in.size());
            alone[0] = 0x5D;  // lc=3, lp=0, pb=2
            for (int b = 0; b < 4; ++b) alone[1 + b] = uint8_t(dictionary >> (8 * b));
            for (int b = 0; b < 8; ++b) alone[5 + b] = uint8_t(uint64_t(hunkBytes) >> (8 * b));
            std::memcpy(alone.data() + 13, in.data(), in.size());
            lzma_stream stream = LZMA_STREAM_INIT;
            Check(lzma_alone_decoder(&stream, 256ull << 20) == LZMA_OK);
            stream.next_in = alone.data();
            stream.avail_in = alone.size();
            stream.next_out = out;
            stream.avail_out = hunkBytes;
            lzma_ret status = LZMA_OK;
            while (status == LZMA_OK && stream.avail_out) status = lzma_code(&stream, LZMA_FINISH);
            const bool ok = stream.avail_out == 0 && (status == LZMA_OK || status == LZMA_STREAM_END);
            lzma_end(&stream);
            Check(ok);
        } else if (codec == kHuff) {
            ChdBits bits(in.data(), in.size());
            ChdHuffman decoder(256, 16);
            Check(decoder.ImportHuffman(bits));
            for (uint32_t i = 0; i < hunkBytes; ++i) out[i] = static_cast<uint8_t>(decoder.Decode(bits));
            Check(!bits.Overflow());
        } else if (codec == kFlac) {
            Check(!in.empty() && (in[0] == 'L' || in[0] == 'B') && hunkBytes % 4 == 0);
            uint32_t block = hunkBytes / 4;
            while (block > 2048) block /= 2;
            std::vector<int32_t> samples;
            Check(ChdFlac::Decode(in.data() + 1, in.size() - 1, hunkBytes / 4, block, samples));
            const bool little = in[0] == 'L';
            for (size_t i = 0; i < samples.size(); ++i) {
                const uint16_t v = static_cast<uint16_t>(samples[i]);
                out[i * 2] = little ? uint8_t(v) : uint8_t(v >> 8);
                out[i * 2 + 1] = little ? uint8_t(v >> 8) : uint8_t(v);
            }
        } else if (codec == kZstd) {
            const size_t n = ZSTD_decompress(out, hunkBytes, in.data(), in.size());
            Check(!ZSTD_isError(n) && n == hunkBytes);
        } else {
            char tag[5] = {char(codec >> 24), char(codec >> 16), char(codec >> 8), char(codec), 0};
            throw ImportError(std::string("This CHD uses the \"") + tag +
                              "\" codec, which is for CDs or video. Re-create it with: chdman createdvd -i game.iso -o game.chd");
        }
    }

    void LoadHunk(uint64_t index, uint8_t* out, int depth = 0) {
        Check(index < map.size() && depth < 8);
        const Entry& entry = map[static_cast<size_t>(index)];
        if (entry.type == kSelf) return LoadHunk(entry.offset, out, depth + 1);
        if (entry.type == kParent) throw ImportError("This CHD needs a parent CHD file. Use a standalone CHD or a plain ISO.");
        if (compressors[0] == 0) {  // uncompressed map: offsets in hunks, 0 = zero hunk
            if (!entry.offset) { std::memset(out, 0, hunkBytes); return; }
            Check(file.Read(entry.offset * hunkBytes, out, hunkBytes));
            return;
        }
        if (entry.type == kNone) {
            Check(entry.length == hunkBytes && file.Read(entry.offset, out, hunkBytes));
        } else {
            Check(entry.length <= hunkBytes * 2u + 1024u);
            input.resize(entry.length);
            Check(file.Read(entry.offset, input.data(), input.size()));
            const uint32_t codec = compressors[entry.type];
            Check(codec != 0);
            Decompress(codec, input, out);
        }
        if (ChdCrc16(out, hunkBytes) != entry.crc) {
            char text[160];
            std::snprintf(text, sizeof text, "The CHD file is damaged: hunk %llu failed its integrity check.", static_cast<unsigned long long>(index));
            throw ImportError(text);
        }
    }

public:
    explicit ChdImage(const fs::path& path) : file(path) {
        uint8_t head[124];
        Check(file.Read(0, head, sizeof head) && std::memcmp(head, "MComprHD", 8) == 0, "Not a CHD file.");
        const uint32_t version = Be32(head + 12);
        if (version != 5) throw ImportError("This CHD uses an old format version. Re-create it with a current chdman (createdvd).");
        for (int i = 0; i < 4; ++i) compressors[i] = Be32(head + 16 + 4 * i);
        logical = uint64_t(Be32(head + 0x20)) << 32 | Be32(head + 0x24);
        const uint64_t mapOffset = uint64_t(Be32(head + 0x28)) << 32 | Be32(head + 0x2C);
        hunkBytes = Be32(head + 0x38);
        unitBytes = Be32(head + 0x3C);
        Check(hunkBytes >= 512 && hunkBytes <= (1u << 20) && unitBytes && hunkBytes % unitBytes == 0 && logical &&
              logical <= (16ull << 30));
        static const uint8_t zeroSha[20] = {};
        if (std::memcmp(head + 0x68, zeroSha, 20) != 0)
            throw ImportError("This CHD is a delta that needs its parent CHD. Use a standalone CHD or a plain ISO.");
        const uint64_t hunks = (logical + hunkBytes - 1) / hunkBytes;
        if (compressors[0] == 0) {
            Bytes raw = ReadBytes(file, mapOffset, hunks * 4);
            map.resize(static_cast<size_t>(hunks));
            for (size_t i = 0; i < map.size(); ++i) map[i] = {kNone, hunkBytes, Be32(raw.data() + i * 4), 0};
        } else {
            LoadCompressedMap(mapOffset, hunks);
        }
        hunk.resize(hunkBytes);
    }
    uint64_t Size() const override { return logical; }
    bool Read(uint64_t off, void* dst, size_t size) override {
        auto* out = static_cast<uint8_t*>(dst);
        while (size) {
            // Some chdman versions round the logical size down to whole hunks;
            // the trimmed tail of a Wii disc is unused padding.
            if (off >= logical) {
                if (off >= 0x230480000ull) return false;
                std::memset(out, 0, size);
                return true;
            }
            const uint64_t index = off / hunkBytes;
            if (index != cached) {
                LoadHunk(index, hunk.data());
                cached = index;
            }
            const size_t within = static_cast<size_t>(off % hunkBytes);
            const size_t n = static_cast<size_t>(std::min<uint64_t>({size, hunkBytes - within, logical - off}));
            std::memcpy(out, hunk.data() + within, n);
            off += n;
            out += n;
            size -= n;
        }
        return true;
    }
};
