// vu1_flag_ring.h -- MAC/CLIP visibility ring and STATUS undo record shared by the interpreter (ps2_vu1_core.cpp) and the generated
// hot-block code (generated/vu1_hot_blocks.cpp). See PERF-HV: flag writes are applied at issue; a reader gets the newest write whose
// issue cycle + 4 <= its cycle. State is per unit (0 = VU0, 1 = VU1) and defined once through C++17 inline variables.
#pragma once

#include <cstdint>

namespace vuflags
{
    struct FlgRing
    {
        uint64_t cyc[8];
        uint32_t val[8];
        uint32_t base;
        uint8_t head;
        uint8_t count;
    };

    struct FlgStatusUndo
    {
        uint64_t cyc;
        uint32_t old;
        bool valid;
    };

    inline FlgRing s_flgMac[2], s_flgClip[2];
    inline FlgStatusUndo s_flgStatusUndo[2];

    inline void flgPush(FlgRing &ring, uint64_t cycle, uint32_t v)
    {
        if (ring.count == 8u)
            ring.base = ring.val[ring.head]; // overwriting the oldest entry: it has certainly landed
        else
            ++ring.count;
        ring.cyc[ring.head] = cycle;
        ring.val[ring.head] = v;
        ring.head = static_cast<uint8_t>((ring.head + 1u) & 7u);
    }

    inline void flgPushOrReplace(FlgRing &ring, uint64_t cycle, uint32_t v)
    {
        if (ring.count != 0u)
        {
            const uint32_t newest = (ring.head + 7u) & 7u;
            if (ring.cyc[newest] == cycle)
            {
                ring.val[newest] = v;
                return;
            }
        }
        flgPush(ring, cycle, v);
    }

    inline uint32_t flgVisible(const FlgRing &ring, uint64_t cycle)
    {
        for (uint32_t i = 0; i < ring.count; ++i)
        {
            const uint32_t idx = (ring.head + 7u - i) & 7u;
            if (ring.cyc[idx] + 4u <= cycle)
                return ring.val[idx];
        }
        return ring.base;
    }

    inline uint32_t flgNewest(const FlgRing &ring)
    {
        return ring.count ? ring.val[(ring.head + 7u) & 7u] : ring.base;
    }

    inline void flgInit(FlgRing &ring, uint32_t base)
    {
        ring.base = base;
        ring.head = 0;
        ring.count = 0;
    }
} // namespace vuflags
