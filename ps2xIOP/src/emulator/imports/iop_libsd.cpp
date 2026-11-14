#include "iop_libsd.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../../spu2/spu2_state.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ps2x::iop
{
    // PERF-AU1: Diagnostics bridge declared locally in RPC.cpp (header rule safe).
    struct LibsdDiagSnapshot
    {
        uint64_t totalCalls = 0u;
        uint64_t keyOns = 0u;
        uint64_t transferBytes = 0u;
        std::array<uint64_t, 28> ordinalCounts{};
    };

    void getLibsdDiagnostics(LibsdDiagSnapshot &outSnapshot, std::vector<std::string> &outPendingLogs);
}

namespace ps2x::iop::detail
{
    namespace
    {
        std::mutex g_libsdDiagMutex;
        LibsdDiagSnapshot g_libsdDiag;
        std::vector<std::string> g_libsdFirstLogs;
        uint32_t g_libsdLoggedCount = 0u;
        constexpr uint32_t kMaxVerbatimLogs = 50u;

        const char *libsdOrdinalName(uint16_t ordinal) noexcept
        {
            switch (ordinal)
            {
            case 4: return "sceSdInit";
            case 5: return "sceSdSetParam";
            case 6: return "sceSdGetParam";
            case 7: return "sceSdSetSwitch";
            case 8: return "sceSdGetSwitch";
            case 9: return "sceSdSetAddr";
            case 10: return "sceSdGetAddr";
            case 11: return "sceSdSetCoreAttr";
            case 12: return "sceSdGetCoreAttr";
            case 17: return "sceSdVoiceTrans";
            case 18: return "sceSdBlockTrans";
            case 19: return "sceSdVoiceTransStatus";
            case 20: return "sceSdBlockTransStatus";
            case 21: return "sceSdSetTransCallback";
            case 23: return "sceSdSetEffectAttr";
            case 24: return "sceSdGetEffectAttr";
            case 25: return "sceSdClearEffectWorkArea";
            case 26: return "sceSdSetTransIntrHandler";
            default: return "sceSdUnknown";
            }
        }

        uint32_t popcount24(uint32_t mask) noexcept
        {
            uint32_t m = mask & 0x00FFFFFFu;
            uint32_t count = 0u;
            while (m != 0u)
            {
                count += (m & 1u);
                m >>= 1u;
            }
            return count;
        }

        void recordLibsdCall(uint16_t ordinal, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t ret, uint64_t xferBytes, uint64_t keyOnCount)
        {
            std::lock_guard<std::mutex> lock(g_libsdDiagMutex);
            ++g_libsdDiag.totalCalls;
            if (ordinal < g_libsdDiag.ordinalCounts.size())
                ++g_libsdDiag.ordinalCounts[ordinal];
            g_libsdDiag.keyOns += keyOnCount;
            g_libsdDiag.transferBytes += xferBytes;

            if (g_libsdLoggedCount < kMaxVerbatimLogs)
            {
                ++g_libsdLoggedCount;
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "#%02u %s(ord=%u) a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x -> 0x%08x",
                              g_libsdLoggedCount, libsdOrdinalName(ordinal),
                              static_cast<unsigned>(ordinal), a0, a1, a2, a3, ret);
                g_libsdFirstLogs.emplace_back(buf);
            }
        }
    }

    IopLibsd::IopLibsd(IopMemory &memory, Spu2State &spu2) noexcept
        : m_memory(memory), m_spu2(spu2)
    {
    }

    void IopLibsd::reset()
    {
        m_spu2.reset(true);
        m_pendingCallback.reset();
        std::lock_guard<std::mutex> lock(g_libsdDiagMutex);
        g_libsdDiag = {};
        g_libsdFirstLogs.clear();
        g_libsdLoggedCount = 0u;
    }

    std::optional<IopLibsd::CompletionCallback> IopLibsd::takeCompletionCallback() noexcept
    {
        auto cb = m_pendingCallback;
        m_pendingCallback.reset();
        return cb;
    }

    bool IopLibsd::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const uint32_t a3 = cpu.gpr[7];
        const uint32_t sp = cpu.gpr[29];

        uint32_t ret = 0u;
        uint64_t xferBytes = 0u;
        uint64_t keyOns = 0u;

        switch (ordinal)
        {
        case 4: // sceSdInit(int flag)
            m_spu2.reset(a0 == 0u);
            ret = 0u;
            break;

        case 5: // sceSdSetParam(u16 entry, u16 value)
            m_spu2.setParam(static_cast<uint16_t>(a0 & 0xFFFFu), static_cast<uint16_t>(a1 & 0xFFFFu));
            ret = 0u;
            break;

        case 6: // sceSdGetParam(u16 entry)
            ret = m_spu2.getParam(static_cast<uint16_t>(a0 & 0xFFFFu));
            break;

        case 7: // sceSdSetSwitch(u16 entry, u32 value)
            m_spu2.setSwitch(static_cast<uint16_t>(a0 & 0xFFFFu), a1);
            if ((a0 & 0xFF00u) == 0x1500u) // SD_S_KON
                keyOns = popcount24(a1);
            ret = 0u;
            break;

        case 8: // sceSdGetSwitch(u16 entry)
            ret = m_spu2.getSwitch(static_cast<uint16_t>(a0 & 0xFFFFu));
            break;

        case 9: // sceSdSetAddr(u16 entry, u32 value)
            m_spu2.setAddr(static_cast<uint16_t>(a0 & 0xFFFFu), a1);
            ret = 0u;
            break;

        case 10: // sceSdGetAddr(u16 entry)
            ret = m_spu2.getAddr(static_cast<uint16_t>(a0 & 0xFFFFu));
            break;

        case 11: // sceSdSetCoreAttr(u16 entry, u16 value)
            m_spu2.setCoreAttr(static_cast<uint16_t>(a0 & 0xFFFFu), static_cast<uint16_t>(a1 & 0xFFFFu));
            ret = 0u;
            break;

        case 12: // sceSdGetCoreAttr(u16 entry)
            ret = m_spu2.getCoreAttr(static_cast<uint16_t>(a0 & 0xFFFFu));
            break;

        case 17: // sceSdVoiceTrans(s16 channel, u16 mode, u8 *m_addr, u32 s_addr, u32 size)
        {
            const uint32_t ch = a0 & 1u;
            const uint32_t mode = a1;
            const uint32_t mAddr = a2;
            const uint32_t sAddr = a3;
            const uint32_t size = m_memory.read32(sp + 16u);
            if (size > 0u && m_memory.ownsRamRange(mAddr, size))
            {
                std::vector<uint8_t> buf(size);
                if ((mode & 1u) == 0u) // SD_TRANS_WRITE: IOP RAM -> SPU2 RAM
                {
                    if (m_memory.readRam(mAddr, buf.data(), size))
                    {
                        m_spu2.writeRam(sAddr, buf.data(), size);
                        xferBytes = size;
                    }
                }
                else // SD_TRANS_READ: SPU2 RAM -> IOP RAM
                {
                    m_spu2.readRam(sAddr, buf.data(), size);
                    (void)m_memory.writeRam(mAddr, buf.data(), size);
                    xferBytes = size;
                }
            }
            auto &tc = m_spu2.transChannel(ch);
            tc.voiceTransStatus = 1u;
            if (tc.intrHandler != 0u)
            {
                m_pendingCallback = CompletionCallback{tc.intrHandler, tc.intrGp, ch, tc.intrArg};
            }
            ret = size;
            break;
        }

        case 18: // sceSdBlockTrans(s16 channel, u16 mode, u8 *m_addr, u32 size, ...)
        {
            const uint32_t ch = a0 & 1u;
            const uint32_t mode = a1;
            const uint32_t mAddr = a2;
            const uint32_t size = a3;
            auto &tc = m_spu2.transChannel(ch);
            if ((mode & 3u) == 2u) // SD_TRANS_STOP
            {
                tc.blockTransActive = false;
                tc.blockTransMode = mode;
            }
            else
            {
                tc.blockTransActive = true;
                tc.blockTransMode = mode;
                tc.blockTransAddr = mAddr;
                tc.blockTransSize = size;
                xferBytes = size;
            }
            ret = 0u;
            break;
        }

        case 19: // sceSdVoiceTransStatus(s16 channel, s16 flag)
            ret = m_spu2.transChannel(a0 & 1u).voiceTransStatus;
            break;

        case 20: // sceSdBlockTransStatus(s16 channel, s16 flag)
        {
            const auto &tc = m_spu2.transChannel(a0 & 1u);
            ret = (tc.blockTransActive ? 0x01000000u : 0u) | (tc.blockTransAddr & 0x00FFFFFFu);
            break;
        }

        case 21: // sceSdSetTransCallback(s16 channel, sceSdTransCallback func)
        {
            auto &tc = m_spu2.transChannel(a0 & 1u);
            ret = tc.legacyCallback;
            tc.legacyCallback = a1;
            break;
        }

        case 23: // sceSdSetEffectAttr(s32 core, const sceSdEffectAttr *attr)
        {
            if (a1 != 0u && m_memory.ownsRamRange(a1, 20u))
            {
                const int32_t mode = static_cast<int32_t>(m_memory.read32(a1 + 4u));
                const int16_t depthL = static_cast<int16_t>(m_memory.read16(a1 + 8u));
                const int16_t depthR = static_cast<int16_t>(m_memory.read16(a1 + 10u));
                const int32_t delay = static_cast<int32_t>(m_memory.read32(a1 + 12u));
                const int32_t feedback = static_cast<int32_t>(m_memory.read32(a1 + 16u));
                m_spu2.setEffectAttr(static_cast<int32_t>(a0), mode, depthL, depthR, delay, feedback);
            }
            ret = 0u;
            break;
        }

        case 24: // sceSdGetEffectAttr(s32 core, sceSdEffectAttr *attr)
        {
            if (a1 != 0u && m_memory.ownsRamRange(a1, 20u))
            {
                const auto &ea = m_spu2.getEffectAttr(static_cast<int32_t>(a0));
                m_memory.write32(a1 + 0u, a0 & 1u);
                m_memory.write32(a1 + 4u, static_cast<uint32_t>(ea.mode));
                m_memory.write16(a1 + 8u, static_cast<uint16_t>(ea.depthL));
                m_memory.write16(a1 + 10u, static_cast<uint16_t>(ea.depthR));
                m_memory.write32(a1 + 12u, static_cast<uint32_t>(ea.delay));
                m_memory.write32(a1 + 16u, static_cast<uint32_t>(ea.feedback));
            }
            ret = 0u;
            break;
        }

        case 25: // sceSdClearEffectWorkArea(s32 core, s32 channel, s32 effect_mode)
            ret = 0u;
            break;

        case 26: // sceSdSetTransIntrHandler(s32 channel, sceSdTransIntrHandler func, void *arg)
        {
            auto &tc = m_spu2.transChannel(a0 & 1u);
            ret = tc.intrHandler;
            tc.intrHandler = a1;
            tc.intrArg = a2;
            tc.intrGp = cpu.gpr[28];
            break;
        }

        default:
            return false;
        }

        cpu.gpr[2] = ret;
        recordLibsdCall(ordinal, a0, a1, a2, a3, ret, xferBytes, keyOns);
        return true;
    }
}

namespace ps2x::iop
{
    void getLibsdDiagnostics(LibsdDiagSnapshot &outSnapshot, std::vector<std::string> &outPendingLogs)
    {
        std::lock_guard<std::mutex> lock(detail::g_libsdDiagMutex);
        outSnapshot = detail::g_libsdDiag;
        outPendingLogs.swap(detail::g_libsdFirstLogs);
    }
}
