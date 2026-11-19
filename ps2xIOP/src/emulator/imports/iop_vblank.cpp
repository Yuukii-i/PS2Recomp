#include "iop_vblank.h"

#include "../core/iop_cpu.h"
#include "../iop_emulator_const.h"
#include "../core/iop_kernel.h"

#include <algorithm>

namespace ps2x::iop::detail
{
    IopVblank::IopVblank(IopKernel &kernel) noexcept
        : m_kernel(kernel)
    {
    }

    void IopVblank::reset()
    {
        m_handlers.clear();
        m_nextStartCycle = 0u;
        m_nextEndCycle = 0u;
        m_deliveredCount = 0u;
    }

    bool IopVblank::dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        switch (ordinal)
        {
        case 4: // WaitVblankStart
        case 5: // WaitVblankEnd
        case 6: // WaitVblank
        case 7: // WaitNonVblank
        {
            const bool waitForEnd = ordinal == 5u || ordinal == 7u;
            const uint64_t phase = waitForEnd ? kVblankEndPhaseCycles : 0u;
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            uint64_t wakeCycle = fieldStart + phase;
            if (wakeCycle <= currentCycle)
                wakeCycle += kVblankPeriodCycles;
            m_kernel.delayCurrentUntil(wakeCycle, cpu);
            cpu.gpr[2] = 0u;
            return true;
        }
        case 8: // RegisterVblankHandler(int startend, int priority, int (*handler)(void *), void *common)
        {
            const uint32_t startEnd = cpu.gpr[4] & 1u;
            const int32_t priority = static_cast<int32_t>(cpu.gpr[5]);
            const uint32_t function = cpu.gpr[6];
            const uint32_t common = cpu.gpr[7];
            const uint32_t gp = cpu.gpr[28];
            if (function == 0u)
            {
                cpu.gpr[2] = static_cast<uint32_t>(-1);
                return true;
            }
            for (const auto &h : m_handlers)
            {
                if (h.startEnd == startEnd && h.function == function)
                {
                    cpu.gpr[2] = static_cast<uint32_t>(-1);
                    return true;
                }
            }
            m_handlers.push_back(Handler{startEnd, priority, function, common, gp});
            std::stable_sort(m_handlers.begin(), m_handlers.end(), [](const Handler &a, const Handler &b) {
                return a.priority < b.priority;
            });
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            if (startEnd == 0u && m_nextStartCycle == 0u)
                m_nextStartCycle = fieldStart + kVblankPeriodCycles;
            if (startEnd == 1u && m_nextEndCycle == 0u)
            {
                uint64_t nextEnd = fieldStart + kVblankEndPhaseCycles;
                if (nextEnd <= currentCycle)
                    nextEnd += kVblankPeriodCycles;
                m_nextEndCycle = nextEnd;
            }
            cpu.gpr[2] = 0u;
            return true;
        }
        case 9: // ReleaseVblankHandler(int startend, int (*handler)(void *))
        {
            const uint32_t startEnd = cpu.gpr[4] & 1u;
            const uint32_t function = cpu.gpr[5];
            const auto it = std::find_if(m_handlers.begin(), m_handlers.end(), [&](const Handler &h) {
                return h.startEnd == startEnd && h.function == function;
            });
            if (it == m_handlers.end())
            {
                cpu.gpr[2] = static_cast<uint32_t>(-1);
                return true;
            }
            m_handlers.erase(it);
            cpu.gpr[2] = 0u;
            return true;
        }
        default:
            return false;
        }
    }

    void IopVblank::serviceDue(uint64_t currentCycle, std::vector<DueCallback> &outCallbacks)
    {
        if (m_handlers.empty())
            return;

        if (m_nextStartCycle != 0u && currentCycle >= m_nextStartCycle)
        {
            for (const auto &h : m_handlers)
            {
                if (h.startEnd == 0u)
                {
                    outCallbacks.push_back(DueCallback{h.function, h.gp, h.common});
                    ++m_deliveredCount;
                }
            }
            const uint64_t missed = (currentCycle - m_nextStartCycle) / kVblankPeriodCycles + 1u;
            m_nextStartCycle += missed * kVblankPeriodCycles;
        }

        if (m_nextEndCycle != 0u && currentCycle >= m_nextEndCycle)
        {
            for (const auto &h : m_handlers)
            {
                if (h.startEnd == 1u)
                {
                    outCallbacks.push_back(DueCallback{h.function, h.gp, h.common});
                    ++m_deliveredCount;
                }
            }
            const uint64_t missed = (currentCycle - m_nextEndCycle) / kVblankPeriodCycles + 1u;
            m_nextEndCycle += missed * kVblankPeriodCycles;
        }
    }

    uint64_t IopVblank::nextEventCycle(uint64_t horizon) const noexcept
    {
        if (m_handlers.empty())
            return horizon;
        bool hasStart = false;
        bool hasEnd = false;
        for (const auto &h : m_handlers)
        {
            if (h.startEnd == 0u)
                hasStart = true;
            else
                hasEnd = true;
        }
        if (hasStart && m_nextStartCycle != 0u)
            horizon = std::min(horizon, m_nextStartCycle);
        if (hasEnd && m_nextEndCycle != 0u)
            horizon = std::min(horizon, m_nextEndCycle);
        return horizon;
    }
}
