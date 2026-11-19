#pragma once

#include "spu2_state.h"

#include <array>
#include <cstdint>

namespace ps2x::iop::detail
{
    // PERF-AU2: Software SPU2 48 kHz ADPCM + ADSR voice synthesizer and stereo mixer.
    class Spu2Mixer
    {
    public:
        struct MixStats
        {
            uint32_t activeVoices = 0u;
            uint64_t keyOns = 0u;
            int32_t peakLevel = 0;
        };

        void reset();
        void mixBlock(Spu2State &state, int16_t *outStereo, uint32_t frameCount, MixStats &outStats);

    private:
        enum class AdsrPhase : uint8_t
        {
            Off = 0,
            Attack,
            Decay,
            Sustain,
            Release
        };

        struct VoiceState
        {
            bool active = false;
            AdsrPhase adsrPhase = AdsrPhase::Off;
            int32_t envLevel = 0;
            uint32_t adsrCounter = 0u;

            uint32_t currentAddr = 0u;
            uint32_t lsax = 0u;
            bool lsaxExplicit = false;
            bool lsaxUpdated = false;

            int32_t hist1 = 0;
            int32_t hist2 = 0;
            int16_t prevSample = 0;
            std::array<int16_t, 28> decoded{};
            uint8_t blockFlags = 0u;
            uint32_t sampleIdx = 28u;
            uint32_t fracPhase = 0u;
        };

        std::array<std::array<VoiceState, Spu2State::kNumVoices>, Spu2State::kNumCores> m_voices{};
        Spu2State::MixerSnapshot m_snapshot{};
        Spu2State::MixerFeedback m_feedback{};

        void keyOnVoice(const Spu2State::Voice &reg, VoiceState &v, const Spu2State &state);
        void decodeBlock(VoiceState &v, const Spu2State &state);
        bool advanceBlock(VoiceState &v, const Spu2State &state, uint32_t voiceBit, uint32_t &newlyEndedMask);
        static void stepAdsr(const Spu2State::Voice &reg, VoiceState &v);
    };
}

namespace ps2x::iop
{
    // PERF-AU2: Bridge functions for host audio output glue (declared locally in ps2_spu2_audio.cpp).
    void registerActiveSpu2State(detail::Spu2State *state) noexcept;
    void unregisterActiveSpu2State(detail::Spu2State *state) noexcept;
    uint32_t mixSpu2AudioBlock(int16_t *outStereo, uint32_t frameCount);
    void getSpu2MixStats(uint32_t &outActiveVoices, uint64_t &outKeyOns, int32_t &outPeak);
}
