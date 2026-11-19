#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ps2x::iop::detail
{
    // PERF-AU1: Software SPU2 register and RAM model for HLE libsd.
    class Spu2State
    {
    public:
        static constexpr uint32_t kNumCores = 2u;
        static constexpr uint32_t kNumVoices = 24u;
        static constexpr uint32_t kRamSize = 2u * 1024u * 1024u; // 2 MB

        struct Voice
        {
            uint16_t voll = 0u;
            uint16_t volr = 0u;
            uint16_t pitch = 0u;
            uint16_t adsr1 = 0u;
            uint16_t adsr2 = 0u;
            uint16_t envx = 0u;
            uint16_t volxl = 0u;
            uint16_t volxr = 0u;
            uint32_t ssa = 0u;
            uint32_t lsax = 0u;
            uint32_t nax = 0u;
            bool lsaxExplicit = false;
        };

        struct EffectAttr
        {
            int32_t mode = 0;
            int16_t depthL = 0;
            int16_t depthR = 0;
            int32_t delay = 0;
            int32_t feedback = 0;
        };

        struct Core
        {
            std::array<Voice, kNumVoices> voices{};
            uint16_t mmix = 0u;
            uint16_t mvoll = 0u;
            uint16_t mvolr = 0u;
            uint16_t evoll = 0u;
            uint16_t evolr = 0u;
            uint16_t avoll = 0u;
            uint16_t avolr = 0u;
            uint16_t bvoll = 0u;
            uint16_t bvolr = 0u;
            uint16_t mvolxl = 0u;
            uint16_t mvolxr = 0u;

            uint32_t pmon = 0u;
            uint32_t non = 0u;
            uint32_t kon = 0u;
            uint32_t koff = 0u;
            uint32_t endx = 0u;
            uint32_t vmixl = 0u;
            uint32_t vmixel = 0u;
            uint32_t vmixr = 0u;
            uint32_t vmixer = 0u;

            uint32_t eea = 0u;
            uint32_t esa = 0u;
            uint32_t irqa = 0u;

            uint16_t effectEnable = 0u;
            uint16_t irqEnable = 0u;
            uint16_t muteEnable = 0u;
            uint16_t noiseClk = 0u;
            uint16_t spdifMode = 0u;

            EffectAttr effectAttr{};
        };

        struct TransChannel
        {
            uint32_t voiceTransStatus = 1u;
            bool blockTransActive = false;
            uint32_t blockTransMode = 0u;
            uint32_t blockTransAddr = 0u;
            uint32_t blockTransSize = 0u;
            uint32_t blockTransHalf = 0u;      // PERF-AU3: active half-buffer (0 or 1)
            uint64_t blockTransNextCycle = 0u; // PERF-AU3: next IOP cycle for half-buffer interrupt
            uint32_t intrHandler = 0u;
            uint32_t intrArg = 0u;
            uint32_t intrGp = 0u;
            uint32_t legacyCallback = 0u;
        };

        Spu2State();

        void reset(bool clearRam = true);

        void writeRam(uint32_t spuAddr, const void *src, uint32_t size);
        void readRam(uint32_t spuAddr, void *dst, uint32_t size) const;
        [[nodiscard]] bool blockWritten(uint32_t addr) const; // PROBE-AU3E

        void setParam(uint16_t entry, uint16_t value);
        [[nodiscard]] uint16_t getParam(uint16_t entry) const;

        void setSwitch(uint16_t entry, uint32_t value);
        [[nodiscard]] uint32_t getSwitch(uint16_t entry) const;

        void setAddr(uint16_t entry, uint32_t value);
        [[nodiscard]] uint32_t getAddr(uint16_t entry) const;

        void setCoreAttr(uint16_t entry, uint16_t value);
        [[nodiscard]] uint16_t getCoreAttr(uint16_t entry) const;

        void setEffectAttr(int32_t core, int32_t mode, int16_t depthL, int16_t depthR, int32_t delay, int32_t feedback);
        [[nodiscard]] const EffectAttr &getEffectAttr(int32_t core) const;

        [[nodiscard]] TransChannel &transChannel(uint32_t ch) noexcept { return m_transChannels[ch & 1u]; }
        [[nodiscard]] const TransChannel &transChannel(uint32_t ch) const noexcept { return m_transChannels[ch & 1u]; }

        // PERF-AU2: Lock-light mixer synchronization and 16-byte ADPCM block read.
        struct MixerSnapshot
        {
            std::array<Core, kNumCores> cores{};
            std::array<uint32_t, kNumCores> pendingKon{};
            std::array<uint32_t, kNumCores> pendingKoff{};
            bool pendingReset = false;
            uint64_t keyOnCount = 0u;
        };

        struct MixerFeedback
        {
            struct VoiceUpdate
            {
                uint16_t envx = 0u;
                uint32_t nax = 0u;
                uint32_t lsax = 0u;
                bool updateLsax = false;
            };

            std::array<std::array<VoiceUpdate, kNumVoices>, kNumCores> voices{};
            std::array<uint32_t, kNumCores> newlyEndedMask{};
            std::array<bool, kNumCores> irqTriggered{}; // PERF-AU3
        };

        void snapshotForMixer(MixerSnapshot &outSnapshot);
        void commitMixerFeedback(const MixerFeedback &feedback);
        void readRamBlock16(uint32_t spuAddr, uint8_t outBlock[16]) const;

        // PERF-AU3: AutoDMA (sceSdBlockTrans) ring buffer + IRQ state.
        void startBlockTrans(uint32_t ch, const void *initialData, uint32_t size);
        void updateBlockTransHalf(uint32_t ch, uint32_t halfOffset, const void *data, uint32_t halfSize);
        void stopBlockTrans(uint32_t ch);
        bool readAutoDmaFrames(uint32_t ch, int16_t *outStereo, uint32_t frameCount);
        [[nodiscard]] bool consumeIrqPending(uint32_t core) noexcept;

    private:
        mutable std::mutex m_regMutex;
        mutable std::mutex m_ramMutex;
        mutable std::mutex m_autoDmaMutex; // PERF-AU3
        std::array<Core, kNumCores> m_cores{};
        std::array<uint32_t, kNumCores> m_pendingKon{};
        std::array<uint32_t, kNumCores> m_pendingKoff{};
        bool m_pendingReset = false;
        uint64_t m_keyOnCount = 0u;
        std::array<TransChannel, 2> m_transChannels{};
        std::array<bool, kNumCores> m_irqPending{}; // PERF-AU3
        struct AutoDmaRing
        {
            bool active = false;
            std::vector<uint8_t> buffer;
            uint32_t readBytePos = 0u;
        };
        std::array<AutoDmaRing, 2> m_autoDmaRings{}; // PERF-AU3
        std::vector<uint8_t> m_ram;
        std::array<uint64_t, kRamSize / (16u * 64u)> m_writtenBlocks{}; // PROBE-AU3E
    };
}
