#include "Common.h"
#include "IPU.h"
#include "runtime/ee_scheduler.h"
#include <cstdlib> // PROBE-IP1

extern void ipuProbeLogCall(uint32_t ra, const char *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t ret); // PROBE-IP1

namespace
{
    constexpr uint32_t REG_IPU_CTRL = 0x10002010u;
    constexpr uint32_t REG_IPU_CMD = 0x10002000u;

    inline bool ipuProbeOn() // PROBE-IP1
    { // PROBE-IP1
        static const bool s_on = []() { const char *e = std::getenv("PS2X_IPU_LOG"); return e && e[0] == '1'; }(); // PROBE-IP1
        return s_on; // PROBE-IP1
    } // PROBE-IP1

    struct IpuCallLogger // PROBE-IP1
    { // PROBE-IP1
        const char *name; // PROBE-IP1
        R5900Context *ctx; // PROBE-IP1
        uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, ra = 0; // PROBE-IP1
        IpuCallLogger(const char *n, R5900Context *c) : name(n), ctx(c) // PROBE-IP1
        { // PROBE-IP1
            if (ipuProbeOn() && ctx) // PROBE-IP1
            { // PROBE-IP1
                a0 = getRegU32(ctx, 4); // PROBE-IP1
                a1 = getRegU32(ctx, 5); // PROBE-IP1
                a2 = getRegU32(ctx, 6); // PROBE-IP1
                a3 = getRegU32(ctx, 7); // PROBE-IP1
                ra = getRegU32(ctx, 31); // PROBE-IP1
            } // PROBE-IP1
        } // PROBE-IP1
        ~IpuCallLogger() // PROBE-IP1
        { // PROBE-IP1
            if (ipuProbeOn()) // PROBE-IP1
            { // PROBE-IP1
                const uint32_t ret = ctx ? getRegU32(ctx, 2) : 0u; // PROBE-IP1
                ipuProbeLogCall(ra, name, a0, a1, a2, a3, ret); // PROBE-IP1
            } // PROBE-IP1
        } // PROBE-IP1
    }; // PROBE-IP1

    void completeIpuInit(PS2Runtime *runtime)
    {
        if (!runtime)
            return;
        PS2Memory &mem = runtime->memory();
        mem.write32(REG_IPU_CTRL, 0x40000000u);
        mem.write32(REG_IPU_CMD, 0u);
    }
}

namespace ps2_stubs
{
    void sceIpuInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IpuCallLogger ipuLogger("sceIpuInit", ctx); // PROBE-IP1
        (void)rdram;
        if (!runtime)
            return;

        if (!runtime->memory().getRDRAM())
        {
            if (!runtime->memory().initialize())
            {
                setReturnS32(ctx, -1);
                return;
            }
        }

        if (!runtime->syncCoreSubsystems())
        {
            setReturnS32(ctx, -1);
            return;
        }

        completeIpuInit(runtime);
        setReturnS32(ctx, 0);
    }

    void sceIpuRestartDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IpuCallLogger ipuLogger("sceIpuRestartDMA", ctx); // PROBE-IP1
        setReturnS32(ctx, 0);
    }

    void sceIpuStopDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IpuCallLogger ipuLogger("sceIpuStopDMA", ctx); // PROBE-IP1
        setReturnS32(ctx, 0);
    }

    void sceIpuSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IpuCallLogger ipuLogger("sceIpuSync", ctx); // PROBE-IP1
        setReturnS32(ctx, 0);
    }
}
