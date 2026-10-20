#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "IsoDisc.h"

namespace ps2x::vfi
{
    // -------------------------------------------------------------------------
    // Minimal RFC 1951 Raw DEFLATE Decompressor (Mark Adler's Puff, modified)
    // -------------------------------------------------------------------------
    namespace puff_detail
    {
        constexpr int MAXBITS = 15;
        constexpr int MAXLCODES = 286;
        constexpr int MAXDCODES = 30;
        constexpr int MAXCODES = (MAXLCODES + MAXDCODES);
        constexpr int FIXLCODES = 288;

        struct state
        {
            unsigned char *out;
            unsigned long outcnt;
            unsigned long outlen;
            const unsigned char *in;
            unsigned long incnt;
            unsigned long inlen;
            int bitbuf;
            int bitcnt;
        };

        inline int bits(state *s, int need)
        {
            long val = s->bitbuf;
            while (s->bitcnt < need)
            {
                if (s->incnt == s->inlen) return -1;
                val |= static_cast<long>(s->in[s->incnt++]) << s->bitcnt;
                s->bitcnt += 8;
            }
            s->bitbuf = static_cast<int>(val >> need);
            s->bitcnt -= need;
            return static_cast<int>(val & ((1L << need) - 1));
        }

        inline int stored(state *s)
        {
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->incnt + 4 > s->inlen) return 2;
            unsigned len = s->in[s->incnt++];
            len |= static_cast<unsigned>(s->in[s->incnt++]) << 8;
            unsigned nlen = s->in[s->incnt++];
            nlen |= static_cast<unsigned>(s->in[s->incnt++]) << 8;
            if (len != (~nlen & 0xffff)) return -2;
            if (s->incnt + len > s->inlen) return 2;
            if (s->outcnt + len > s->outlen) return 1;
            std::memcpy(s->out + s->outcnt, s->in + s->incnt, len);
            s->incnt += len;
            s->outcnt += len;
            return 0;
        }

        struct huffman
        {
            short *count;
            short *symbol;
        };

        inline int decode(state *s, const huffman *h)
        {
            int code = 0;
            int first = 0;
            int index = 0;
            for (int len = 1; len <= MAXBITS; len++)
            {
                int next = bits(s, 1);
                if (next < 0) return -1;
                code |= next;
                int count = h->count[len];
                if (code - count < first)
                    return h->symbol[index + (code - first)];
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            return -10;
        }

        inline int construct(huffman *h, const short *length, int n)
        {
            for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
            for (int symbol = 0; symbol < n; symbol++) (h->count[length[symbol]])++;
            if (h->count[0] == n) return 0;
            int left = 1;
            for (int len = 1; len <= MAXBITS; len++)
            {
                left <<= 1;
                left -= h->count[len];
                if (left < 0) return left;
            }
            short offs[MAXBITS + 1];
            offs[1] = 0;
            for (int len = 1; len < MAXBITS; len++)
                offs[len + 1] = offs[len] + h->count[len];
            for (int symbol = 0; symbol < n; symbol++)
            {
                if (length[symbol] != 0)
                    h->symbol[offs[length[symbol]]++] = static_cast<short>(symbol);
            }
            return left;
        }

        inline int codes(state *s, const huffman *lencode, const huffman *distcode)
        {
            static const short lens[29] = {
                3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
            };
            static const short lext[29] = {
                0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
            };
            static const short dists[30] = {
                1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                8193, 12289, 16385, 24577
            };
            static const short dext[30] = {
                0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
            };

            int symbol;
            do
            {
                symbol = decode(s, lencode);
                if (symbol < 0) return symbol;
                if (symbol < 256)
                {
                    if (s->outcnt == s->outlen) return 1;
                    s->out[s->outcnt++] = static_cast<unsigned char>(symbol);
                }
                else if (symbol > 256)
                {
                    symbol -= 257;
                    if (symbol >= 29) return -10;
                    int len = lens[symbol] + bits(s, lext[symbol]);
                    symbol = decode(s, distcode);
                    if (symbol < 0) return symbol;
                    unsigned dist = dists[symbol] + static_cast<unsigned>(bits(s, dext[symbol]));
                    if (dist > s->outcnt) return -11;
                    if (s->outcnt + static_cast<unsigned long>(len) > s->outlen) return 1;
                    while (len--)
                    {
                        s->out[s->outcnt] = s->out[s->outcnt - dist];
                        s->outcnt++;
                    }
                }
            } while (symbol != 256);
            return 0;
        }

        inline int fixed(state *s)
        {
            static short lencnt[MAXBITS + 1], lensym[FIXLCODES];
            static short distcnt[MAXBITS + 1], distsym[MAXDCODES];
            static huffman lencode = { lencnt, lensym };
            static huffman distcode = { distcnt, distsym };
            static bool s_initialized = false;
            if (!s_initialized)
            {
                short lengths[FIXLCODES];
                int symbol = 0;
                for (; symbol < 144; symbol++) lengths[symbol] = 8;
                for (; symbol < 256; symbol++) lengths[symbol] = 9;
                for (; symbol < 280; symbol++) lengths[symbol] = 7;
                for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
                construct(&lencode, lengths, FIXLCODES);
                for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
                construct(&distcode, lengths, MAXDCODES);
                s_initialized = true;
            }
            return codes(s, &lencode, &distcode);
        }

        inline int dynamic(state *s)
        {
            static const short order[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
            };
            int nlen = bits(s, 5) + 257;
            int ndist = bits(s, 5) + 1;
            int ncode = bits(s, 4) + 4;
            if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;

            short lengths[MAXCODES];
            for (int index = 0; index < ncode; index++) lengths[order[index]] = static_cast<short>(bits(s, 3));
            for (int index = ncode; index < 19; index++) lengths[order[index]] = 0;

            short lencnt[MAXBITS + 1], lensym[MAXLCODES];
            huffman lencode = { lencnt, lensym };
            construct(&lencode, lengths, 19);

            int index = 0;
            while (index < nlen + ndist)
            {
                int symbol = decode(s, &lencode);
                if (symbol < 0) return symbol;
                if (symbol < 16) lengths[index++] = static_cast<short>(symbol);
                else
                {
                    int len = 0;
                    if (symbol == 16)
                    {
                        if (index == 0) return -5;
                        len = lengths[index - 1];
                        symbol = 3 + bits(s, 2);
                    }
                    else if (symbol == 17)
                    {
                        symbol = 3 + bits(s, 3);
                    }
                    else
                    {
                        symbol = 11 + bits(s, 7);
                    }
                    if (index + symbol > nlen + ndist) return -6;
                    while (symbol--) lengths[index++] = static_cast<short>(len);
                }
            }
            construct(&lencode, lengths, nlen);

            short distcnt[MAXBITS + 1], distsym[MAXDCODES];
            huffman distcode = { distcnt, distsym };
            construct(&distcode, lengths + nlen, ndist);

            return codes(s, &lencode, &distcode);
        }

        inline int puff(unsigned char *dest, unsigned long *destlen, const unsigned char *source, unsigned long *sourcelen)
        {
            state s{};
            s.out = dest;
            s.outlen = *destlen;
            s.outcnt = 0;
            s.in = source;
            s.inlen = *sourcelen;
            s.incnt = 0;
            s.bitbuf = 0;
            s.bitcnt = 0;

            int err = 0;
            int last = 0;
            do
            {
                last = bits(&s, 1);
                int type = bits(&s, 2);
                if (type == 0) err = stored(&s);
                else if (type == 1) err = fixed(&s);
                else if (type == 2) err = dynamic(&s);
                else err = -1;
                if (err != 0) break;
            } while (!last);

            *destlen = s.outcnt;
            *sourcelen = s.incnt;
            return err;
        }
    } // namespace puff_detail
    // 256-entry CRC-16 table extracted directly from file3.irx (.data:0x3BE0)
    inline constexpr uint16_t CRC16_TABLE[256] = {
        0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
        0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
        0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
        0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
        0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
        0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
        0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
        0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
        0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
        0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
        0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
        0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
        0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
        0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
        0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
        0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
        0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
        0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
        0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
        0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
        0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
        0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
        0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
        0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
        0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
        0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
        0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
        0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
        0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
        0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
        0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
        0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78
    };

    inline uint16_t computeCrc16(const char *path)
    {
        if (!path) return 0;
        while (*path == '/') path++;
        uint16_t crc = 0xFFFF;
        while (*path)
        {
            uint8_t b = static_cast<uint8_t>(*path);
            uint8_t idx = static_cast<uint8_t>((crc ^ b) & 0xFF);
            crc = CRC16_TABLE[idx] ^ (crc >> 8);
            if (b == '/')
            {
                while (*(path + 1) == '/') path++;
            }
            path++;
        }
        return crc & 0xFFFF;
    }

    inline std::string normalizeGamePath(const char *rawPath)
    {
        if (!rawPath) return "";
        std::string p(rawPath);
        // Strip leading whitespace
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
        while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();

        // Convert backslashes to forward slashes and lowercase
        for (char &c : p)
        {
            if (c == '\\') c = '/';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }

        // Strip prefixes: "cdrom0:", "host0:", "cdrom:", "host:"
        const char *prefixes[] = {"cdrom0:", "host0:", "cdrom:", "host:"};
        for (const char *pre : prefixes)
        {
            size_t len = std::strlen(pre);
            if (p.rfind(pre, 0) == 0)
            {
                p = p.substr(len);
                break;
            }
        }

        // Strip leading slashes
        while (!p.empty() && p.front() == '/') p.erase(p.begin());

        // Strip trailing version ";1"
        size_t semi = p.find(';');
        if (semi != std::string::npos)
        {
            p.erase(semi);
        }

        // Strip trailing slashes
        while (!p.empty() && p.back() == '/') p.pop_back();

        return p;
    }

    struct VfiEntry
    {
        std::string fullPath;
        std::string name;
        uint32_t startSector = 0; // relative to DATA.BIN base LSN
        uint32_t isoLsn = 0;      // absolute ISO LSN
        uint64_t fileSize = 0;    // exact uncompressed size
        uint16_t crc16 = 0;       // Kermit CRC16
        bool isDecompressedSz = false;
        uint64_t rawFileSize = 0;
    };

    struct VfiDirRecord
    {
        uint16_t relOff = 0;
        uint16_t recLen = 0;
        uint16_t subtreeEnd = 0;
        uint16_t parentOff = 0;
        uint16_t firstFile = 0;
        std::string name;
    };

    struct VfiDirEntry
    {
        std::string fullPath;
        std::string name;
        uint16_t relOff = 0;
    };

    class VfiArchive
    {
    public:
        static VfiArchive &instance()
        {
            static VfiArchive s_vfi;
            return s_vfi;
        }

        bool initialize()
        {
            if (m_initialized) return m_loadSuccess;
            m_initialized = true;

            IsoDisc::Entry dataBinEntry{};
            if (!IsoDisc::instance().find("DATA.BIN", dataBinEntry) &&
                !IsoDisc::instance().find("DATA.BIN;1", dataBinEntry))
            {
                std::fprintf(stderr, "[VfiArchive] ERROR: DATA.BIN not found in ISO directory.\n");
                std::fflush(stderr);
                m_loadSuccess = false;
                return false;
            }

            m_dataBinLsn = dataBinEntry.lsn;
            std::fprintf(stderr, "[VfiArchive] DATA.BIN located at LSN %u (size %u bytes)\n",
                         m_dataBinLsn, dataBinEntry.size);
            std::fflush(stderr);

            // Read 63 sectors (129,024 bytes) of VFI index
            constexpr uint32_t kVfiSectorCount = 63u;
            constexpr size_t kVfiIndexBytes = kVfiSectorCount * 2048u;
            std::vector<uint8_t> vfiIndex(kVfiIndexBytes);

            if (!IsoDisc::instance().readSectors(m_dataBinLsn, kVfiSectorCount, vfiIndex.data()))
            {
                std::fprintf(stderr, "[VfiArchive] ERROR: Failed to read %u VFI index sectors from LSN %u\n",
                             kVfiSectorCount, m_dataBinLsn);
                std::fflush(stderr);
                m_loadSuccess = false;
                return false;
            }

            const uint8_t *data = vfiIndex.data();
            if (std::memcmp(data, "VFI\0", 4) != 0)
            {
                std::fprintf(stderr, "[VfiArchive] ERROR: Invalid VFI magic: %02x %02x %02x %02x\n",
                             data[0], data[1], data[2], data[3]);
                std::fflush(stderr);
                m_loadSuccess = false;
                return false;
            }

            uint32_t fileTabOff = *reinterpret_cast<const uint32_t *>(data + 0x14);
            uint32_t dirTabOff = *reinterpret_cast<const uint32_t *>(data + 0x18);
            uint32_t dirEndOff = *reinterpret_cast<const uint32_t *>(data + 0x1C);

            // 1. Parse Directory Tree Table
            uint32_t pos = dirTabOff;
            while (pos + 8 <= dirEndOff && pos < kVfiIndexBytes)
            {
                uint16_t recLen = *reinterpret_cast<const uint16_t *>(data + pos);
                if (recLen == 0 || pos + recLen > dirEndOff) break;
                uint16_t subtreeEnd = *reinterpret_cast<const uint16_t *>(data + pos + 2);
                uint16_t parentOff = *reinterpret_cast<const uint16_t *>(data + pos + 4);
                uint16_t firstFile = *reinterpret_cast<const uint16_t *>(data + pos + 6);
                
                std::string name;
                for (size_t i = pos + 8; i < pos + recLen && data[i] != '\0'; ++i)
                {
                    name.push_back(static_cast<char>(data[i]));
                }

                uint16_t relOff = static_cast<uint16_t>(pos - dirTabOff);
                m_dirs[relOff] = VfiDirRecord{relOff, recLen, subtreeEnd, parentOff, firstFile, name};
                pos += recLen;
            }

            // 2. Parse File Record Table
            pos = fileTabOff;
            while (pos + 12 <= dirTabOff && pos < kVfiIndexBytes)
            {
                uint16_t recLen = *reinterpret_cast<const uint16_t *>(data + pos);
                if (recLen == 0 || pos + recLen > dirTabOff) break;
                uint16_t dirRelOff = *reinterpret_cast<const uint16_t *>(data + pos + 2);
                uint32_t startSector = *reinterpret_cast<const uint32_t *>(data + pos + 4);
                uint32_t fileSize = *reinterpret_cast<const uint32_t *>(data + pos + 8);

                std::string name;
                for (size_t i = pos + 12; i < pos + recLen && data[i] != '\0'; ++i)
                {
                    name.push_back(static_cast<char>(data[i]));
                }

                std::string dirPath = getDirPath(dirRelOff);
                std::string fullPath = dirPath.empty() ? name : (dirPath + "/" + name);
                std::string normPath = normalizeGamePath(fullPath.c_str());
                uint16_t crc = computeCrc16(fullPath.c_str());

                VfiEntry entry;
                entry.fullPath = fullPath;
                entry.name = name;
                entry.startSector = startSector;
                entry.isoLsn = m_dataBinLsn + startSector;
                entry.fileSize = static_cast<uint64_t>(fileSize);
                entry.crc16 = crc;

                m_files.push_back(entry);
                m_filesByPath[normPath] = entry;

                pos += recLen;
            }

            // Populate directory lookup by path
            for (const auto &[relOff, rec] : m_dirs)
            {
                std::string fullPath = getDirPath(relOff);
                if (!fullPath.empty())
                {
                    std::string norm = normalizeGamePath(fullPath.c_str());
                    m_dirsByPath[norm] = VfiDirEntry{fullPath, rec.name, relOff};
                }
            }

            std::fprintf(stderr, "[VfiArchive] Initialized: %zu directories (%zu by path), %zu files parsed successfully.\n",
                         m_dirs.size(), m_dirsByPath.size(), m_files.size());
            std::fflush(stderr);

            m_loadSuccess = true;

            // Self-test if requested by PS2X_VFI_SELFTEST=1
            runSelfTest();

            return true;
        }

        const VfiDirEntry *findDir(const std::string &path)
        {
            if (!m_initialized) initialize();
            if (!m_loadSuccess) return nullptr;

            std::string norm = normalizeGamePath(path.c_str());
            auto it = m_dirsByPath.find(norm);
            if (it != m_dirsByPath.end())
            {
                return &it->second;
            }
            return nullptr;
        }

        const VfiEntry *find(const std::string &path)
        {
            if (!m_initialized) initialize();
            if (!m_loadSuccess) return nullptr;

            std::string norm = normalizeGamePath(path.c_str());
            const bool requestedWithSz = (norm.size() > 3 && norm.rfind(".sz") == norm.size() - 3);

            // Direct match
            auto it = m_filesByPath.find(norm);
            if (it != m_filesByPath.end())
            {
                return &it->second;
            }

            // Fallback 1: if path was uncompressed, check if norm + ".sz" exists
            if (!requestedWithSz)
            {
                std::string szPath = norm + ".sz";
                auto itSz = m_filesByPath.find(szPath);
                if (itSz != m_filesByPath.end())
                {
                    std::lock_guard<std::mutex> lock(m_decompressMutex);
                    auto cachedIt = m_decompressedEntries.find(norm);
                    if (cachedIt != m_decompressedEntries.end())
                    {
                        return &cachedIt->second;
                    }

                    // Read 4-byte uncompressed size header from start sector of itSz
                    std::vector<uint8_t> secBuf(2048);
                    if (!IsoDisc::instance().readSectors(itSz->second.isoLsn, 1, secBuf.data()))
                    {
                        std::fprintf(stderr, "[VFI] ERROR: Failed to read sector for .sz header of <%s>\n", szPath.c_str());
                        std::fflush(stderr);
                        return &itSz->second;
                    }

                    uint32_t uncompSize = *reinterpret_cast<const uint32_t *>(secBuf.data());
                    VfiEntry decompEntry = itSz->second;
                    decompEntry.fullPath = norm;
                    if (decompEntry.name.size() > 3 && decompEntry.name.rfind(".sz") == decompEntry.name.size() - 3)
                    {
                        decompEntry.name = decompEntry.name.substr(0, decompEntry.name.size() - 3);
                    }
                    decompEntry.rawFileSize = itSz->second.fileSize;
                    decompEntry.fileSize = uncompSize;
                    decompEntry.isDecompressedSz = true;

                    std::fprintf(stderr, "[VFI] .sz fallback: requested <%s> served <%s> (%llu bytes compressed)\n",
                                 path.c_str(), itSz->second.fullPath.c_str(),
                                 static_cast<unsigned long long>(itSz->second.fileSize));
                    std::fprintf(stderr, "[VFI] .sz transparent decompression prepared: <%s> uncompressedSize=%u bytes (compressed=%llu bytes)\n",
                                 norm.c_str(), uncompSize, static_cast<unsigned long long>(itSz->second.fileSize));
                    std::fflush(stderr);

                    auto inserted = m_decompressedEntries.emplace(norm, std::move(decompEntry));
                    return &inserted.first->second;
                }
            }

            // Fallback 2: if path already had ".sz", check without ".sz"
            if (requestedWithSz)
            {
                auto itNoSz = m_filesByPath.find(norm.substr(0, norm.size() - 3));
                if (itNoSz != m_filesByPath.end())
                {
                    return &itNoSz->second;
                }
            }

            // Fallback 3: search by filename if direct match failed
            for (const auto &f : m_files)
            {
                std::string nName = normalizeGamePath(f.name.c_str());
                if (nName == norm)
                {
                    return &f;
                }
                if (!requestedWithSz && nName == (norm + ".sz"))
                {
                    std::string fNorm = normalizeGamePath(f.fullPath.c_str());
                    if (fNorm.size() > 3 && fNorm.rfind(".sz") == fNorm.size() - 3)
                    {
                        return find(fNorm.substr(0, fNorm.size() - 3));
                    }
                }
            }
            return nullptr;
        }

        bool readFileData(const VfiEntry &entry, uint64_t offset, uint64_t size, void *dst)
        {
            if (offset >= entry.fileSize || size == 0 || !dst)
            {
                return false;
            }
            uint64_t clampedSize = std::min(size, entry.fileSize - offset);

            if (entry.isDecompressedSz)
            {
                std::lock_guard<std::mutex> lock(m_decompressMutex);
                auto it = m_decompressedCache.find(entry.fullPath);
                if (it == m_decompressedCache.end())
                {
                    uint64_t compSize = entry.rawFileSize;
                    uint32_t totalSec = static_cast<uint32_t>((compSize + 2047ull) / 2048ull);
                    std::vector<uint8_t> compBuf(static_cast<size_t>(totalSec) * 2048u);
                    if (!IsoDisc::instance().readSectors(entry.isoLsn, totalSec, compBuf.data()))
                    {
                        std::fprintf(stderr, "[VFI] ERROR: Failed to read %u compressed sectors for <%s>\n",
                                     totalSec, entry.fullPath.c_str());
                        std::fflush(stderr);
                        return false;
                    }

                    uint32_t expectedUncomp = *reinterpret_cast<const uint32_t *>(compBuf.data());
                    std::vector<uint8_t> uncompBuf(expectedUncomp);
                    unsigned long destLen = expectedUncomp;
                    unsigned long srcLen = static_cast<unsigned long>(compSize - 4);

                    int err = puff_detail::puff(uncompBuf.data(), &destLen, compBuf.data() + 4, &srcLen);
                    if (err != 0 || destLen != expectedUncomp)
                    {
                        std::fprintf(stderr, "[VFI] ERROR: Decompression failed for <%s>: err=%d, destLen=%lu, expected=%u\n",
                                     entry.fullPath.c_str(), err, destLen, expectedUncomp);
                        std::fflush(stderr);
                        return false;
                    }

                    std::fprintf(stderr, "[VFI] Successfully decompressed <%s>: %u bytes (magic %.4s)\n",
                                 entry.fullPath.c_str(), expectedUncomp, reinterpret_cast<const char *>(uncompBuf.data()));
                    std::fflush(stderr);

                    auto ins = m_decompressedCache.emplace(entry.fullPath, std::move(uncompBuf));
                    it = ins.first;
                }

                if (offset + clampedSize > it->second.size())
                {
                    clampedSize = (offset < it->second.size()) ? (it->second.size() - offset) : 0ull;
                }
                std::memcpy(dst, it->second.data() + offset, static_cast<size_t>(clampedSize));
                return true;
            }

            uint32_t startSec = entry.isoLsn + static_cast<uint32_t>(offset / 2048ull);
            uint32_t secOffset = static_cast<uint32_t>(offset % 2048ull);
            uint32_t totalSectors = static_cast<uint32_t>((secOffset + clampedSize + 2047ull) / 2048ull);

            std::vector<uint8_t> secBuf(static_cast<size_t>(totalSectors) * 2048u);
            if (!IsoDisc::instance().readSectors(startSec, totalSectors, secBuf.data()))
            {
                std::fprintf(stderr, "[VfiArchive] readSectors failed: startSec=%u count=%u\n", startSec, totalSectors);
                std::fflush(stderr);
                return false;
            }

            std::memcpy(dst, secBuf.data() + secOffset, static_cast<size_t>(clampedSize));
            return true;
        }

        size_t totalFiles() const { return m_files.size(); }
        uint32_t dataBinLsn() const { return m_dataBinLsn; }

    private:
        VfiArchive() = default;

        std::string getDirPath(uint16_t relOff) const
        {
            std::vector<std::string> parts;
            uint16_t curr = relOff;
            while (true)
            {
                auto it = m_dirs.find(curr);
                if (it == m_dirs.end()) break;
                parts.push_back(it->second.name);
                if (curr == 0) break;
                uint16_t p = it->second.parentOff;
                if (p == curr || (p == 0 && curr >= 36)) break;
                curr = p;
            }
            std::string res;
            for (int i = static_cast<int>(parts.size()) - 1; i >= 0; --i)
            {
                if (!res.empty()) res += '/';
                res += parts[i];
            }
            return res;
        }

        void runSelfTest()
        {
            const char *env = std::getenv("PS2X_VFI_SELFTEST");
            if (!env || std::strcmp(env, "1") != 0)
            {
                return;
            }

            std::fprintf(stderr, "[VfiArchive:SelfTest] Running self-test checks...\n");
            bool passed = true;

            // Check 1: total files == 3,994
            if (m_files.size() != 3994)
            {
                std::fprintf(stderr, "[VfiArchive:SelfTest] FAIL: Expected 3994 files, got %zu\n", m_files.size());
                passed = false;
            }
            else
            {
                std::fprintf(stderr, "[VfiArchive:SelfTest] PASS: Total files count is 3994\n");
            }

            // Check 2: seurat_20x20.apefnt = 6,728 bytes at LSN 113644
            const VfiEntry *fnt = find("debug/us/etc/font/seurat_20x20.apefnt");
            if (!fnt || fnt->fileSize != 6728 || fnt->isoLsn != 113644)
            {
                std::fprintf(stderr, "[VfiArchive:SelfTest] FAIL: seurat_20x20.apefnt expected 6728 B at LSN 113644, got %llu B at LSN %u\n",
                             fnt ? static_cast<unsigned long long>(fnt->fileSize) : 0ull,
                             fnt ? fnt->isoLsn : 0u);
                passed = false;
            }
            else
            {
                std::fprintf(stderr, "[VfiArchive:SelfTest] PASS: seurat_20x20.apefnt (6728 B, LSN 113644, CRC 0x%04X)\n", fnt->crc16);
            }

            // Check 3: seurat_20x20.ttxbin = 7,156 bytes at LSN 113648
            const VfiEntry *ttx = find("debug/us/etc/font/seurat_20x20.ttxbin");
            if (!ttx || ttx->fileSize != 7156 || ttx->isoLsn != 113648)
            {
                std::fprintf(stderr, "[VfiArchive:SelfTest] FAIL: seurat_20x20.ttxbin expected 7156 B at LSN 113648, got %llu B at LSN %u\n",
                             ttx ? static_cast<unsigned long long>(ttx->fileSize) : 0ull,
                             ttx ? ttx->isoLsn : 0u);
                passed = false;
            }
            else
            {
                std::fprintf(stderr, "[VfiArchive:SelfTest] PASS: seurat_20x20.ttxbin (7156 B, LSN 113648, CRC 0x%04X)\n", ttx->crc16);
            }

            std::fflush(stderr);
            if (!passed)
            {
                std::fprintf(stderr, "[VfiArchive:SelfTest] ABORTING due to self-test mismatch.\n");
                std::fflush(stderr);
                std::abort();
            }
            std::fprintf(stderr, "[VfiArchive:SelfTest] All VFI self-test checks PASSED.\n");
            std::fflush(stderr);
        }

        bool m_initialized = false;
        bool m_loadSuccess = false;
        uint32_t m_dataBinLsn = 0;
        std::unordered_map<uint16_t, VfiDirRecord> m_dirs;
        std::unordered_map<std::string, VfiDirEntry> m_dirsByPath;
        std::vector<VfiEntry> m_files;
        std::unordered_map<std::string, VfiEntry> m_filesByPath;
        std::mutex m_decompressMutex;
        std::unordered_map<std::string, std::vector<uint8_t>> m_decompressedCache;
        std::unordered_map<std::string, VfiEntry> m_decompressedEntries;
    };
}
