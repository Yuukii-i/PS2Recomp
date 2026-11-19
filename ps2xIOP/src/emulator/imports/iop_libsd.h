#pragma once

#include <cstdint>
#include <optional>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopMemory;
    class Spu2State;

    // PERF-AU1: HLE libsd import library backed by Spu2State.
    class IopLibsd
    {
    public:
        struct CompletionCallback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            uint32_t channel = 0u;
            uint32_t arg = 0u;
        };

        IopLibsd(IopMemory &memory, Spu2State &spu2) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<CompletionCallback> takeCompletionCallback() noexcept;

    private:
        IopMemory &m_memory;
        Spu2State &m_spu2;
        std::optional<CompletionCallback> m_pendingCallback;
    };
}
