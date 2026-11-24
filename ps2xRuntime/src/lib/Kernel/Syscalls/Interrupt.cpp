#include "Common.h"
#include "probe_ae.h"
#include "Interrupt.h"
#include <cstdlib> // PROBE-M13

void ps2xSafeLog(const char *fmt, ...); // PROBE-M13
extern int g_ps2M13SyncVIfreq; // PROBE-M13

namespace ps2_syscalls
{
    namespace
    {
        EeScheduler &scheduler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            EeScheduler &result = runtime->eeScheduler();
            result.bindMainContextForSyscall(*ctx, rdram);
            return result;
        }

        void setCauseEnabled(uint8_t *rdram,
                             R5900Context *ctx,
                             PS2Runtime *runtime,
                             bool dmac,
                             bool enabled)
        {
            uint32_t cause = getRegU32(ctx, 4);
            if (!enabled && cause == 3u) {
                probe_ae::logRmIntc(dmac ? "DisableDmac" : "DisableIntc", cause, getRegU32(ctx, 31));
            }
            setReturnS32(ctx,
                         scheduler(rdram, ctx, runtime)
                             .setIrqCauseEnabled(dmac, cause, enabled));
        }

        void addHandler(uint8_t *rdram,
                        R5900Context *ctx,
                        PS2Runtime *runtime,
                        bool dmac)
        {
            probe_ae::logIntcRegister(dmac, getRegU32(ctx, 4), getRegU32(ctx, 5));
            const int id = scheduler(rdram, ctx, runtime)
                               .addIrqHandler(dmac,
                                              getRegU32(ctx, 4),
                                              getRegU32(ctx, 5),
                                              getRegU32(ctx, 6) != 0u,
                                              getRegU32(ctx, 7),
                                              getRegU32(ctx, 28),
                                              getRegU32(ctx, 29));
            setReturnS32(ctx, id);
        }

        void removeHandler(uint8_t *rdram,
                           R5900Context *ctx,
                           PS2Runtime *runtime,
                           bool dmac)
        {
            uint32_t cause = getRegU32(ctx, 4);
            if (cause == 3u) {
                probe_ae::logRmIntc(dmac ? "RemoveDmacHandler" : "RemoveIntcHandler", cause, getRegU32(ctx, 31));
            }
            setReturnS32(ctx,
                         scheduler(rdram, ctx, runtime)
                             .removeIrqHandler(dmac,
                                               cause,
                                               static_cast<int>(getRegU32(ctx, 5))));
        }

        void setHandlerEnabled(uint8_t *rdram,
                               R5900Context *ctx,
                               PS2Runtime *runtime,
                               bool dmac,
                               bool enabled)
        {
            if (!enabled) {
                int handlerId = static_cast<int>(getRegU32(ctx, 5));
                probe_ae::logRmIntc(dmac ? "DisableDmacHandler" : "DisableIntcHandler", static_cast<uint32_t>(handlerId), getRegU32(ctx, 31));
            }
            setReturnS32(ctx,
                         scheduler(rdram, ctx, runtime)
                             .setIrqHandlerEnabled(dmac,
                                                   static_cast<int>(getRegU32(ctx, 5)),
                                                   enabled));
        }
    }

    void dispatchDmacHandlersForCause(uint8_t *, PS2Runtime *runtime, uint32_t cause)
    {
        runtime->eeScheduler().dispatchIrq(true, cause);
    }

    uint64_t GetCurrentVSyncTick(PS2Runtime *runtime)
    {
        return runtime->eeScheduler().currentVSyncTick();
    }

    void WaitVSyncTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int fixedResult)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const uint64_t startTick = ee.currentVSyncTick();
        const uint32_t ra = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t a0 = ctx ? getRegU32(ctx, 4) : 0u;
        const bool interlace = (fixedResult < 0);
        const auto t0 = std::chrono::steady_clock::now();
        static const bool s_syncvLogOn = []() { const char *e = std::getenv("PS2X_SYNCV_LOG"); return e && e[0] == '1'; }(); // PROBE-M13
        const bool dcProbeOn = probe_ae::isDcProbeEnabled();

        if (s_syncvLogOn) // PROBE-M13
        {
            const int tid = ee.currentThreadId();
            if (tid != 1)
            {
                ::ps2xSafeLog("[syncv] non-main tid=%d ra=0x%08X\n", tid, ra);
            }
        }

        if (dcProbeOn || s_syncvLogOn) {
            if (dcProbeOn) {
                probe_ae::onDcSyncVStart(ra, a0, interlace, rdram, ee.currentThreadId());
            }
            const int ifreq = s_syncvLogOn ? g_ps2M13SyncVIfreq : 0; // PROBE-M13
            ee.waitVSync(startTick, fixedResult, [&ee, runtime, startTick, ra, t0, dcProbeOn, ifreq](R5900Context &c) {
                const uint64_t resumeTick = ee.currentVSyncTick();
                const auto t1 = std::chrono::steady_clock::now();
                if (dcProbeOn) {
                    const uint64_t deltaTicks = (resumeTick >= startTick) ? (resumeTick - startTick) : 0u;
                    const uint32_t ret = getRegU32(&c, 2);
                    const double waitMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                    probe_ae::onDcSyncVComplete(ra, deltaTicks, ret, waitMs);
                }
                if (s_syncvLogOn) { // PROBE-M13
                    static uint32_t s_n = 0u, s_d0 = 0u, s_d1 = 0u, s_d2 = 0u, s_d3 = 0u, s_d4p = 0u, s_nowait = 0u, s_dtCount = 0u;
                    static double s_minMs = 0.0, s_maxMs = 0.0, s_sumMs = 0.0;
                    static bool s_hasPrevReturn = false, s_hasWindow = false;
                    static uint64_t s_prevExitTick = 0u;
                    static std::chrono::steady_clock::time_point s_prevReturnTime{};
                    static std::chrono::steady_clock::time_point s_windowStartTime{};
                    static uint32_t s_prevCount = 0u, s_prevT0 = 0u, s_prevT1 = 0u;

                    if (!s_hasWindow) {
                        s_hasWindow = true;
                        s_windowStartTime = t1;
                        s_prevCount = c.cop0_count;
                        s_prevT0 = runtime ? runtime->memory().readIORegister(0x10000000u) : 0u;
                        s_prevT1 = runtime ? runtime->memory().readIORegister(0x10000800u) : 0u;
                    }

                    ++s_n;
                    if (resumeTick == startTick) {
                        ++s_nowait;
                    }
                    if (s_hasPrevReturn) {
                        const uint64_t dv = (resumeTick >= s_prevExitTick) ? (resumeTick - s_prevExitTick) : 0u;
                        if (dv == 0u) ++s_d0;
                        else if (dv == 1u) ++s_d1;
                        else if (dv == 2u) ++s_d2;
                        else if (dv == 3u) ++s_d3;
                        else ++s_d4p;

                        const double dtMs = std::chrono::duration<double, std::milli>(t1 - s_prevReturnTime).count();
                        if (s_dtCount == 0u || dtMs < s_minMs) s_minMs = dtMs;
                        if (s_dtCount == 0u || dtMs > s_maxMs) s_maxMs = dtMs;
                        s_sumMs += dtMs;
                        ++s_dtCount;
                    }
                    s_hasPrevReturn = true;
                    s_prevExitTick = resumeTick;
                    s_prevReturnTime = t1;

                    const double windowMs = std::chrono::duration<double, std::milli>(t1 - s_windowStartTime).count();
                    if (windowMs >= 1000.0) {
                        const double minMs = (s_dtCount > 0u) ? s_minMs : 0.0;
                        const double avgMs = (s_dtCount > 0u) ? (s_sumMs / static_cast<double>(s_dtCount)) : 0.0;
                        const double maxMs = (s_dtCount > 0u) ? s_maxMs : 0.0;
                        ::ps2xSafeLog("[syncv] n=%u d0=%u d1=%u d2=%u d3=%u d4p=%u nowait=%u hostMs(min/avg/max)=%.1f/%.1f/%.1f ifreq=%d\n",
                                      s_n, s_d0, s_d1, s_d2, s_d3, s_d4p, s_nowait, minMs, avgMs, maxMs, ifreq);

                        const uint32_t count = c.cop0_count;
                        const uint32_t t0Reg = runtime ? runtime->memory().readIORegister(0x10000000u) : 0u;
                        const uint32_t t1Reg = runtime ? runtime->memory().readIORegister(0x10000800u) : 0u;
                        const uint32_t dCount = count - s_prevCount;
                        const uint32_t dT0 = (t0Reg - s_prevT0) & 0xFFFFu;
                        const uint32_t dT1 = (t1Reg - s_prevT1) & 0xFFFFu;
                        ::ps2xSafeLog("[time] count=%u dCount=%u t0=%u dT0=%u t1=%u dT1=%u hostMs=%.1f\n",
                                      count, dCount, t0Reg, dT0, t1Reg, dT1, windowMs);

                        s_n = s_d0 = s_d1 = s_d2 = s_d3 = s_d4p = s_nowait = s_dtCount = 0u;
                        s_minMs = s_maxMs = s_sumMs = 0.0;
                        s_windowStartTime = t1;
                        s_prevCount = count;
                        s_prevT0 = t0Reg;
                        s_prevT1 = t1Reg;
                    }
                }
            });
        } else {
            ee.waitVSync(startTick, fixedResult);
        }
    }

    void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t flagAddress = getRegU32(ctx, 4);
        const uint32_t tickAddress = getRegU32(ctx, 5);
        if ((flagAddress != 0u && !getEeGuestStruct<uint32_t>(rdram, flagAddress)) ||
            (tickAddress != 0u && !getEeGuestStruct<uint64_t>(rdram, tickAddress)))
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        scheduler(rdram, ctx, runtime).setVSyncFlag(flagAddress, tickAddress);
        setReturnS32(ctx, KE_OK);
    }

    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setCauseEnabled(rdram, ctx, runtime, false, true);
    }

    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableIntc(rdram, ctx, runtime);
    }

    void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setCauseEnabled(rdram, ctx, runtime, false, false);
    }

    void iDisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableIntc(rdram, ctx, runtime);
    }

    void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        addHandler(rdram, ctx, runtime, false);
    }

    void AddIntcHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddIntcHandler(rdram, ctx, runtime);
    }

    void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        removeHandler(rdram, ctx, runtime, false);
    }

    void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        addHandler(rdram, ctx, runtime, true);
    }

    void AddDmacHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddDmacHandler(rdram, ctx, runtime);
    }

    void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        removeHandler(rdram, ctx, runtime, true);
    }

    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setHandlerEnabled(rdram, ctx, runtime, false, true);
    }

    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setHandlerEnabled(rdram, ctx, runtime, false, false);
    }

    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setHandlerEnabled(rdram, ctx, runtime, true, true);
    }

    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setHandlerEnabled(rdram, ctx, runtime, true, false);
    }

    void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setCauseEnabled(rdram, ctx, runtime, true, true);
    }

    void iEnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableDmac(rdram, ctx, runtime);
    }

    void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setCauseEnabled(rdram, ctx, runtime, true, false);
    }

    void iDisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableDmac(rdram, ctx, runtime);
    }
}
