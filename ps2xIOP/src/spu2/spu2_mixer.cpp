#include "spu2_mixer.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int32_t kAdpcmFilter0[5] = { 0, 60, 115, 98, 122 };
        constexpr int32_t kAdpcmFilter1[5] = { 0, 0, -52, -55, -60 };

        constexpr int32_t decodeFixedVol(uint16_t reg) noexcept
        {
            if ((reg & 0x8000u) != 0u)
                return 0x3FFF;
            return static_cast<int32_t>(static_cast<int16_t>(reg << 1u) >> 1u);
        }

        void stepAdsrRate(int32_t &env, uint32_t &counter, uint32_t rate, bool exponential, bool decreasing) noexcept
        {
            if (rate >= 127u)
                return;

            const uint32_t shift = rate >> 2u;
            const int32_t stepBase = 7 - static_cast<int32_t>(rate & 3u);
            uint32_t threshold = (shift > 11u) ? (1u << (shift - 11u)) : 1u;
            if (!decreasing && exponential && env > 0x6000)
                threshold <<= 2u;

            ++counter;
            if (counter < threshold)
                return;
            counter = 0u;

            int32_t step = (shift < 11u) ? (stepBase << (11u - shift)) : stepBase;
            if (decreasing)
            {
                if (exponential)
                {
                    step = (step * env) >> 15;
                    if (step == 0 && env > 0)
                        step = 1;
                }
                env = std::max(0, env - step);
            }
            else
            {
                env = std::min(0x7FFF, env + step);
            }
        }
    }

    void Spu2Mixer::reset()
    {
        for (auto &core : m_voices)
            core = {};
    }

    void Spu2Mixer::decodeBlock(VoiceState &v, const Spu2State &state)
    {
        uint8_t block[16]{};
        state.readRamBlock16(v.currentAddr, block);

        uint32_t shift = block[0] & 0x0Fu;
        if (shift > 12u)
            shift = 9u;
        uint32_t filter = (block[0] >> 4u) & 0x0Fu;
        if (filter > 4u)
            filter = 4u;

        v.blockFlags = block[1];
        if ((v.blockFlags & 0x04u) != 0u && !v.lsaxExplicit)
        {
            v.lsax = v.currentAddr;
            v.lsaxUpdated = true;
        }

        const int32_t f0 = kAdpcmFilter0[filter];
        const int32_t f1 = kAdpcmFilter1[filter];

        for (uint32_t i = 0u; i < 28u; ++i)
        {
            const uint8_t byte = block[2u + (i >> 1u)];
            const uint32_t nibble = (i & 1u) != 0u ? (byte >> 4u) : (byte & 0x0Fu);
            const int32_t s = static_cast<int32_t>(static_cast<int16_t>(nibble << 12u)) >> static_cast<int32_t>(shift);
            const int32_t predicted = (v.hist1 * f0 + v.hist2 * f1 + 32) >> 6;
            const int32_t sample = std::clamp(s + predicted, -32768, 32767);
            v.hist2 = v.hist1;
            v.hist1 = sample;
            v.decoded[i] = static_cast<int16_t>(sample);
        }
        v.sampleIdx = 0u;
    }

    void Spu2Mixer::keyOnVoice(const Spu2State::Voice &reg, VoiceState &v, const Spu2State &state)
    {
        v.active = true;
        v.adsrPhase = AdsrPhase::Attack;
        v.envLevel = 0;
        v.adsrCounter = 0u;

        v.currentAddr = reg.ssa & ~0xFu & (Spu2State::kRamSize - 1u);
        v.lsax = reg.lsax & ~0xFu & (Spu2State::kRamSize - 1u);
        v.lsaxExplicit = reg.lsaxExplicit;
        v.lsaxUpdated = false;

        v.hist1 = 0;
        v.hist2 = 0;
        v.prevSample = 0;
        v.fracPhase = 0u;
        decodeBlock(v, state);
    }

    bool Spu2Mixer::advanceBlock(VoiceState &v, const Spu2State &state, uint32_t voiceBit, uint32_t &newlyEndedMask)
    {
        if ((v.blockFlags & 0x01u) != 0u) // Loop End
        {
            newlyEndedMask |= voiceBit;
            if ((v.blockFlags & 0x02u) != 0u) // Loop Repeat
            {
                v.currentAddr = v.lsax & ~0xFu & (Spu2State::kRamSize - 1u);
            }
            else
            {
                v.envLevel = 0;
                v.adsrPhase = AdsrPhase::Off;
                v.active = false;
                return false;
            }
        }
        else
        {
            v.currentAddr = (v.currentAddr + 16u) & (Spu2State::kRamSize - 1u);
        }

        decodeBlock(v, state);
        return true;
    }

    void Spu2Mixer::stepAdsr(const Spu2State::Voice &reg, VoiceState &v)
    {
        switch (v.adsrPhase)
        {
        case AdsrPhase::Off:
            v.envLevel = 0;
            v.active = false;
            break;

        case AdsrPhase::Attack:
        {
            const uint32_t ar = (reg.adsr1 >> 8u) & 0x7Fu;
            const bool exp = (reg.adsr1 & 0x8000u) != 0u;
            stepAdsrRate(v.envLevel, v.adsrCounter, ar, exp, false);
            if (v.envLevel >= 0x7FFF)
            {
                v.envLevel = 0x7FFF;
                v.adsrPhase = AdsrPhase::Decay;
                v.adsrCounter = 0u;
            }
            break;
        }

        case AdsrPhase::Decay:
        {
            const uint32_t dr = ((reg.adsr1 >> 4u) & 0x0Fu) << 2u;
            const int32_t sustainLevel = std::min(0x7FFF, (static_cast<int32_t>(reg.adsr1 & 0x0Fu) + 1) << 11);
            stepAdsrRate(v.envLevel, v.adsrCounter, dr, true, true);
            if (v.envLevel <= sustainLevel)
            {
                v.adsrPhase = AdsrPhase::Sustain;
                v.adsrCounter = 0u;
            }
            break;
        }

        case AdsrPhase::Sustain:
        {
            const uint32_t sr = (reg.adsr2 >> 6u) & 0x7Fu;
            const bool exp = (reg.adsr2 & 0x8000u) != 0u;
            const bool dec = (reg.adsr2 & 0x4000u) != 0u;
            stepAdsrRate(v.envLevel, v.adsrCounter, sr, exp, dec);
            break;
        }

        case AdsrPhase::Release:
        {
            const uint32_t rr = (reg.adsr2 & 0x1Fu) << 2u;
            const bool exp = (reg.adsr2 & 0x0020u) != 0u;
            stepAdsrRate(v.envLevel, v.adsrCounter, rr, exp, true);
            if (v.envLevel <= 0)
            {
                v.envLevel = 0;
                v.adsrPhase = AdsrPhase::Off;
                v.active = false;
            }
            break;
        }
        }
    }

    void Spu2Mixer::mixBlock(Spu2State &state, int16_t *outStereo, uint32_t frameCount, MixStats &outStats)
    {
        if (!outStereo || frameCount == 0u)
            return;

        state.snapshotForMixer(m_snapshot);
        if (m_snapshot.pendingReset)
            reset();

        m_feedback = {};

        for (uint32_t ci = 0u; ci < Spu2State::kNumCores; ++ci)
        {
            const uint32_t kon = m_snapshot.pendingKon[ci];
            const uint32_t koff = m_snapshot.pendingKoff[ci];
            for (uint32_t vi = 0u; vi < Spu2State::kNumVoices; ++vi)
            {
                const uint32_t bit = 1u << vi;
                VoiceState &v = m_voices[ci][vi];
                const Spu2State::Voice &reg = m_snapshot.cores[ci].voices[vi];
                if ((kon & bit) != 0u)
                {
                    keyOnVoice(reg, v, state);
                }
                else if ((koff & bit) != 0u && v.active)
                {
                    v.adsrPhase = AdsrPhase::Release;
                    v.adsrCounter = 0u;
                }
            }
        }

        int32_t peak = 0;

        for (uint32_t f = 0u; f < frameCount; ++f)
        {
            int64_t totalL = 0;
            int64_t totalR = 0;

            for (uint32_t ci = 0u; ci < Spu2State::kNumCores; ++ci)
            {
                const Spu2State::Core &coreReg = m_snapshot.cores[ci];
                int64_t coreL = 0;
                int64_t coreR = 0;

                for (uint32_t vi = 0u; vi < Spu2State::kNumVoices; ++vi)
                {
                    VoiceState &v = m_voices[ci][vi];
                    if (!v.active)
                        continue;

                    const Spu2State::Voice &reg = coreReg.voices[vi];
                    stepAdsr(reg, v);
                    if (!v.active)
                        continue;

                    if (v.sampleIdx >= 28u)
                    {
                        if (!advanceBlock(v, state, 1u << vi, m_feedback.newlyEndedMask[ci]))
                            continue;
                    }

                    // Linear interpolation with 12-bit fractional phase (0x1000 = 48 kHz).
                    const int32_t curr = v.decoded[v.sampleIdx];
                    const int32_t prev = (v.sampleIdx > 0u) ? v.decoded[v.sampleIdx - 1u] : v.prevSample;
                    const int32_t frac = static_cast<int32_t>(v.fracPhase & 0x0FFFu);
                    const int32_t sample = prev + (((curr - prev) * frac) >> 12);

                    const int32_t envSample = (sample * v.envLevel) >> 15;
                    const uint32_t bit = 1u << vi;
                    if ((coreReg.vmixl & bit) != 0u)
                        coreL += (static_cast<int64_t>(envSample) * decodeFixedVol(reg.voll)) >> 14;
                    if ((coreReg.vmixr & bit) != 0u)
                        coreR += (static_cast<int64_t>(envSample) * decodeFixedVol(reg.volr)) >> 14;

                    const uint32_t pitch = std::min<uint32_t>(reg.pitch & 0x3FFFu, 0x3FFFu);
                    v.fracPhase += pitch;
                    uint32_t steps = v.fracPhase >> 12u;
                    v.fracPhase &= 0x0FFFu;

                    while (steps > 0u && v.active)
                    {
                        v.prevSample = v.decoded[v.sampleIdx];
                        ++v.sampleIdx;
                        --steps;
                        if (v.sampleIdx >= 28u)
                        {
                            if (!advanceBlock(v, state, bit, m_feedback.newlyEndedMask[ci]))
                                break;
                        }
                    }
                }

                if (coreReg.muteEnable == 0u)
                {
                    coreL = (coreL * decodeFixedVol(coreReg.mvoll)) >> 14;
                    coreR = (coreR * decodeFixedVol(coreReg.mvolr)) >> 14;
                    totalL += coreL;
                    totalR += coreR;
                }
            }

            const int32_t outL = static_cast<int32_t>(std::clamp<int64_t>(totalL, -32768, 32767));
            const int32_t outR = static_cast<int32_t>(std::clamp<int64_t>(totalR, -32768, 32767));
            outStereo[f * 2u + 0u] = static_cast<int16_t>(outL);
            outStereo[f * 2u + 1u] = static_cast<int16_t>(outR);

            const int32_t absL = std::abs(outL);
            const int32_t absR = std::abs(outR);
            if (absL > peak)
                peak = absL;
            if (absR > peak)
                peak = absR;
        }

        uint32_t activeCount = 0u;
        for (uint32_t ci = 0u; ci < Spu2State::kNumCores; ++ci)
        {
            for (uint32_t vi = 0u; vi < Spu2State::kNumVoices; ++vi)
            {
                VoiceState &v = m_voices[ci][vi];
                if (v.active)
                    ++activeCount;
                auto &fb = m_feedback.voices[ci][vi];
                fb.envx = static_cast<uint16_t>(std::clamp(v.envLevel, 0, 0x7FFF));
                fb.nax = v.currentAddr;
                fb.lsax = v.lsax;
                fb.updateLsax = v.lsaxUpdated;
                v.lsaxUpdated = false;
            }
        }

        state.commitMixerFeedback(m_feedback);

        outStats.activeVoices = activeCount;
        outStats.keyOns = m_snapshot.keyOnCount;
        outStats.peakLevel = peak;
    }
}

namespace ps2x::iop
{
    namespace
    {
        std::mutex g_spu2BridgeMutex;
        detail::Spu2State *g_activeSpu2State = nullptr;
        detail::Spu2Mixer g_spu2Mixer;
        std::atomic<uint32_t> g_lastActiveVoices{0u};
        std::atomic<uint64_t> g_lastKeyOns{0u};
        std::atomic<int32_t> g_peakAccum{0};
    }

    void registerActiveSpu2State(detail::Spu2State *state) noexcept
    {
        std::lock_guard<std::mutex> lock(g_spu2BridgeMutex);
        g_activeSpu2State = state;
        g_spu2Mixer.reset();
        g_lastActiveVoices.store(0u, std::memory_order_relaxed);
        g_lastKeyOns.store(0u, std::memory_order_relaxed);
        g_peakAccum.store(0, std::memory_order_relaxed);
    }

    void unregisterActiveSpu2State(detail::Spu2State *state) noexcept
    {
        std::lock_guard<std::mutex> lock(g_spu2BridgeMutex);
        if (g_activeSpu2State == state)
            g_activeSpu2State = nullptr;
    }

    uint32_t mixSpu2AudioBlock(int16_t *outStereo, uint32_t frameCount)
    {
        if (!outStereo || frameCount == 0u)
            return 0u;

        std::lock_guard<std::mutex> lock(g_spu2BridgeMutex);
        if (!g_activeSpu2State)
        {
            std::memset(outStereo, 0, static_cast<size_t>(frameCount) * 2u * sizeof(int16_t));
            g_lastActiveVoices.store(0u, std::memory_order_relaxed);
            return 0u;
        }

        detail::Spu2Mixer::MixStats stats{};
        g_spu2Mixer.mixBlock(*g_activeSpu2State, outStereo, frameCount, stats);
        g_lastActiveVoices.store(stats.activeVoices, std::memory_order_relaxed);
        g_lastKeyOns.store(stats.keyOns, std::memory_order_relaxed);

        int32_t prevPeak = g_peakAccum.load(std::memory_order_relaxed);
        while (stats.peakLevel > prevPeak &&
               !g_peakAccum.compare_exchange_weak(prevPeak, stats.peakLevel, std::memory_order_relaxed))
        {
        }
        return stats.activeVoices;
    }

    void getSpu2MixStats(uint32_t &outActiveVoices, uint64_t &outKeyOns, int32_t &outPeak)
    {
        outActiveVoices = g_lastActiveVoices.load(std::memory_order_relaxed);
        outKeyOns = g_lastKeyOns.load(std::memory_order_relaxed);
        outPeak = g_peakAccum.exchange(0, std::memory_order_relaxed);
    }
}
