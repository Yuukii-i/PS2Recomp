#pragma once

#include <chrono>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "raylib.h"

// Forward externs from other translation units (no header edits)
extern void ps2xSafeLog(const char *fmt, ...);
extern std::atomic<uint32_t> g_diagSg2Fno0Calls;
extern std::atomic<uint32_t> g_diagDbcBufferWrites;
extern std::atomic<uint32_t> g_diagCid1Requests;
extern std::atomic<uint64_t> g_diagCid1BytesServed;
extern void diagGetLastCid1Path(char *out, size_t maxLen);
extern std::atomic<uint32_t> g_diagMcCalls;
extern void gsGetAndResetPrimsPerFbp(std::vector<std::pair<uint32_t, uint32_t>> &out);
extern std::atomic<uint32_t> g_diagMscalCount;
extern std::atomic<uint32_t> g_diagGifPacketCount;
extern void ps2xDumpVramAndState(const char *triggerName, uint64_t tick, void *rtVoid);

namespace ps2_diag
{
    constexpr uint32_t FB_WIDTH = 640u;
    constexpr uint32_t FB_HEIGHT = 512u;
    constexpr uint32_t DEFAULT_DISPLAY_HEIGHT = 448u;

    class DiagSampler
    {
    public:
        static void onFrame(PS2Runtime *rt,
                            uint64_t currentTick,
                            const std::vector<uint8_t> &uploadBuffer,
                            uint32_t displayFbp,
                            uint32_t sourceFbp,
                            uint32_t width,
                            uint32_t height)
        {
            if (!rt) return;

            const auto now = std::chrono::steady_clock::now();
            static auto s_initTime = now;
            static auto s_lastStatsTime = now;
            static auto s_lastSamplerTime = now;

            static bool s_envChecked = false;
            static const char *s_frameDumpDir = nullptr;
            static uint32_t s_dumpEvery = 60u;
            static uint32_t s_dumpMax = 20u;
            static bool s_enableStats = false;
            static bool s_enableSampler = false;
            static const char *s_vramDumpTrigger = nullptr;
            static bool s_vramDumpDone = false;
            static uint64_t s_targetTick = 0;
            static bool s_isTickTrigger = false;
            static bool s_isMcTrigger = false;
            static std::string s_cid1Substr;
            static bool s_isCrossTrigger = false;

            if (!s_envChecked)
            {
                s_envChecked = true;
                const char *dumpEnv = std::getenv("PS2X_FRAME_DUMP");
                if (dumpEnv && dumpEnv[0] != '\0')
                {
                    s_frameDumpDir = dumpEnv;
                    const char *everyEnv = std::getenv("PS2X_FRAME_DUMP_EVERY");
                    if (everyEnv && everyEnv[0] != '\0')
                    {
                        try { s_dumpEvery = std::stoul(everyEnv); } catch (...) {}
                    }
                    const char *maxEnv = std::getenv("PS2X_FRAME_DUMP_MAX");
                    if (maxEnv && maxEnv[0] != '\0')
                    {
                        try { s_dumpMax = std::stoul(maxEnv); } catch (...) {}
                    }
                }
                const char *statsEnv = std::getenv("PS2X_STATS");
                if (statsEnv && (std::strcmp(statsEnv, "1") == 0 || std::strcmp(statsEnv, "true") == 0))
                {
                    s_enableStats = true;
                }
                const char *samplerEnv = std::getenv("PS2X_SAMPLER");
                if (samplerEnv && (std::strcmp(samplerEnv, "1") == 0 || std::strcmp(samplerEnv, "true") == 0))
                {
                    s_enableSampler = true;
                }
                const char *vramDumpEnv = std::getenv("PS2X_GS_VRAM_DUMP");
                if (vramDumpEnv && vramDumpEnv[0] != '\0')
                {
                    s_vramDumpTrigger = vramDumpEnv;
                    if (std::strncmp(vramDumpEnv, "tick:", 5) == 0)
                    {
                        s_isTickTrigger = true;
                        try { s_targetTick = std::stoull(vramDumpEnv + 5); } catch (...) {}
                    }
                    else if (std::strcmp(vramDumpEnv, "mc") == 0)
                    {
                        s_isMcTrigger = true;
                    }
                    else if (std::strncmp(vramDumpEnv, "cid1:", 5) == 0)
                    {
                        s_cid1Substr = vramDumpEnv + 5;
                    }
                    else if (std::strcmp(vramDumpEnv, "cross") == 0)
                    {
                        s_isCrossTrigger = true;
                    }
                }
            }

            if (s_vramDumpTrigger && !s_vramDumpDone)
            {
                bool triggerFired = false;
                if (s_isTickTrigger && currentTick >= s_targetTick)
                {
                    triggerFired = true;
                }
                else if (s_isMcTrigger && g_diagMcCalls.load(std::memory_order_relaxed) > 0u)
                {
                    triggerFired = true;
                }
                else if (!s_cid1Substr.empty())
                {
                    char lastPath[256] = {0};
                    diagGetLastCid1Path(lastPath, sizeof(lastPath));
                    if (std::strstr(lastPath, s_cid1Substr.c_str()) != nullptr)
                    {
                        triggerFired = true;
                    }
                }
                else if (s_isCrossTrigger)
                {
                    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_initTime).count();
                    if (elapsedMs >= 25000)
                    {
                        triggerFired = true;
                    }
                }

                if (triggerFired)
                {
                    s_vramDumpDone = true;
                    ps2xDumpVramAndState(s_vramDumpTrigger, currentTick, rt);
                }
            }

            // Calculate current frame hash (FNV-1a)
            uint32_t windowHash = 2166136261u;
            const uint8_t *rawBytes = uploadBuffer.data();
            const size_t byteCount = uploadBuffer.size();
            for (size_t bi = 0; bi < byteCount; bi += 32)
            {
                windowHash = (windowHash ^ rawBytes[bi]) * 16777619u;
            }

            // Fraction of non-black pixels (any channel > 8)
            uint32_t nonBlackCount = 0u;
            const size_t totalPixels = byteCount / 4u;
            for (size_t pi = 0; pi < totalPixels; ++pi)
            {
                const uint8_t r = uploadBuffer[pi * 4u + 0];
                const uint8_t g = uploadBuffer[pi * 4u + 1];
                const uint8_t b = uploadBuffer[pi * 4u + 2];
                if (r > 8u || g > 8u || b > 8u)
                {
                    ++nonBlackCount;
                }
            }
            const double nonBlackFraction = (totalPixels > 0u) ? (static_cast<double>(nonBlackCount) / static_cast<double>(totalPixels)) : 0.0;

            // --- 1.a: Frame Dumping ---
            if (s_frameDumpDir)
            {
                static uint32_t s_frameCounter = 0u;
                static uint32_t s_dumpsCompleted = 0u;
                s_frameCounter++;

                if (s_dumpsCompleted < s_dumpMax && (s_frameCounter % s_dumpEvery == 0u))
                {
                    std::error_code ec;
                    std::filesystem::create_directories(s_frameDumpDir, ec);

                    // (i) Window frame
                    char fn[512];
                    std::snprintf(fn, sizeof(fn), "%s/%llu_window_fbp%u_0x%08x.png",
                                  s_frameDumpDir, static_cast<unsigned long long>(currentTick), displayFbp, windowHash);
                    Image img{};
                    img.data = const_cast<uint8_t *>(uploadBuffer.data());
                    img.width = static_cast<int>(FB_WIDTH);
                    img.height = (height != 0u) ? static_cast<int>(std::min<uint32_t>(height, FB_HEIGHT)) : static_cast<int>(DEFAULT_DISPLAY_HEIGHT);
                    img.mipmaps = 1;
                    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                    ExportImage(img, fn);

                    // (ii) Raw framebuffer for each distinct FBP among FRAME_1, FRAME_2, DISPFB1, DISPFB2
                    const auto &regs = rt->memory().gs();
                    const auto &frame1 = rt->gs().getContextFrame(0);
                    const auto &frame2 = rt->gs().getContextFrame(1);
                    const uint32_t dispfb1_fbp = static_cast<uint32_t>(regs.dispfb1 & 0x1FFu);
                    const uint32_t dispfb1_fbw = static_cast<uint32_t>((regs.dispfb1 >> 9u) & 0x3Fu);
                    const uint8_t dispfb1_psm  = static_cast<uint8_t>((regs.dispfb1 >> 15u) & 0x1Fu);
                    const uint32_t dispfb2_fbp = static_cast<uint32_t>(regs.dispfb2 & 0x1FFu);
                    const uint32_t dispfb2_fbw = static_cast<uint32_t>((regs.dispfb2 >> 9u) & 0x3Fu);
                    const uint8_t dispfb2_psm  = static_cast<uint8_t>((regs.dispfb2 >> 15u) & 0x1Fu);

                    struct Candidate {
                        const char *which;
                        uint32_t fbp;
                        uint32_t fbw;
                        uint8_t psm;
                    };
                    const Candidate cands[4] = {
                        {"frame1", frame1.fbp, frame1.fbw, frame1.psm},
                        {"frame2", frame2.fbp, frame2.fbw, frame2.psm},
                        {"dispfb1", dispfb1_fbp, dispfb1_fbw, dispfb1_psm},
                        {"dispfb2", dispfb2_fbp, dispfb2_fbw, dispfb2_psm}
                    };

                    std::unordered_set<uint32_t> dumpedFbps;
                    for (const auto &c : cands)
                    {
                        if (dumpedFbps.count(c.fbp) != 0)
                        {
                            continue;
                        }
                        dumpedFbps.insert(c.fbp);

                        if (c.psm != 0u /* GS_PSM_CT32 */)
                        {
                            ps2xSafeLog("[frame:dump] %s FBP %u has PSM 0x%02x (expected PSMCT32 0x00)\n",
                                        c.which, c.fbp, static_cast<unsigned>(c.psm));
                            std::fflush(stderr);
                        }

                        const uint32_t fbw = (c.fbw != 0u) ? c.fbw : 10u;
                        const uint32_t rawW = fbw * 64u;
                        const uint32_t rawH = 448u;
                        std::vector<uint32_t> rawBuf(static_cast<size_t>(rawW) * rawH, 0xFF000000u);
                        const uint32_t baseBlock = c.fbp * 32u;
                        for (uint32_t ry = 0; ry < rawH; ++ry)
                        {
                            for (uint32_t rx = 0; rx < rawW; ++rx)
                            {
                                const uint32_t c32 = rt->gs().ReadVram(c.psm, baseBlock, fbw, rx, ry);
                                const uint8_t r = static_cast<uint8_t>(c32 & 0xFFu);
                                const uint8_t g = static_cast<uint8_t>((c32 >> 8u) & 0xFFu);
                                const uint8_t b = static_cast<uint8_t>((c32 >> 16u) & 0xFFu);
                                rawBuf[ry * rawW + rx] = static_cast<uint32_t>(r) |
                                                         (static_cast<uint32_t>(g) << 8u) |
                                                         (static_cast<uint32_t>(b) << 16u) |
                                                         0xFF000000u;
                            }
                        }

                        uint32_t rawHash = 2166136261u;
                        const uint8_t *rawP = reinterpret_cast<const uint8_t *>(rawBuf.data());
                        const size_t rawBytesCount = rawBuf.size() * 4u;
                        for (size_t bi = 0; bi < rawBytesCount; bi += 32)
                        {
                            rawHash = (rawHash ^ rawP[bi]) * 16777619u;
                        }

                        char rfn[512];
                        std::snprintf(rfn, sizeof(rfn), "%s/%llu_%s_fbp%u_0x%08x.png",
                                      s_frameDumpDir, static_cast<unsigned long long>(currentTick), c.which, c.fbp, rawHash);
                        Image rimg{};
                        rimg.data = rawBuf.data();
                        rimg.width = static_cast<int>(rawW);
                        rimg.height = static_cast<int>(rawH);
                        rimg.mipmaps = 1;
                        rimg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                        ExportImage(rimg, rfn);
                    }

                    s_dumpsCompleted++;
                    ps2xSafeLog("[frame:dump] Completed dump %u/%u at tick %llu\n",
                                s_dumpsCompleted, s_dumpMax, static_cast<unsigned long long>(currentTick));
                    std::fflush(stderr);
                }
            }

            // --- 1.b: Stats Tracking ---
            static uint32_t s_statsFramesInSec = 0u;
            static std::unordered_set<uint32_t> s_statsHashesInSec;
            static uint64_t s_lastVsyncTick = rt->eeScheduler().currentVSyncTick();
            static uint32_t s_lastGifCount = 0u;

            s_statsFramesInSec++;
            s_statsHashesInSec.insert(windowHash);

            if (s_enableStats && (now - s_lastStatsTime >= std::chrono::seconds(1)))
            {
                const uint64_t elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - s_initTime).count();
                const uint64_t curVsync = rt->eeScheduler().currentVSyncTick();
                const uint64_t vblanksPerSec = (curVsync >= s_lastVsyncTick) ? (curVsync - s_lastVsyncTick) : 0u;
                s_lastVsyncTick = curVsync;

                const uint32_t curGif = g_diagGifPacketCount.load(std::memory_order_relaxed);
                const uint32_t gifPerSec = (curGif >= s_lastGifCount) ? (curGif - s_lastGifCount) : 0u;
                s_lastGifCount = curGif;

                std::vector<std::pair<uint32_t, uint32_t>> primsPerFbp;
                gsGetAndResetPrimsPerFbp(primsPerFbp);
                std::string primsStr;
                for (size_t i = 0; i < primsPerFbp.size(); ++i)
                {
                    if (i > 0) primsStr += ",";
                    primsStr += "fbp" + std::to_string(primsPerFbp[i].first) + ":" + std::to_string(primsPerFbp[i].second);
                }
                if (primsStr.empty()) primsStr = "none";

                const auto &regs = rt->memory().gs();
                const auto &frame1 = rt->gs().getContextFrame(0);
                const auto &frame2 = rt->gs().getContextFrame(1);
                const uint32_t dispfb1_fbp = static_cast<uint32_t>(regs.dispfb1 & 0x1FFu);
                const uint32_t dispfb1_fbw = static_cast<uint32_t>((regs.dispfb1 >> 9u) & 0x3Fu);
                const uint8_t dispfb1_psm  = static_cast<uint8_t>((regs.dispfb1 >> 15u) & 0x1Fu);
                const uint32_t dispfb2_fbp = static_cast<uint32_t>(regs.dispfb2 & 0x1FFu);
                const uint32_t dispfb2_fbw = static_cast<uint32_t>((regs.dispfb2 >> 9u) & 0x3Fu);
                const uint8_t dispfb2_psm  = static_cast<uint8_t>((regs.dispfb2 >> 15u) & 0x1Fu);

                const uint32_t dw1 = static_cast<uint32_t>((regs.display1 >> 32u) & 0x0FFFu);
                const uint32_t dh1 = static_cast<uint32_t>((regs.display1 >> 44u) & 0x07FFu);
                const uint32_t magh1 = static_cast<uint32_t>((regs.display1 >> 23u) & 0x0Fu);
                const uint32_t dw2 = static_cast<uint32_t>((regs.display2 >> 32u) & 0x0FFFu);
                const uint32_t dh2 = static_cast<uint32_t>((regs.display2 >> 44u) & 0x07FFu);
                const uint32_t magh2 = static_cast<uint32_t>((regs.display2 >> 23u) & 0x0Fu);

                const uint32_t mscalCount = g_diagMscalCount.exchange(0u, std::memory_order_relaxed);

                ps2xSafeLog("[stats] sec=%llu fps=%u vblanks=%llu gif=%u prims=[%s] FRAME_1.fbp=%u FRAME_2.fbp=%u DISPFB1=(%u,%u,0x%02x) DISPFB2=(%u,%u,0x%02x) PMODE=0x%04llx DISPLAY1=(%u,%u,%u) DISPLAY2=(%u,%u,%u) hashes=%zu non_black=%.2f%% mscal=%u xgkick=0 mpg=0 unk_op=0 unsupp_gs=0\n",
                            static_cast<unsigned long long>(elapsedSec),
                            s_statsFramesInSec,
                            static_cast<unsigned long long>(vblanksPerSec),
                            gifPerSec,
                            primsStr.c_str(),
                            frame1.fbp, frame2.fbp,
                            dispfb1_fbp, dispfb1_fbw, dispfb1_psm,
                            dispfb2_fbp, dispfb2_fbw, dispfb2_psm,
                            static_cast<unsigned long long>(regs.pmode),
                            dw1, dh1, magh1,
                            dw2, dh2, magh2,
                            s_statsHashesInSec.size(),
                            nonBlackFraction * 100.0,
                            mscalCount);
                std::fflush(stderr);

                s_statsFramesInSec = 0u;
                s_statsHashesInSec.clear();
                s_lastStatsTime = now;
            }

            // --- 1.c: Sampler Tracking ---
            if (s_enableSampler && (now - s_lastSamplerTime >= std::chrono::seconds(1)))
            {
                const uint64_t elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - s_initTime).count();
                ps2xSafeLog("[sampler] sec=%llu\n", static_cast<unsigned long long>(elapsedSec));

                // 1. Threads
                const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
                ps2xSafeLog("  threads (running=%d, total=%zu):\n", snap.runningThreadId, snap.threads.size());
                for (const auto &th : snap.threads)
                {
                    const char *statusStr = "UNKNOWN";
                    switch (th.status)
                    {
                    case EeThreadStatus::Running: statusStr = "RUNNING"; break;
                    case EeThreadStatus::Ready: statusStr = "READY"; break;
                    case EeThreadStatus::Waiting: statusStr = "WAITING"; break;
                    case EeThreadStatus::WaitingSuspended: statusStr = "WAITING_SUSPENDED"; break;
                    case EeThreadStatus::Suspended: statusStr = "SUSPENDED"; break;
                    case EeThreadStatus::Dormant: statusStr = "DORMANT"; break;
                    }
                    const char *waitStr = "None";
                    switch (th.waitReason)
                    {
                    case EeWaitReason::None: waitStr = "None"; break;
                    case EeWaitReason::Sleep: waitStr = "Sleep"; break;
                    case EeWaitReason::Semaphore: waitStr = "Semaphore"; break;
                    case EeWaitReason::EventFlag: waitStr = "EventFlag"; break;
                    case EeWaitReason::VSync: waitStr = "VSync"; break;
                    case EeWaitReason::External: waitStr = "External"; break;
                    case EeWaitReason::Mpeg: waitStr = "Mpeg"; break;
                    }
                    ps2xSafeLog("    Thread id=%d prio=%d status=%s pc=0x%08X ra=0x%08X sp=0x%08X wait=%s (id=%d) invocations=%u\n",
                                th.id, th.currentPriority, statusStr, th.pc, th.ra, th.sp, waitStr, th.waitId, th.invocationDepth);
                }

                // 2. Memory addresses
                const uint8_t *rdram = rt->memory().m_rdram;
                if (rdram)
                {
                    const uint32_t val_635544 = (0x635544 + 4 <= PS2_RAM_SIZE)
                                                    ? *reinterpret_cast<const uint32_t *>(rdram + 0x635544) : 0u;
                    const uint8_t val_649db0_19 = (0x649DB0 + 0x19 < PS2_RAM_SIZE)
                                                      ? *(rdram + 0x649DB0 + 0x19) : 0u;
                    ps2xSafeLog("  memory: *(0x635544)=0x%08X *(0x649DB0+0x19)=0x%02X\n", val_635544, val_649db0_19);

                    auto dumpPadBuf = [&](uint32_t addr, const char *name)
                    {
                        if (addr + 0x80u > PS2_RAM_SIZE) return;
                        const uint8_t status = *(rdram + addr);
                        const uint32_t at_4 = *reinterpret_cast<const uint32_t *>(rdram + addr + 4u);
                        const uint32_t seq = *reinterpret_cast<const uint32_t *>(rdram + addr + 0x7Cu);
                        const uint8_t b0 = *(rdram + addr + 0x1Cu + 2u);
                        const uint8_t b1 = *(rdram + addr + 0x1Cu + 3u);
                        const uint8_t b2 = *(rdram + addr + 0x1Cu + 4u);
                        const uint8_t b3 = *(rdram + addr + 0x1Cu + 5u);
                        ps2xSafeLog("  %s (0x%08X): status=0x%02X at+4=0x%08X seq=0x%08X btns=[0x%02X,0x%02X,0x%02X,0x%02X]\n",
                                    name, addr, status, at_4, seq, b0, b1, b2, b3);
                    };
                    dumpPadBuf(0x784D40u, "padBufA");
                    dumpPadBuf(0x784DC0u, "padBufB");

                    const uint32_t padStructPtr = (0x649728u + 4u <= PS2_RAM_SIZE)
                                                      ? *reinterpret_cast<const uint32_t *>(rdram + 0x649728u) : 0u;
                    const uint16_t padBtn = (padStructPtr != 0u && padStructPtr + 0x0Eu <= PS2_RAM_SIZE)
                                                ? *reinterpret_cast<const uint16_t *>(rdram + padStructPtr + 0x0Cu) : 0u;
                    ps2xSafeLog("  guestPadStruct: ptr=0x%08X buttons_at_0x0C=0x%04X\n", padStructPtr, padBtn);
                }

                // 3. IO / RPC rates
                const uint32_t sg2Calls = g_diagSg2Fno0Calls.exchange(0u, std::memory_order_relaxed);
                const uint32_t dbcWrites = g_diagDbcBufferWrites.exchange(0u, std::memory_order_relaxed);
                const uint32_t cid1Reqs = g_diagCid1Requests.exchange(0u, std::memory_order_relaxed);
                const uint64_t cid1Bytes = g_diagCid1BytesServed.exchange(0u, std::memory_order_relaxed);
                char lastCid1[256]{};
                diagGetLastCid1Path(lastCid1, sizeof(lastCid1));
                const uint32_t mcCalls = g_diagMcCalls.exchange(0u, std::memory_order_relaxed);

                ps2xSafeLog("  io: sg2_fno0=%u/s dbc_writes=%u/s cid1_reqs=%u/s cid1_bytes=%llu/s last_cid1='%s' mc_calls=%u/s\n",
                            sg2Calls, dbcWrites, cid1Reqs, static_cast<unsigned long long>(cid1Bytes), lastCid1, mcCalls);
                std::fflush(stderr);

                s_lastSamplerTime = now;
            }
        }
    };
}
