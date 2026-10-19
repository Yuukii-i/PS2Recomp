#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>

class IsoDisc
{
public:
    struct Entry
    {
        uint32_t lsn = 0;
        uint32_t size = 0;
        uint8_t flags = 0;
        uint8_t date[8]{};
        std::string name;
    };

    static IsoDisc &instance();

    bool open(const std::string &path = "");
    void close();
    bool isOpen() const { return m_file != nullptr; }

    bool readSectors(uint32_t lsn, uint32_t count, void *dst);
    bool find(const std::string &path, Entry &outEntry, int layer = 0);

    uint64_t totalBytes() const { return m_totalBytes; }
    uint64_t totalSectors() const { return m_totalSectors; }
    uint32_t rootLsn() const { return m_rootLsn; }
    uint32_t rootSize() const { return m_rootSize; }
    const std::string &openedPath() const { return m_openedPath; }

private:
    IsoDisc() = default;
    ~IsoDisc();
    IsoDisc(const IsoDisc &) = delete;
    IsoDisc &operator=(const IsoDisc &) = delete;

    void dumpDirectory(uint32_t dirLsn, uint32_t dirSize, const std::string &indent);

    std::mutex m_mutex;
    FILE *m_file = nullptr;
    std::string m_openedPath;
    uint64_t m_totalBytes = 0;
    uint64_t m_totalSectors = 0;
    uint32_t m_rootLsn = 0;
    uint32_t m_rootSize = 0;
};
