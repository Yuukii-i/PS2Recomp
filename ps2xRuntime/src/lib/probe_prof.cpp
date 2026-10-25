#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include <thread>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace probe_ae {
    void bhLog(const char *fmt, ...);

#ifdef _WIN32
    static std::atomic<bool> s_cgSamplerStarted{false};
    static std::atomic<DWORD> s_cgMainGuestThreadId{0};

    void recordMainGuestThread() {
        s_cgMainGuestThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }

    struct CgThreadAgg {
        uint64_t totalSamples = 0;
        std::unordered_map<std::string, uint64_t> leafCounts;
    };

    static void cgSamplerWorker() {
        HANDLE hProc = GetCurrentProcess();
        DWORD currentPid = GetCurrentProcessId();
        DWORD samplerTid = GetCurrentThreadId();

        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);

        char exeDir[MAX_PATH] = {};
        strncpy(exeDir, exePath, sizeof(exeDir) - 1);
        char *lastSlash = strrchr(exeDir, '\\');
        if (lastSlash) {
            *lastSlash = '\0';
        } else {
            strcpy(exeDir, ".");
        }

        char searchPath[MAX_PATH * 3] = {};
        snprintf(searchPath, sizeof(searchPath), "%s;%s\\..;.", exeDir, exeDir);

        char pdbPath[MAX_PATH] = {};
        strncpy(pdbPath, exePath, sizeof(pdbPath) - 1);
        char *dot = strrchr(pdbPath, '.');
        if (dot) {
            strcpy(dot, ".pdb");
        } else {
            strncat(pdbPath, ".pdb", sizeof(pdbPath) - strlen(pdbPath) - 1);
        }
        DWORD pdbAttr = GetFileAttributesA(pdbPath);
        int pdbExists = (pdbAttr != INVALID_FILE_ATTRIBUTES && !(pdbAttr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;

        DWORD64 exeBase = reinterpret_cast<DWORD64>(GetModuleHandleA(NULL));

        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);

        BOOL initOk = SymInitialize(hProc, searchPath, TRUE);
        DWORD initErr = GetLastError();
        if (!initOk && initErr == ERROR_INVALID_PARAMETER) {
            initOk = TRUE;
        }
        SymSetSearchPath(hProc, searchPath);

        bhLog("[prof2] syminit ok=%d err=%lu exe=%s pdb_exists=%d base=0x%llX\n",
              initOk ? 1 : 0, initErr, exePath, pdbExists, (unsigned long long)exeBase);

        IMAGEHLP_MODULE64 modInfo{};
        modInfo.SizeOfStruct = sizeof(modInfo);
        bool needExplicitLoad = true;
        if (SymGetModuleInfo64(hProc, exeBase, &modInfo)) {
            if (modInfo.SymType != SymNone && modInfo.SymType != SymExport) {
                needExplicitLoad = false;
            }
        }
        if (needExplicitLoad) {
            SymLoadModule64(hProc, NULL, exePath, NULL, exeBase, 0);
        }

        // Self-test
        DWORD64 testAddr = reinterpret_cast<DWORD64>(&recordMainGuestThread);
        char testBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
        PSYMBOL_INFO pTestSymbol = reinterpret_cast<PSYMBOL_INFO>(testBuffer);
        pTestSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pTestSymbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 testDisp = 0;
        std::string testSymName = "none";
        if (SymFromAddr(hProc, testAddr, &testDisp, pTestSymbol)) {
            testSymName = pTestSymbol->Name;
        }

        IMAGEHLP_LINE64 testLineInfo{};
        testLineInfo.SizeOfStruct = sizeof(testLineInfo);
        DWORD testLineDisp = 0;
        std::string testLineStr = "none";
        if (SymGetLineFromAddr64(hProc, testAddr, &testLineDisp, &testLineInfo) && testLineInfo.FileName) {
            const char *lastSlashF = strrchr(testLineInfo.FileName, '\\');
            const char *lastSlashF2 = strrchr(testLineInfo.FileName, '/');
            const char *baseFile = lastSlashF ? (lastSlashF + 1) : (lastSlashF2 ? (lastSlashF2 + 1) : testLineInfo.FileName);
            char lbuf[64];
            snprintf(lbuf, sizeof(lbuf), "%s:%u", baseFile, testLineInfo.LineNumber);
            testLineStr = lbuf;
        }

        IMAGEHLP_MODULE64 testModInfo{};
        testModInfo.SizeOfStruct = sizeof(testModInfo);
        int testSymType = -1;
        if (SymGetModuleInfo64(hProc, testAddr, &testModInfo)) {
            testSymType = static_cast<int>(testModInfo.SymType);
        }

        bhLog("[prof2] selftest sym=%s line=%s symtype=%d\n",
              testSymName.c_str(), testLineStr.c_str(), testSymType);

        std::unordered_map<DWORD64, std::string> symCache;
        symCache.reserve(16384);

        auto resolveSym = [&](DWORD64 addr) -> std::string {
            DWORD64 key = addr >> 4;
            auto it = symCache.find(key);
            if (it != symCache.end()) return it->second;

            if (symCache.size() >= 16384) {
                symCache.clear();
            }

            char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
            PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
            pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            pSymbol->MaxNameLen = MAX_SYM_NAME;

            DWORD64 displacement = 0;
            std::string name;
            if (SymFromAddr(hProc, addr, &displacement, pSymbol)) {
                name = pSymbol->Name;
            } else {
                IMAGEHLP_MODULE64 mInfo{};
                mInfo.SizeOfStruct = sizeof(mInfo);
                if (SymGetModuleInfo64(hProc, addr, &mInfo)) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s+0x%llX", mInfo.ModuleName, (unsigned long long)(addr - mInfo.BaseOfImage));
                    name = buf;
                } else {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)addr);
                    name = buf;
                }
            }
            symCache[key] = name;
            return name;
        };

        std::unordered_map<DWORD64, std::string> lineCache;
        lineCache.reserve(16384);

        auto resolveLine = [&](DWORD64 addr, const std::string &fallbackSym) -> std::string {
            auto it = lineCache.find(addr);
            if (it != lineCache.end()) return it->second;

            if (lineCache.size() >= 16384) {
                lineCache.clear();
            }

            IMAGEHLP_LINE64 lineInfo{};
            lineInfo.SizeOfStruct = sizeof(lineInfo);
            DWORD lineDisp = 0;
            std::string res;
            if (SymGetLineFromAddr64(hProc, addr, &lineDisp, &lineInfo) && lineInfo.FileName) {
                const char *lastSlashF = strrchr(lineInfo.FileName, '\\');
                const char *lastSlashF2 = strrchr(lineInfo.FileName, '/');
                const char *baseFile = lastSlashF ? (lastSlashF + 1) : (lastSlashF2 ? (lastSlashF2 + 1) : lineInfo.FileName);
                char lbuf[64];
                snprintf(lbuf, sizeof(lbuf), "%s:%u", baseFile, lineInfo.LineNumber);
                res = lbuf;
            } else {
                res = fallbackSym;
            }
            lineCache[addr] = res;
            return res;
        };

        std::unordered_map<DWORD, CgThreadAgg> threadAggs;
        uint64_t totalProcessSamples = 0;
        uint64_t mainStackWalkCount = 0;
        uint64_t mainSamplesCount = 0;
        std::unordered_map<std::string, uint64_t> mainInclusiveCounts;
        std::unordered_map<std::string, uint64_t> mainLineCounts;

        std::unordered_map<DWORD, HANDLE> threadHandles;
        auto lastRefresh = std::chrono::steady_clock::now();
        auto lastDump = std::chrono::steady_clock::now();
        auto startTime = std::chrono::steady_clock::now();

        auto refreshThreads = [&]() {
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap == INVALID_HANDLE_VALUE) return;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            std::vector<DWORD> foundTids;
            if (Thread32First(hSnap, &te)) {
                do {
                    if (te.th32OwnerProcessID == currentPid && te.th32ThreadID != samplerTid) {
                        foundTids.push_back(te.th32ThreadID);
                        if (threadHandles.find(te.th32ThreadID) == threadHandles.end()) {
                            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                            if (h) {
                                threadHandles[te.th32ThreadID] = h;
                            }
                        }
                    }
                } while (Thread32Next(hSnap, &te));
            }
            CloseHandle(hSnap);

            for (auto it = threadHandles.begin(); it != threadHandles.end(); ) {
                if (std::find(foundTids.begin(), foundTids.end(), it->first) == foundTids.end()) {
                    CloseHandle(it->second);
                    it = threadHandles.erase(it);
                } else {
                    ++it;
                }
            }
        };

        refreshThreads();

        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRefresh).count() >= 250) {
                lastRefresh = now;
                refreshThreads();
            }

            DWORD mainTid = s_cgMainGuestThreadId.load(std::memory_order_relaxed);

            for (auto it = threadHandles.begin(); it != threadHandles.end(); ) {
                DWORD tid = it->first;
                HANDLE hThread = it->second;

                if (SuspendThread(hThread) == (DWORD)-1) {
                    CloseHandle(hThread);
                    it = threadHandles.erase(it);
                    continue;
                }

                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                bool gotContext = (GetThreadContext(hThread, &ctx) != 0);
                DWORD64 rip = gotContext ? ctx.Rip : 0;

                std::vector<DWORD64> frames;
                if (gotContext && mainTid != 0 && tid == mainTid) {
                    STACKFRAME64 sf{};
                    sf.AddrPC.Offset = ctx.Rip;
                    sf.AddrPC.Mode = AddrModeFlat;
                    sf.AddrStack.Offset = ctx.Rsp;
                    sf.AddrStack.Mode = AddrModeFlat;
                    sf.AddrFrame.Offset = ctx.Rbp;
                    sf.AddrFrame.Mode = AddrModeFlat;
                    CONTEXT walkCtx = ctx;
                    for (int f = 0; f < 6; ++f) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProc, hThread, &sf, &walkCtx,
                                        NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
                            break;
                        }
                        if (sf.AddrPC.Offset == 0) break;
                        frames.push_back(sf.AddrPC.Offset);
                    }
                }

                ResumeThread(hThread);

                if (gotContext && rip != 0) {
                    std::string leafSym = resolveSym(rip);
                    CgThreadAgg &tagg = threadAggs[tid];
                    tagg.totalSamples++;
                    tagg.leafCounts[leafSym]++;
                    totalProcessSamples++;

                    if (mainTid != 0 && tid == mainTid) {
                        mainSamplesCount++;
                        std::string leafLine = resolveLine(rip, leafSym);
                        mainLineCounts[leafLine]++;

                        mainStackWalkCount++;
                        std::vector<std::string> frameSyms;
                        frameSyms.push_back(leafSym);
                        for (size_t i = 0; i < frames.size() && frameSyms.size() < 3; ++i) {
                            std::string fsym = resolveSym(frames[i]);
                            if (std::find(frameSyms.begin(), frameSyms.end(), fsym) == frameSyms.end()) {
                                frameSyms.push_back(fsym);
                            }
                        }
                        for (size_t i = 0; i < frameSyms.size(); ++i) {
                            mainInclusiveCounts[frameSyms[i]]++;
                        }
                    }
                }
                ++it;
            }

            now = std::chrono::steady_clock::now();
            double elapsedSinceDump = std::chrono::duration<double>(now - lastDump).count();
            if (elapsedSinceDump >= 5.0) {
                lastDump = now;
                uint32_t tSec = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count());

                if (totalProcessSamples > 0) {
                    for (const auto &pair : threadAggs) {
                        DWORD tid = pair.first;
                        const CgThreadAgg &tagg = pair.second;
                        double threadPct = (static_cast<double>(tagg.totalSamples) * 100.0) / static_cast<double>(totalProcessSamples);
                        if (threadPct > 5.0) {
                            std::vector<std::pair<std::string, uint64_t>> sortedLeaf(tagg.leafCounts.begin(), tagg.leafCounts.end());
                            std::sort(sortedLeaf.begin(), sortedLeaf.end(), [](const std::pair<std::string, uint64_t> &a, const std::pair<std::string, uint64_t> &b) {
                                return a.second > b.second;
                            });

                            std::string topStr;
                            size_t limit = (std::min)(size_t(12), sortedLeaf.size());
                            for (size_t i = 0; i < limit; ++i) {
                                double symPct = (static_cast<double>(sortedLeaf[i].second) * 100.0) / static_cast<double>(tagg.totalSamples);
                                char sbuf[128];
                                snprintf(sbuf, sizeof(sbuf), "%s=%.1f%% ", sortedLeaf[i].first.c_str(), symPct);
                                topStr += sbuf;
                            }
                            if (!topStr.empty() && topStr.back() == ' ') topStr.pop_back();

                            bhLog("[prof2] t=%u thread=%u samples=%llu top: %s\n",
                                  tSec, (unsigned)tid, (unsigned long long)tagg.totalSamples, topStr.c_str());
                        }
                    }

                    if (mainSamplesCount > 0 && !mainLineCounts.empty()) {
                        std::vector<std::pair<std::string, uint64_t>> sortedLines(mainLineCounts.begin(), mainLineCounts.end());
                        std::sort(sortedLines.begin(), sortedLines.end(), [](const std::pair<std::string, uint64_t> &a, const std::pair<std::string, uint64_t> &b) {
                            return a.second > b.second;
                        });

                        std::string linesStr;
                        size_t limit = (std::min)(size_t(15), sortedLines.size());
                        for (size_t i = 0; i < limit; ++i) {
                            double linePct = (static_cast<double>(sortedLines[i].second) * 100.0) / static_cast<double>(mainSamplesCount);
                            char sbuf[128];
                            snprintf(sbuf, sizeof(sbuf), "%s=%.1f%% ", sortedLines[i].first.c_str(), linePct);
                            linesStr += sbuf;
                        }
                        if (!linesStr.empty() && linesStr.back() == ' ') linesStr.pop_back();

                        bhLog("[prof2] main lines: %s\n", linesStr.c_str());
                    }

                    if (mainStackWalkCount > 0) {
                        std::vector<std::pair<std::string, uint64_t>> sortedInc(mainInclusiveCounts.begin(), mainInclusiveCounts.end());
                        std::sort(sortedInc.begin(), sortedInc.end(), [](const std::pair<std::string, uint64_t> &a, const std::pair<std::string, uint64_t> &b) {
                            return a.second > b.second;
                        });

                        std::string incStr;
                        size_t limit = (std::min)(size_t(12), sortedInc.size());
                        for (size_t i = 0; i < limit; ++i) {
                            double incPct = (static_cast<double>(sortedInc[i].second) * 100.0) / static_cast<double>(mainStackWalkCount);
                            char sbuf[128];
                            snprintf(sbuf, sizeof(sbuf), "%s=%.1f%% ", sortedInc[i].first.c_str(), incPct);
                            incStr += sbuf;
                        }
                        if (!incStr.empty() && incStr.back() == ' ') incStr.pop_back();

                        bhLog("[prof2] main inclusive: %s\n", incStr.c_str());
                    }
                }

                threadAggs.clear();
                mainInclusiveCounts.clear();
                mainLineCounts.clear();
                totalProcessSamples = 0;
                mainStackWalkCount = 0;
                mainSamplesCount = 0;
            }

            Sleep(1);
        }
    }

    void cgStartSamplerIfEnabled() {
        const char *env = std::getenv("PS2X_PROF_SAMPLE");
        if (!env || (env[0] != '1' && env[0] != 't' && env[0] != 'T')) return;

        bool expected = false;
        if (!s_cgSamplerStarted.compare_exchange_strong(expected, true)) return;

        std::thread samplerThread(cgSamplerWorker);
        samplerThread.detach();
    }
#else
    void recordMainGuestThread() {}
    void cgStartSamplerIfEnabled() {}
#endif
}
