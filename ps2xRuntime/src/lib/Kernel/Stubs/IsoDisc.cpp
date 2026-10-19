#include "IsoDisc.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>

namespace
{
    std::string toUpperAscii(std::string str)
    {
        for (char &c : str)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return str;
    }

    std::string normalizeIsoPath(std::string path)
    {
        for (char &c : path)
        {
            if (c == '/')
            {
                c = '\\';
            }
        }

        std::string lower = path;
        for (char &c : lower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (lower.rfind("cdrom0:", 0) == 0)
        {
            path = path.substr(7);
        }
        else if (lower.rfind("cdrom:", 0) == 0)
        {
            path = path.substr(6);
        }

        while (!path.empty() && (path.front() == '\\' || path.front() == '/'))
        {
            path.erase(path.begin());
        }
        while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
        {
            path.pop_back();
        }
        return path;
    }

    std::string stripVersionSuffix(std::string name)
    {
        const size_t semi = name.find(';');
        if (semi != std::string::npos)
        {
            name.erase(semi);
        }
        return name;
    }

    std::vector<std::string> splitPath(const std::string &path)
    {
        std::vector<std::string> tokens;
        size_t start = 0;
        while (start < path.size())
        {
            size_t end = path.find('\\', start);
            if (end == std::string::npos)
            {
                end = path.size();
            }
            if (end > start)
            {
                tokens.push_back(path.substr(start, end - start));
            }
            start = end + 1;
        }
        return tokens;
    }
}

IsoDisc &IsoDisc::instance()
{
    static IsoDisc s_instance;
    return s_instance;
}

IsoDisc::~IsoDisc()
{
    close();
}

void IsoDisc::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
    m_openedPath.clear();
    m_totalBytes = 0;
    m_totalSectors = 0;
    m_rootLsn = 0;
    m_rootSize = 0;
}

bool IsoDisc::open(const std::string &hintPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file)
    {
        return true;
    }

    std::vector<std::string> candidates;
    if (!hintPath.empty())
    {
        candidates.push_back(hintPath);
    }

    if (const char *envIso = std::getenv("PS2X_ISO"); envIso && envIso[0] != '\0')
    {
        candidates.push_back(envIso);
    }

    candidates.push_back("build/PS2Recomp/ps2xRuntime/ApeEscape3.iso");
    candidates.push_back("ApeEscape3.iso");
    candidates.push_back("../ApeEscape3.iso");
    candidates.push_back("../build/PS2Recomp/ps2xRuntime/ApeEscape3.iso");
    candidates.push_back("../../build/PS2Recomp/ps2xRuntime/ApeEscape3.iso");

    std::string chosenPath;
    for (const auto &c : candidates)
    {
        std::error_code ec;
        if (std::filesystem::exists(c, ec) && !std::filesystem::is_directory(c, ec))
        {
            chosenPath = c;
            break;
        }
    }

    if (chosenPath.empty())
    {
        fprintf(stderr, "[IsoDisc] No ISO image found in search paths or PS2X_ISO.\n");
        fflush(stderr);
        return false;
    }

    m_file = std::fopen(chosenPath.c_str(), "rb");
    if (!m_file)
    {
        fprintf(stderr, "[IsoDisc] Failed to open ISO file: %s\n", chosenPath.c_str());
        fflush(stderr);
        return false;
    }

#if defined(_WIN32)
    _fseeki64(m_file, 0, SEEK_END);
    m_totalBytes = static_cast<uint64_t>(_ftelli64(m_file));
#else
    fseeko(m_file, 0, SEEK_END);
    m_totalBytes = static_cast<uint64_t>(ftello(m_file));
#endif
    m_totalSectors = m_totalBytes / 2048;

    // Read PVD at LBA 16 (offset 16 * 2048 = 32768)
    uint8_t pvd[2048]{};
    const int64_t pvdOffset = static_cast<int64_t>(16) * 2048;
#if defined(_WIN32)
    _fseeki64(m_file, pvdOffset, SEEK_SET);
#else
    fseeko(m_file, pvdOffset, SEEK_SET);
#endif
    if (std::fread(pvd, 1, 2048, m_file) != 2048)
    {
        fprintf(stderr, "[IsoDisc] Failed to read PVD at LBA 16 from %s\n", chosenPath.c_str());
        fflush(stderr);
        std::fclose(m_file);
        m_file = nullptr;
        return false;
    }

    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
    {
        fprintf(stderr, "[IsoDisc] Invalid ISO9660 PVD header in %s\n", chosenPath.c_str());
        fflush(stderr);
        std::fclose(m_file);
        m_file = nullptr;
        return false;
    }

    const uint8_t *rootRec = pvd + 156;
    m_rootLsn = *reinterpret_cast<const uint32_t *>(rootRec + 2);
    m_rootSize = *reinterpret_cast<const uint32_t *>(rootRec + 10);
    m_openedPath = chosenPath;

    fprintf(stderr, "[IsoDisc] Successfully opened ISO: %s\n", chosenPath.c_str());
    fprintf(stderr, "[IsoDisc] Disc size: %llu bytes (%llu sectors), Root directory at LSN %u (size %u bytes)\n",
            static_cast<unsigned long long>(m_totalBytes),
            static_cast<unsigned long long>(m_totalSectors),
            m_rootLsn, m_rootSize);
    fprintf(stderr, "[IsoDisc] Root directory listing:\n");
    dumpDirectory(m_rootLsn, m_rootSize, "  ");
    fflush(stderr);

    return true;
}

bool IsoDisc::readSectors(uint32_t lsn, uint32_t count, void *dst)
{
    if (!isOpen())
    {
        if (!open())
        {
            return false;
        }
    }

    if (!m_file || !dst || count == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const int64_t offset = static_cast<int64_t>(lsn) * 2048;
#if defined(_WIN32)
    if (_fseeki64(m_file, offset, SEEK_SET) != 0)
    {
        return false;
    }
#else
    if (fseeko(m_file, offset, SEEK_SET) != 0)
    {
        return false;
    }
#endif

    const size_t bytesToRead = static_cast<size_t>(count) * 2048;
    const size_t bytesRead = std::fread(dst, 1, bytesToRead, m_file);
    return bytesRead == bytesToRead;
}

void IsoDisc::dumpDirectory(uint32_t dirLsn, uint32_t dirSize, const std::string &indent)
{
    const uint32_t sectorCount = (dirSize + 2047) / 2048;
    std::vector<uint8_t> sector(2048);

    for (uint32_t s = 0; s < sectorCount; ++s)
    {
        const int64_t offset = static_cast<int64_t>(dirLsn + s) * 2048;
#if defined(_WIN32)
        if (_fseeki64(m_file, offset, SEEK_SET) != 0) break;
#else
        if (fseeko(m_file, offset, SEEK_SET) != 0) break;
#endif
        if (std::fread(sector.data(), 1, 2048, m_file) != 2048) break;

        uint32_t pos = 0;
        while (pos < 2048)
        {
            const uint8_t len_dr = sector[pos];
            if (len_dr == 0)
            {
                break; // Zero padding to next sector boundary
            }
            if (pos + len_dr > 2048)
            {
                break;
            }

            const uint32_t recLsn = *reinterpret_cast<const uint32_t *>(sector.data() + pos + 2);
            const uint32_t recSize = *reinterpret_cast<const uint32_t *>(sector.data() + pos + 10);
            const uint8_t flags = sector[pos + 25];
            const uint8_t len_fi = sector[pos + 32];
            const char *fi = reinterpret_cast<const char *>(sector.data() + pos + 33);

            std::string name;
            if (len_fi == 1 && fi[0] == 0)
            {
                name = ".";
            }
            else if (len_fi == 1 && fi[0] == 1)
            {
                name = "..";
            }
            else
            {
                name.assign(fi, len_fi);
            }

            fprintf(stderr, "%s[IsoDisc] Entry: '%s' LSN=%u Size=%u Flags=0x%02x\n",
                    indent.c_str(), name.c_str(), recLsn, recSize, flags);

            pos += len_dr;
        }
    }
}

bool IsoDisc::find(const std::string &path, Entry &outEntry, int layer)
{
    (void)layer;
    if (!isOpen())
    {
        if (!open())
        {
            return false;
        }
    }

    const std::string normalized = normalizeIsoPath(path);
    const std::vector<std::string> tokens = splitPath(normalized);

    if (tokens.empty())
    {
        outEntry.lsn = m_rootLsn;
        outEntry.size = m_rootSize;
        outEntry.flags = 0x02;
        outEntry.name = "\\";
        return true;
    }

    uint32_t currLsn = m_rootLsn;
    uint32_t currSize = m_rootSize;

    std::vector<uint8_t> sector(2048);

    for (size_t t = 0; t < tokens.size(); ++t)
    {
        const std::string tokenClean = toUpperAscii(stripVersionSuffix(tokens[t]));
        const bool isLastToken = (t == tokens.size() - 1);
        bool foundToken = false;

        const uint32_t sectorCount = (currSize + 2047) / 2048;
        for (uint32_t s = 0; s < sectorCount && !foundToken; ++s)
        {
            if (!readSectors(currLsn + s, 1, sector.data()))
            {
                return false;
            }

            uint32_t pos = 0;
            while (pos < 2048)
            {
                const uint8_t len_dr = sector[pos];
                if (len_dr == 0)
                {
                    break; // Sector padding
                }
                if (pos + len_dr > 2048)
                {
                    break;
                }

                const uint8_t len_fi = sector[pos + 32];
                const char *fi = reinterpret_cast<const char *>(sector.data() + pos + 33);
                const uint8_t flags = sector[pos + 25];

                if (len_fi > 0 && fi[0] != 0 && fi[0] != 1)
                {
                    std::string recName(fi, len_fi);
                    std::string recClean = toUpperAscii(stripVersionSuffix(recName));

                    if (recClean == tokenClean)
                    {
                        const uint32_t recLsn = *reinterpret_cast<const uint32_t *>(sector.data() + pos + 2);
                        const uint32_t recSize = *reinterpret_cast<const uint32_t *>(sector.data() + pos + 10);

                        if (isLastToken)
                        {
                            outEntry.lsn = recLsn;
                            outEntry.size = recSize;
                            outEntry.flags = flags;
                            std::memcpy(outEntry.date, sector.data() + pos + 18, 7);
                            outEntry.date[7] = 0;
                            outEntry.name = stripVersionSuffix(recName);
                            return true;
                        }

                        if (!(flags & 0x02))
                        {
                            return false; // Intermediate path element is not a directory
                        }

                        currLsn = recLsn;
                        currSize = recSize;
                        foundToken = true;
                        break;
                    }
                }

                pos += len_dr;
            }
        }

        if (!foundToken)
        {
            return false;
        }
    }

    return false;
}
