#pragma once

#include <cstdint>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;

    class IopVblank
    {
    public:
        // PERF-AU3B: Vblank callback registration & 59.94 Hz delivery.
        struct DueCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t common = 0u;
        };

        explicit IopVblank(IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        void serviceDue(uint64_t currentCycle, std::vector<DueCallback> &outCallbacks);
        [[nodiscard]] uint64_t nextEventCycle(uint64_t horizon) const noexcept;
        [[nodiscard]] uint32_t handlerCount() const noexcept { return static_cast<uint32_t>(m_handlers.size()); }
        [[nodiscard]] uint64_t deliveredCount() const noexcept { return m_deliveredCount; }

    private:
        struct Handler
        {
            uint32_t startEnd = 0u;
            int32_t priority = 0;
            uint32_t function = 0u;
            uint32_t common = 0u;
            uint32_t gp = 0u;
        };

        IopKernel &m_kernel;
        std::vector<Handler> m_handlers;
        uint64_t m_nextStartCycle = 0u;
        uint64_t m_nextEndCycle = 0u;
        uint64_t m_deliveredCount = 0u;
    };
}
