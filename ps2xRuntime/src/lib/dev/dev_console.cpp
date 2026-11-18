// FEAT-DF: Developer console v1 - command file + warp <stage> [spawn]
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_dcall.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-DF
extern std::atomic<int> g_mcPendingChoice;     // FEAT-DF (defined in RPC.cpp, FEAT-DE)

namespace
{

constexpr uint32_t kAreaUpdateAddr = 0x004DE1A4u; // FEAT-DF
constexpr uint32_t kNewAreaSceneFn = 0x0020C378u; // FEAT-DF
constexpr uint32_t kTransitionFn = 0x00208BF4u;   // FEAT-DF
constexpr uint32_t kSceneMgrGlobal = 0x00635530u; // FEAT-DF
constexpr uint32_t kAreaVtable = 0x006ADE10u;     // FEAT-DF

struct PendingWarp // FEAT-DF
{
    bool active = false;
    char stage[32]{};
    char spawn[32]{};
};

std::mutex s_warpMutex;                       // FEAT-DF
PendingWarp s_pendingWarp;                    // FEAT-DF
std::atomic<bool> s_warpPending{false};       // FEAT-DF
std::atomic<bool> s_consoleInstalled{false};  // FEAT-DF

PS2Runtime::RecompiledFunction s_origAreaUpdate = nullptr; // FEAT-DF

enum class WarpState : uint8_t // FEAT-DF
{
    Idle = 0,
    CallingC378 = 1,
    Calling8BF4 = 2
};

WarpState s_warpState = WarpState::Idle; // FEAT-DF
uint32_t s_savedEntryRa = 0;             // FEAT-DF
uint32_t s_savedSp = 0;                  // FEAT-DF
char s_activeStage[32]{};                // FEAT-DF
char s_activeSpawn[32]{};                // FEAT-DF

void copyZeroPadded32(char (&dst)[32], const std::string &src) // FEAT-DF
{
    std::memset(dst, 0, sizeof(dst));
    const size_t n = (src.size() < 31u) ? src.size() : 31u;
    if (n > 0u)
    {
        std::memcpy(dst, src.data(), n);
    }
}

std::string readGuestCString32(const uint8_t *rdram, uint32_t addr) // FEAT-DF
{
    if (!rdram || addr == 0u || addr + 32u > PS2_RAM_SIZE)
    {
        return {};
    }
    char buf[32]{};
    std::memcpy(buf, rdram + addr, 31u);
    buf[31] = '\0';
    return std::string(buf);
}

void handleWhereCommand(PS2Runtime &rt) // FEAT-DF
{
    const uint8_t *rdram = rt.memory().getRDRAM();
    uint32_t scene = 0u;
    uint32_t vtable = 0u;
    std::string stage;
    std::string spawn;

    if (rdram && kSceneMgrGlobal + 4u <= PS2_RAM_SIZE)
    {
        std::memcpy(&scene, rdram + kSceneMgrGlobal, sizeof(scene));
    }
    if (rdram && scene != 0u && scene + 4u <= PS2_RAM_SIZE)
    {
        std::memcpy(&vtable, rdram + scene, sizeof(vtable));
        if (vtable == kAreaVtable && scene + 0x50u <= PS2_RAM_SIZE)
        {
            stage = readGuestCString32(rdram, scene + 0x10u);
            spawn = readGuestCString32(rdram, scene + 0x30u);
        }
    }

    ps2xSafeLog("[dev] where scene=0x%08x vtable=0x%08x stage='%s' spawn='%s'\n",
                scene, vtable, stage.c_str(), spawn.c_str());
}

void executeCommandLine(PS2Runtime &rt, const std::string &rawLine) // FEAT-DF
{
    const size_t first = rawLine.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return;
    }
    const size_t last = rawLine.find_last_not_of(" \t\r\n");
    const std::string line = rawLine.substr(first, last - first + 1u);

    ps2xSafeLog("[dev] cmd '%s'\n", line.c_str());

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "warp")
    {
        std::string stage;
        std::string spawn;
        if (!(iss >> stage))
        {
            ps2xSafeLog("[dev] unknown\n");
            return;
        }
        iss >> spawn; // optional; empty string if omitted

        {
            std::lock_guard<std::mutex> lock(s_warpMutex);
            copyZeroPadded32(s_pendingWarp.stage, stage);
            copyZeroPadded32(s_pendingWarp.spawn, spawn);
            s_pendingWarp.active = true;
            s_warpPending.store(true, std::memory_order_release);
        }
        return;
    }

    if (cmd == "mc")
    {
        std::string choice;
        if (iss >> choice)
        {
            if (choice == "boy")
            {
                g_mcPendingChoice.store(0, std::memory_order_relaxed);
                return;
            }
            if (choice == "girl")
            {
                g_mcPendingChoice.store(1, std::memory_order_relaxed);
                return;
            }
        }
        ps2xSafeLog("[dev] unknown\n");
        return;
    }

    if (cmd == "where")
    {
        handleWhereCommand(rt);
        return;
    }

    ps2xSafeLog("[dev] unknown\n");
}

void commandThreadLoop(PS2Runtime *rt) // FEAT-DF
{
    const char *envPath = std::getenv("PS2X_DEV_CMD");
    const std::string cmdPath = (envPath && envPath[0] != '\0') ? std::string(envPath) : std::string("logs\\dev_cmd.txt");
    uint64_t offset = 0u;

    while (!rt->isStopRequested())
    {
        std::error_code ec;
        if (!std::filesystem::exists(cmdPath, ec) || ec)
        {
            offset = 0u;
        }
        else
        {
            const uint64_t fileSize = std::filesystem::file_size(cmdPath, ec);
            if (!ec)
            {
                if (fileSize < offset)
                {
                    offset = 0u;
                }
                if (fileSize > offset)
                {
                    FILE *f = std::fopen(cmdPath.c_str(), "rb");
                    if (f)
                    {
                        if (std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0)
                        {
                            const size_t toRead = static_cast<size_t>(fileSize - offset);
                            std::string chunk(toRead, '\0');
                            const size_t nRead = std::fread(&chunk[0], 1u, toRead, f);
                            chunk.resize(nRead);
                            offset += static_cast<uint64_t>(nRead);

                            std::istringstream lines(chunk);
                            std::string line;
                            while (std::getline(lines, line))
                            {
                                executeCommandLine(*rt, line);
                            }
                        }
                        std::fclose(f);
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void devConsoleAreaUpdateWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FEAT-DF
{
    if (s_warpState == WarpState::CallingC378 && ctx->pc == kAreaUpdateAddr)
    {
        goto resume_after_c378;
    }
    if (s_warpState == WarpState::Calling8BF4 && ctx->pc == kAreaUpdateAddr)
    {
        goto resume_after_8bf4;
    }

    // FEAT-DF2: only treat as a resume of the original when ctx->pc points strictly inside its body
    // (stale pc values from the caller on a fresh call used to make us skip the warp entirely).
    if (ctx->pc > kAreaUpdateAddr && ctx->pc < kAreaUpdateAddr + 0x800u)
    {
        s_origAreaUpdate(rdram, ctx, runtime);
        return;
    }

    {
        static std::atomic<uint32_t> s_updPendLogs{0}; // FEAT-DF3
        const uint32_t inPc = ctx->pc; // FEAT-DF3
        const uint32_t entryRa = GPR_U32(ctx, 31);
        const bool pend = s_warpPending.load(std::memory_order_acquire); // FEAT-DF3
        if (!pend)
        {
            s_origAreaUpdate(rdram, ctx, runtime); // FEAT-DF3: no warp pending, plain passthrough
            return;
        }
        if (s_updPendLogs.fetch_add(1u) < 20u) // FEAT-DF3
        {
            ps2xSafeLog("[dev] upd(pending) in_pc=0x%08x ra=0x%08x state=%d\n",
                        inPc, entryRa, static_cast<int>(s_warpState));
        }

        {
            std::lock_guard<std::mutex> lock(s_warpMutex);
            if (!s_pendingWarp.active)
            {
                s_warpPending.store(false, std::memory_order_release);
                s_origAreaUpdate(rdram, ctx, runtime); // FEAT-DF3
                return;
            }
            std::memcpy(s_activeStage, s_pendingWarp.stage, sizeof(s_activeStage));
            std::memcpy(s_activeSpawn, s_pendingWarp.spawn, sizeof(s_activeSpawn));
        }

        s_savedEntryRa = entryRa;
        s_savedSp = GPR_U32(ctx, 29);
        const uint32_t sp = s_savedSp - 0x80u;
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(sp));

        for (uint32_t i = 0u; i < 32u; ++i)
        {
            WRITE8(sp + i, static_cast<uint8_t>(s_activeStage[i]));
            WRITE8(sp + 0x20u + i, static_cast<uint8_t>(s_activeSpawn[i]));
        }

        SET_GPR_S32(ctx, 4, static_cast<int32_t>(sp));
        SET_GPR_S32(ctx, 5, static_cast<int32_t>(sp + 0x20u));
        SET_GPR_U32(ctx, 31, kAreaUpdateAddr);
        ctx->pc = kNewAreaSceneFn;
        s_warpState = WarpState::CallingC378;
        if (!PS2_DCALL(kNewAreaSceneFn, kAreaUpdateAddr))
        {
            return;
        }
    }

resume_after_c378:
    {
        const uint32_t obj = GPR_U32(ctx, 2);
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(obj));
        SET_GPR_U32(ctx, 31, kAreaUpdateAddr);
        ctx->pc = kTransitionFn;
        s_warpState = WarpState::Calling8BF4;
        if (!PS2_DCALL(kTransitionFn, kAreaUpdateAddr))
        {
            return;
        }
    }

resume_after_8bf4:
    {
        s_warpState = WarpState::Idle;
        const uint32_t resObj = GPR_U32(ctx, 2);
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(s_savedSp));
        ctx->r[2] = _mm_set_epi32(0, 0, 0, static_cast<int32_t>(resObj));
        SET_GPR_U32(ctx, 31, s_savedEntryRa);
        ctx->pc = s_savedEntryRa;

        {
            std::lock_guard<std::mutex> lock(s_warpMutex);
            s_pendingWarp.active = false;
            s_warpPending.store(false, std::memory_order_release);
        }

        ps2xSafeLog("[dev] warp -> '%s' spawn '%s' obj=0x%08x\n", s_activeStage, s_activeSpawn, resObj);
    }
}

} // namespace

void ps2xDevConsoleInstall(PS2Runtime &rt) // FEAT-DF
{
    if (s_consoleInstalled.exchange(true))
    {
        return;
    }

    s_origAreaUpdate = rt.lookupFunction(kAreaUpdateAddr);
    if (s_origAreaUpdate)
    {
        rt.replaceFunction(kAreaUpdateAddr, &devConsoleAreaUpdateWrapper);
    }

    std::thread(commandThreadLoop, &rt).detach();
}
