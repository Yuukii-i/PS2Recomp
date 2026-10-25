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

        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char *lastSlash = strrchr(exePath, '\\');
        char searchPath[MAX_PATH * 2] = {};
        if (lastSlash) {
            *lastSlash = '\0';
            snprintf(searchPath, sizeof(searchPath), ".;%s", exePath);
        }
        SymInitialize(hProc, searchPath[0] ? searchPath : nullptr, TRUE);

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
                IMAGEHLP_MODULE64 modInfo{};
                modInfo.SizeOfStruct = sizeof(modInfo);
                if (SymGetModuleInfo64(hProc, addr, &modInfo)) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s+0x%llX", modInfo.ModuleName, (unsigned long long)(addr - modInfo.BaseOfImage));
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

        std::unordered_map<DWORD, CgThreadAgg> threadAggs;
        uint64_t totalProcessSamples = 0;
        uint64_t mainStackWalkCount = 0;
        std::unordered_map<std::string, uint64_t> mainInclusiveCounts;

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
                totalProcessSamples = 0;
                mainStackWalkCount = 0;
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
