#include "Common.h"
#include "probe_ae.h"
#include "Interrupt.h"

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

        if (probe_ae::isDcProbeEnabled()) {
            probe_ae::onDcSyncVStart(ra, a0, interlace, rdram, ee.currentThreadId());
            ee.waitVSync(startTick, fixedResult, [&ee, startTick, ra, t0](R5900Context &c) {
                const uint64_t resumeTick = ee.currentVSyncTick();
                const uint64_t deltaTicks = (resumeTick >= startTick) ? (resumeTick - startTick) : 0u;
                const uint32_t ret = getRegU32(&c, 2);
                const auto t1 = std::chrono::steady_clock::now();
                const double waitMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                probe_ae::onDcSyncVComplete(ra, deltaTicks, ret, waitMs);
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
