#include "spu2_mixer.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

void ps2xSafeLog(const char *, ...); // FIX-BD

namespace ps2x::iop // PROBE-AU3E
{
    namespace
    {
        std::array<std::array<std::atomic<int32_t>, detail::Spu2State::kNumVoices>, detail::Spu2State::kNumCores> g_voicePeakSinceKon{}; // PROBE-AU3E

        void resetAllVoicePeaks() noexcept // PROBE-AU3E
        {
            for (auto &core : g_voicePeakSinceKon)
                for (auto &v : core)
                    v.store(0, std::memory_order_relaxed);
        }
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int32_t kAdpcmFilter0[5] = { 0, 60, 115, 98, 122 };
        constexpr int32_t kAdpcmFilter1[5] = { 0, 0, -52, -55, -60 };

        // Preset tables from ps2sdk FREESD libsd iop/sound/libsd/src/effect.c (Academic Free License 2.0, TyRaNiD / Lukasz Bruun) // FIX-BD
        static const uint16_t EffectSizes[10] = { 0x1, 0x26C, 0x1F4, 0x484, 0x6FE, 0xADE, 0xF6C, 0x1804, 0x1804, 0x3C0 }; // FIX-BD
        static const uint32_t EffectParams[160] = { // FIX-BD
         0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00010001,0x00000000,0x00000000,0x00000000,0x00010001,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000, // FIX-BD
         0x005B007D,0x54B86D80,0x0000BED0,0xBA800000,0x53005800,0x033304D6,0x022703F0,0x01EF0374,0x01B70336,0x01B60335,0x01B50334,0x01B50334,0x01B50334,0x013601B4,0x005C00B8,0x80008000, // FIX-BD
         0x00250033,0x4FA870F0,0x4410BCE0,0x9C00C0F0,0x4EC05280,0x031B03E4,0x02AF03A4,0x02660372,0x025D031C,0x018E025C,0x0135022F,0x00B701D2,0x00B5018F,0x008000B4,0x0026004C,0x80008000, // FIX-BD
         0x007F00B1,0x4FA870F0,0x4510BCE0,0xB4C0BEF0,0x4EC05280,0x076B0904,0x065F0824,0x061607A2,0x05ED076C,0x042E05EC,0x0305050F,0x02B70462,0x0265042F,0x01B20264,0x00800100,0x80008000, // FIX-BD
         0x00A900E3,0x4FA86F60,0x4510BCE0,0xA680BEF0,0x52C05680,0x0B580DFB,0x0A3C0D09,0x09730BD9,0x08DA0B59,0x05E908D9,0x04B007EC,0x03D206EF,0x031D05EA,0x0238031C,0x00AA0154,0x80008000, // FIX-BD
         0x013901A5,0x50006000,0xB8004C00,0xC000BC00,0x5C006000,0x11BB15BA,0x10BD14C2,0x0DC111BC,0x0DC311C0,0x09C10DC0,0x07C10BC4,0x06CD0A00,0x05C109C2,0x041A05C0,0x013A0274,0x80008000, // FIX-BD
         0x0231033D,0x50007E00,0xB000B400,0xB0004C00,0x54006000,0x1A311ED6,0x183B1D14,0x16B21BC2,0x15EF1A32,0x105515EE,0x0F2D1334,0x0C5D11F6,0x0AE11056,0x07A20AE0,0x02320464,0x80008000, // FIX-BD
         0x00030003,0x7FFF7FFF,0x00000000,0x81000000,0x00000000,0x0FFD1FFD,0x00091009,0x00000000,0x00091009,0x1FFF1FFF,0x1FFE1FFE,0x1FFE1FFE,0x1FFE1FFE,0x10041008,0x00040008,0x80008000, // FIX-BD
         0x00030003,0x7FFF7FFF,0x00000000,0x00000000,0x00000000,0x0FFD1FFD,0x00091009,0x00000000,0x00091009,0x1FFF1FFF,0x1FFE1FFE,0x1FFE1FFE,0x1FFE1FFE,0x10041008,0x00040008,0x80008000, // FIX-BD
         0x00130017,0x4FA870F0,0x4510BCE0,0x8500BEF0,0x54C05F80,0x02AF0371,0x01DF02E5,0x01D702B0,0x026A0358,0x011E01D6,0x00B1012D,0x0059011F,0x00E301A0,0x00400058,0x00140028,0x80008000, // FIX-BD
        }; // FIX-BD

        static const bool g_reverbGate = []() noexcept // FIX-BD
        { // FIX-BD
            const char *env = std::getenv("PS2X_SPU2_REVERB"); // FIX-BD
            return !(env && std::strcmp(env, "0") == 0); // FIX-BD
        }(); // FIX-BD

        struct CoreReverbState // FIX-BD
        { // FIX-BD
            std::vector<int16_t> buf; // FIX-BD
            uint32_t pos = 0u; // FIX-BD
            uint32_t mode = 0u; // FIX-BD
            int32_t delay = 0; // FIX-BD
            int32_t feedback = 0; // FIX-BD
            uint16_t mode_data[32]{}; // FIX-BD
            int64_t accumL = 0; // FIX-BD
            int64_t accumR = 0; // FIX-BD
            uint32_t halfPhase = 0u; // FIX-BD
            int32_t lastLout = 0; // FIX-BD
            int32_t lastRout = 0; // FIX-BD
            bool wasActive = false; // FIX-BD
        }; // FIX-BD

        static CoreReverbState g_reverb[Spu2State::kNumCores]; // FIX-BD
        static uint32_t g_reverbActiveLogCount = 0u; // FIX-BD

        void resetReverbState() noexcept // FIX-BD
        { // FIX-BD
            for (auto &rev : g_reverb) // FIX-BD
            { // FIX-BD
                rev.buf.clear(); // FIX-BD
                rev.pos = 0u; // FIX-BD
                rev.mode = 0u; // FIX-BD
                rev.delay = 0; // FIX-BD
                rev.feedback = 0; // FIX-BD
                std::memset(rev.mode_data, 0, sizeof(rev.mode_data)); // FIX-BD
                rev.accumL = 0; // FIX-BD
                rev.accumR = 0; // FIX-BD
                rev.halfPhase = 0u; // FIX-BD
                rev.lastLout = 0; // FIX-BD
                rev.lastRout = 0; // FIX-BD
                rev.wasActive = false; // FIX-BD
            } // FIX-BD
        } // FIX-BD

        void rebuildReverb(CoreReverbState &rev, uint32_t mode, int32_t delay, int32_t feedback) // FIX-BD
        { // FIX-BD
            rev.mode = mode; // FIX-BD
            rev.delay = delay; // FIX-BD
            rev.feedback = feedback; // FIX-BD
            rev.buf.assign(static_cast<size_t>(EffectSizes[mode]) * 8u, 0); // FIX-BD
            rev.pos = 0u; // FIX-BD
            rev.accumL = 0; // FIX-BD
            rev.accumR = 0; // FIX-BD
            rev.halfPhase = 0u; // FIX-BD
            rev.lastLout = 0; // FIX-BD
            rev.lastRout = 0; // FIX-BD
            std::memcpy(rev.mode_data, &EffectParams[mode * 16u], sizeof(rev.mode_data)); // FIX-BD
            if (mode == 7u || mode == 8u) // FIX-BD
            { // FIX-BD
                const uint16_t d = static_cast<uint16_t>((static_cast<uint32_t>(delay) + 1u) << 5u); // FIX-BD
                const uint16_t fb = static_cast<uint16_t>(((static_cast<uint32_t>(feedback) << 7u) + static_cast<uint32_t>(feedback)) << 1u); // FIX-BD
                rev.mode_data[7] = fb; // FIX-BD
                rev.mode_data[10] = static_cast<uint16_t>((d << 1u) - rev.mode_data[0]); // FIX-BD
                rev.mode_data[11] = static_cast<uint16_t>(d - rev.mode_data[1]); // FIX-BD
                rev.mode_data[16] = static_cast<uint16_t>(rev.mode_data[17] + d); // FIX-BD
                rev.mode_data[12] = static_cast<uint16_t>(rev.mode_data[13] + d); // FIX-BD
                rev.mode_data[27] = static_cast<uint16_t>(rev.mode_data[29] + d); // FIX-BD
                rev.mode_data[26] = static_cast<uint16_t>(rev.mode_data[28] + d); // FIX-BD
            } // FIX-BD
        } // FIX-BD

        void stepReverb(CoreReverbState &rev, int64_t inL, int64_t inR) // FIX-BD
        { // FIX-BD
            const int32_t size = static_cast<int32_t>(rev.buf.size()); // FIX-BD
            if (size <= 0) // FIX-BD
                return; // FIX-BD

            const uint16_t *md = rev.mode_data; // FIX-BD
            auto rd = [&](int32_t sampleOff) -> int32_t { // FIX-BD
                int32_t idx = (static_cast<int32_t>(rev.pos) + sampleOff) % size; // FIX-BD
                if (idx < 0) idx += size; // FIX-BD
                return rev.buf[static_cast<size_t>(idx)]; // FIX-BD
            }; // FIX-BD
            auto wr = [&](int32_t sampleOff, int64_t val) { // FIX-BD
                int32_t idx = (static_cast<int32_t>(rev.pos) + sampleOff) % size; // FIX-BD
                if (idx < 0) idx += size; // FIX-BD
                rev.buf[static_cast<size_t>(idx)] = static_cast<int16_t>(std::clamp<int64_t>(val, -32768, 32767)); // FIX-BD
            }; // FIX-BD
            auto vol = [](uint16_t v) -> int64_t { return static_cast<int16_t>(v); }; // FIX-BD
            auto inVol = [](uint16_t v) -> int64_t { // FIX-BD
                const int16_t s = static_cast<int16_t>(v); // FIX-BD
                return (s == -32768) ? 32767 : static_cast<int64_t>(s); // FIX-BD
            }; // FIX-BD

            const int64_t Lin = (inL * inVol(md[30])) >> 15; // FIX-BD
            const int64_t Rin = (inR * inVol(md[31])) >> 15; // FIX-BD
            const int64_t vIIR = vol(md[2]); // FIX-BD
            const int64_t vWALL = vol(md[7]); // FIX-BD

            const int32_t prevLSame = rd(static_cast<int32_t>(md[10]) * 4 - 1); // FIX-BD
            wr(static_cast<int32_t>(md[10]) * 4, (((Lin + ((rd(static_cast<int32_t>(md[16]) * 4) * vWALL) >> 15) - prevLSame) * vIIR) >> 15) + prevLSame); // FIX-BD
            const int32_t prevRSame = rd(static_cast<int32_t>(md[11]) * 4 - 1); // FIX-BD
            wr(static_cast<int32_t>(md[11]) * 4, (((Rin + ((rd(static_cast<int32_t>(md[17]) * 4) * vWALL) >> 15) - prevRSame) * vIIR) >> 15) + prevRSame); // FIX-BD
            const int32_t prevLDiff = rd(static_cast<int32_t>(md[18]) * 4 - 1); // FIX-BD
            wr(static_cast<int32_t>(md[18]) * 4, (((Lin + ((rd(static_cast<int32_t>(md[25]) * 4) * vWALL) >> 15) - prevLDiff) * vIIR) >> 15) + prevLDiff); // FIX-BD
            const int32_t prevRDiff = rd(static_cast<int32_t>(md[19]) * 4 - 1); // FIX-BD
            wr(static_cast<int32_t>(md[19]) * 4, (((Rin + ((rd(static_cast<int32_t>(md[24]) * 4) * vWALL) >> 15) - prevRDiff) * vIIR) >> 15) + prevRDiff); // FIX-BD

            int64_t Lout = ((vol(md[3]) * rd(static_cast<int32_t>(md[12]) * 4)) >> 15) // FIX-BD
                         + ((vol(md[4]) * rd(static_cast<int32_t>(md[14]) * 4)) >> 15) // FIX-BD
                         + ((vol(md[5]) * rd(static_cast<int32_t>(md[20]) * 4)) >> 15) // FIX-BD
                         + ((vol(md[6]) * rd(static_cast<int32_t>(md[22]) * 4)) >> 15); // FIX-BD
            int64_t Rout = ((vol(md[3]) * rd(static_cast<int32_t>(md[13]) * 4)) >> 15) // FIX-BD
                         + ((vol(md[4]) * rd(static_cast<int32_t>(md[15]) * 4)) >> 15) // FIX-BD
                         + ((vol(md[5]) * rd(static_cast<int32_t>(md[21]) * 4)) >> 15) // FIX-BD
                         + ((vol(md[6]) * rd(static_cast<int32_t>(md[23]) * 4)) >> 15); // FIX-BD

            const int64_t vAPF1 = vol(md[8]); // FIX-BD
            const int32_t lapf1 = rd((static_cast<int32_t>(md[26]) - static_cast<int32_t>(md[0])) * 4); // FIX-BD
            Lout -= (vAPF1 * lapf1) >> 15; // FIX-BD
            wr(static_cast<int32_t>(md[26]) * 4, Lout); // FIX-BD
            Lout = ((Lout * vAPF1) >> 15) + lapf1; // FIX-BD

            const int32_t rapf1 = rd((static_cast<int32_t>(md[27]) - static_cast<int32_t>(md[0])) * 4); // FIX-BD
            Rout -= (vAPF1 * rapf1) >> 15; // FIX-BD
            wr(static_cast<int32_t>(md[27]) * 4, Rout); // FIX-BD
            Rout = ((Rout * vAPF1) >> 15) + rapf1; // FIX-BD

            const int64_t vAPF2 = vol(md[9]); // FIX-BD
            const int32_t lapf2 = rd((static_cast<int32_t>(md[28]) - static_cast<int32_t>(md[1])) * 4); // FIX-BD
            Lout -= (vAPF2 * lapf2) >> 15; // FIX-BD
            wr(static_cast<int32_t>(md[28]) * 4, Lout); // FIX-BD
            Lout = ((Lout * vAPF2) >> 15) + lapf2; // FIX-BD

            const int32_t rapf2 = rd((static_cast<int32_t>(md[29]) - static_cast<int32_t>(md[1])) * 4); // FIX-BD
            Rout -= (vAPF2 * rapf2) >> 15; // FIX-BD
            wr(static_cast<int32_t>(md[29]) * 4, Rout); // FIX-BD
            Rout = ((Rout * vAPF2) >> 15) + rapf2; // FIX-BD

            rev.lastLout = static_cast<int32_t>(std::clamp<int64_t>(Lout, -32768, 32767)); // FIX-BD
            rev.lastRout = static_cast<int32_t>(std::clamp<int64_t>(Rout, -32768, 32767)); // FIX-BD
            rev.pos = (rev.pos + 1u) % static_cast<uint32_t>(size); // FIX-BD
        } // FIX-BD

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
        resetAllVoicePeaks(); // PROBE-AU3E
        resetReverbState(); // FIX-BD
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
                    g_voicePeakSinceKon[ci][vi].store(0, std::memory_order_relaxed); // PROBE-AU3E
                }
                else if ((koff & bit) != 0u && v.active)
                {
                    v.adsrPhase = AdsrPhase::Release;
                    v.adsrCounter = 0u;
                }
            }
        }

        // PERF-AU3: Snapshot active AutoDMA PCM frames for each core before the sample loop.
        std::array<std::vector<int16_t>, Spu2State::kNumCores> autoDmaFrames{};
        std::array<bool, Spu2State::kNumCores> hasAutoDma{};
        for (uint32_t ci = 0u; ci < Spu2State::kNumCores; ++ci)
        {
            autoDmaFrames[ci].resize(static_cast<size_t>(frameCount) * 2u);
            hasAutoDma[ci] = state.readAutoDmaFrames(ci, autoDmaFrames[ci].data(), frameCount);
        }

        int32_t peak = 0;
        std::array<std::array<int32_t, Spu2State::kNumVoices>, Spu2State::kNumCores> blockVoicePeak{}; // PROBE-AU3E

        for (uint32_t f = 0u; f < frameCount; ++f)
        {
            int64_t totalL = 0;
            int64_t totalR = 0;

            for (uint32_t ci = 0u; ci < Spu2State::kNumCores; ++ci)
            {
                const Spu2State::Core &coreReg = m_snapshot.cores[ci];
                int64_t coreL = 0;
                int64_t coreR = 0;
                int64_t wetL = 0; // FIX-BD
                int64_t wetR = 0; // FIX-BD
                const uint32_t irqaAddr = coreReg.irqa & ~0xFu & (Spu2State::kRamSize - 1u);

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
                        if (coreReg.irqEnable != 0u && v.currentAddr == irqaAddr) // PERF-AU3
                            m_feedback.irqTriggered[ci] = true;
                    }

                    // Linear interpolation with 12-bit fractional phase (0x1000 = 48 kHz).
                    const int32_t curr = v.decoded[v.sampleIdx];
                    const int32_t prev = (v.sampleIdx > 0u) ? v.decoded[v.sampleIdx - 1u] : v.prevSample;
                    const int32_t frac = static_cast<int32_t>(v.fracPhase & 0x0FFFu);
                    const int32_t sample = prev + (((curr - prev) * frac) >> 12);

                    const int32_t envSample = (sample * v.envLevel) >> 15;
                    const uint32_t bit = 1u << vi;
                    int32_t vOutL = 0; // PROBE-AU3E
                    int32_t vOutR = 0; // PROBE-AU3E
                    const bool dryL = (coreReg.vmixl & bit) != 0u; // FIX-BD
                    const bool wetSendL = (coreReg.vmixel & bit) != 0u; // FIX-BD
                    if (dryL || wetSendL) // FIX-BD
                    { // FIX-BD
                        vOutL = static_cast<int32_t>((static_cast<int64_t>(envSample) * decodeFixedVol(reg.voll)) >> 14); // PROBE-AU3E
                        if (dryL) // FIX-BD
                            coreL += vOutL; // PROBE-AU3E
                        if (wetSendL) // FIX-BD
                            wetL += vOutL; // FIX-BD
                    } // FIX-BD
                    const bool dryR = (coreReg.vmixr & bit) != 0u; // FIX-BD
                    const bool wetSendR = (coreReg.vmixer & bit) != 0u; // FIX-BD
                    if (dryR || wetSendR) // FIX-BD
                    { // FIX-BD
                        vOutR = static_cast<int32_t>((static_cast<int64_t>(envSample) * decodeFixedVol(reg.volr)) >> 14); // PROBE-AU3E
                        if (dryR) // FIX-BD
                            coreR += vOutR; // PROBE-AU3E
                        if (wetSendR) // FIX-BD
                            wetR += vOutR; // FIX-BD
                    } // FIX-BD
                    const int32_t voiceAbs = std::max(std::abs(vOutL), std::abs(vOutR)); // PROBE-AU3E
                    if (voiceAbs > blockVoicePeak[ci][vi]) // PROBE-AU3E
                        blockVoicePeak[ci][vi] = voiceAbs; // PROBE-AU3E

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
                            if (coreReg.irqEnable != 0u && v.currentAddr == irqaAddr) // PERF-AU3
                                m_feedback.irqTriggered[ci] = true;
                        }
                    }
                }

                { // FIX-BD
                    CoreReverbState &rev = g_reverb[ci]; // FIX-BD
                    uint32_t revMode = static_cast<uint32_t>(coreReg.effectAttr.mode) & 0xFFu; // FIX-BD
                    if (revMode > 9u) // FIX-BD
                        revMode = 9u; // FIX-BD
                    const bool revActive = g_reverbGate && coreReg.effectEnable != 0u && revMode != 0u; // FIX-BD
                    if (revActive) // FIX-BD
                    { // FIX-BD
                        if (rev.buf.empty() || rev.mode != revMode || rev.delay != coreReg.effectAttr.delay || rev.feedback != coreReg.effectAttr.feedback) // FIX-BD
                            rebuildReverb(rev, revMode, coreReg.effectAttr.delay, coreReg.effectAttr.feedback); // FIX-BD
                        if (!rev.wasActive && g_reverbActiveLogCount < 10u) // FIX-BD
                        { // FIX-BD
                            ++g_reverbActiveLogCount; // FIX-BD
                            ps2xSafeLog("[rev] active core=%u mode=%u size=%u evol=%d/%d wetmask=%06x/%06x\n", // FIX-BD
                                        ci, revMode, static_cast<uint32_t>(rev.buf.size()), // FIX-BD
                                        static_cast<int>(static_cast<int16_t>(coreReg.evoll)), // FIX-BD
                                        static_cast<int>(static_cast<int16_t>(coreReg.evolr)), // FIX-BD
                                        coreReg.vmixel & 0xFFFFFFu, coreReg.vmixer & 0xFFFFFFu); // FIX-BD
                        } // FIX-BD
                        rev.accumL += wetL; // FIX-BD
                        rev.accumR += wetR; // FIX-BD
                        rev.halfPhase ^= 1u; // FIX-BD
                        if (rev.halfPhase == 0u) // FIX-BD
                        { // FIX-BD
                            stepReverb(rev, rev.accumL >> 1, rev.accumR >> 1); // FIX-BD
                            rev.accumL = 0; // FIX-BD
                            rev.accumR = 0; // FIX-BD
                        } // FIX-BD
                        coreL += (static_cast<int64_t>(rev.lastLout) * static_cast<int16_t>(coreReg.evoll)) >> 15; // FIX-BD
                        coreR += (static_cast<int64_t>(rev.lastRout) * static_cast<int16_t>(coreReg.evolr)) >> 15; // FIX-BD
                    } // FIX-BD
                    rev.wasActive = revActive; // FIX-BD
                } // FIX-BD

                if (hasAutoDma[ci]) // PERF-AU3: Mix AutoDMA PCM stream
                {
                    const int32_t admaL = autoDmaFrames[ci][f * 2u + 0u];
                    const int32_t admaR = autoDmaFrames[ci][f * 2u + 1u];
                    coreL += (static_cast<int64_t>(admaL) * decodeFixedVol(coreReg.bvoll)) >> 14;
                    coreR += (static_cast<int64_t>(admaR) * decodeFixedVol(coreReg.bvolr)) >> 14;
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
                if (blockVoicePeak[ci][vi] > g_voicePeakSinceKon[ci][vi].load(std::memory_order_relaxed)) // PROBE-AU3E
                    g_voicePeakSinceKon[ci][vi].store(blockVoicePeak[ci][vi], std::memory_order_relaxed); // PROBE-AU3E
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
        resetAllVoicePeaks(); // PROBE-AU3E
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

    int32_t getSpu2VoicePeak(uint32_t core, uint32_t voice) noexcept // PROBE-AU3E
    {
        if (core >= detail::Spu2State::kNumCores || voice >= detail::Spu2State::kNumVoices) // PROBE-AU3E
            return 0; // PROBE-AU3E
        return g_voicePeakSinceKon[core][voice].load(std::memory_order_relaxed); // PROBE-AU3E
    }
}
