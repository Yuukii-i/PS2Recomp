#include "spu2_state.h"

#include <algorithm>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t coreIndex(uint16_t entry) noexcept
        {
            return static_cast<uint32_t>(entry & 1u);
        }

        constexpr uint32_t voiceIndex(uint16_t entry) noexcept
        {
            const uint32_t v = static_cast<uint32_t>((entry >> 1u) & 0x1Fu);
            return v < Spu2State::kNumVoices ? v : 0u;
        }
    }

    Spu2State::Spu2State()
        : m_ram(kRamSize, 0u)
    {
        for (auto &c : m_cores)
        {
            c.vmixl = 0x00FFFFFFu;
            c.vmixr = 0x00FFFFFFu;
        }
    }

    void Spu2State::reset(bool clearRam)
    {
        {
            std::lock_guard<std::mutex> lock(m_regMutex);
            m_cores = {};
            for (auto &c : m_cores)
            {
                // PS2 sceSdInit enables dry mix for all voices by default.
                c.vmixl = 0x00FFFFFFu;
                c.vmixr = 0x00FFFFFFu;
            }
            m_pendingKon = {};
            m_pendingKoff = {};
            m_pendingReset = true;
            m_keyOnCount = 0u;
        }
        m_transChannels = {};
        {
            std::lock_guard<std::mutex> lock(m_regMutex);
            m_irqPending = {};
        }
        {
            std::lock_guard<std::mutex> admaLock(m_autoDmaMutex);
            for (auto &ring : m_autoDmaRings)
            {
                ring.active = false;
                ring.buffer.clear();
                ring.readBytePos = 0u;
            }
        }
        if (clearRam)
        {
            std::lock_guard<std::mutex> ramLock(m_ramMutex);
            if (m_ram.size() != kRamSize)
                m_ram.assign(kRamSize, 0u);
            else
                std::fill(m_ram.begin(), m_ram.end(), 0u);
        }
    }

    void Spu2State::writeRam(uint32_t spuAddr, const void *src, uint32_t size)
    {
        if (!src || size == 0u)
            return;

        std::lock_guard<std::mutex> ramLock(m_ramMutex);
        if (m_ram.size() < kRamSize)
            return;

        uint32_t dst = spuAddr % kRamSize;
        const auto *bytes = static_cast<const uint8_t *>(src);
        uint32_t remaining = size;
        while (remaining > 0u)
        {
            const uint32_t chunk = std::min(remaining, kRamSize - dst);
            std::memcpy(m_ram.data() + dst, bytes, chunk);
            bytes += chunk;
            remaining -= chunk;
            dst = 0u;
        }
    }

    void Spu2State::readRam(uint32_t spuAddr, void *dst, uint32_t size) const
    {
        if (!dst || size == 0u)
            return;

        std::lock_guard<std::mutex> ramLock(m_ramMutex);
        if (m_ram.size() < kRamSize)
            return;

        uint32_t src = spuAddr % kRamSize;
        auto *bytes = static_cast<uint8_t *>(dst);
        uint32_t remaining = size;
        while (remaining > 0u)
        {
            const uint32_t chunk = std::min(remaining, kRamSize - src);
            std::memcpy(bytes, m_ram.data() + src, chunk);
            bytes += chunk;
            remaining -= chunk;
            src = 0u;
        }
    }

    void Spu2State::readRamBlock16(uint32_t spuAddr, uint8_t outBlock[16]) const
    {
        std::lock_guard<std::mutex> ramLock(m_ramMutex);
        if (m_ram.size() < kRamSize)
        {
            std::memset(outBlock, 0, 16u);
            return;
        }
        const uint32_t src = (spuAddr & ~0xFu) % kRamSize;
        if (src + 16u <= kRamSize)
        {
            std::memcpy(outBlock, m_ram.data() + src, 16u);
        }
        else
        {
            const uint32_t first = kRamSize - src;
            std::memcpy(outBlock, m_ram.data() + src, first);
            std::memcpy(outBlock + first, m_ram.data(), 16u - first);
        }
    }

    void Spu2State::setParam(uint16_t entry, uint16_t value)
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        Core &c = m_cores[coreIndex(entry)];
        Voice &v = c.voices[voiceIndex(entry)];
        switch (entry & 0xFF00u)
        {
        case 0x0000u: v.voll = value; break;   // SD_VP_VOLL
        case 0x0100u: v.volr = value; break;   // SD_VP_VOLR
        case 0x0200u: v.pitch = value; break;  // SD_VP_PITCH
        case 0x0300u: v.adsr1 = value; break;  // SD_VP_ADSR1
        case 0x0400u: v.adsr2 = value; break;  // SD_VP_ADSR2
        case 0x0500u: v.envx = value; break;   // SD_VP_ENVX
        case 0x0600u: v.volxl = value; break;  // SD_VP_VOLXL
        case 0x0700u: v.volxr = value; break;  // SD_VP_VOLXR
        case 0x0800u: c.mmix = value; break;   // SD_P_MMIX
        case 0x0900u: c.mvoll = value; break;  // SD_P_MVOLL
        case 0x0A00u: c.mvolr = value; break;  // SD_P_MVOLR
        case 0x0B00u: c.evoll = value; break;  // SD_P_EVOLL
        case 0x0C00u: c.evolr = value; break;  // SD_P_EVOLR
        case 0x0D00u: c.avoll = value; break;  // SD_P_AVOLL
        case 0x0E00u: c.avolr = value; break;  // SD_P_AVOLR
        case 0x0F00u: c.bvoll = value; break;  // SD_P_BVOLL
        case 0x1000u: c.bvolr = value; break;  // SD_P_BVOLR
        case 0x1100u: c.mvolxl = value; break; // SD_P_MVOLXL
        case 0x1200u: c.mvolxr = value; break; // SD_P_MVOLXR
        default: break;
        }
    }

    uint16_t Spu2State::getParam(uint16_t entry) const
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        const Core &c = m_cores[coreIndex(entry)];
        const Voice &v = c.voices[voiceIndex(entry)];
        switch (entry & 0xFF00u)
        {
        case 0x0000u: return v.voll;
        case 0x0100u: return v.volr;
        case 0x0200u: return v.pitch;
        case 0x0300u: return v.adsr1;
        case 0x0400u: return v.adsr2;
        case 0x0500u: return v.envx;
        case 0x0600u: return v.volxl;
        case 0x0700u: return v.volxr;
        case 0x0800u: return c.mmix;
        case 0x0900u: return c.mvoll;
        case 0x0A00u: return c.mvolr;
        case 0x0B00u: return c.evoll;
        case 0x0C00u: return c.evolr;
        case 0x0D00u: return c.avoll;
        case 0x0E00u: return c.avolr;
        case 0x0F00u: return c.bvoll;
        case 0x1000u: return c.bvolr;
        case 0x1100u: return c.mvolxl;
        case 0x1200u: return c.mvolxr;
        default: return 0u;
        }
    }

    void Spu2State::setSwitch(uint16_t entry, uint32_t value)
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        const uint32_t ci = coreIndex(entry);
        Core &c = m_cores[ci];
        const uint32_t mask = value & 0x00FFFFFFu;
        switch (entry & 0xFF00u)
        {
        case 0x1300u: c.pmon = mask; break;   // SD_S_PMON
        case 0x1400u: c.non = mask; break;    // SD_S_NON
        case 0x1500u:                         // SD_S_KON
            c.kon |= mask;
            c.koff &= ~mask;
            c.endx &= ~mask;
            m_pendingKon[ci] |= mask;
            m_pendingKoff[ci] &= ~mask;
            for (uint32_t i = 0u; i < kNumVoices; ++i)
            {
                if ((mask & (1u << i)) != 0u)
                {
                    c.voices[i].nax = c.voices[i].ssa;
                    ++m_keyOnCount;
                }
            }
            break;
        case 0x1600u:                         // SD_S_KOFF
            c.koff |= mask;
            c.kon &= ~mask;
            m_pendingKoff[ci] |= mask;
            m_pendingKon[ci] &= ~mask;
            break;
        case 0x1700u: c.endx &= ~mask; break; // SD_S_ENDX (write clears)
        case 0x1800u: c.vmixl = mask; break;  // SD_S_VMIXL
        case 0x1900u: c.vmixel = mask; break; // SD_S_VMIXEL
        case 0x1A00u: c.vmixr = mask; break;  // SD_S_VMIXR
        case 0x1B00u: c.vmixer = mask; break; // SD_S_VMIXER
        default: break;
        }
    }

    uint32_t Spu2State::getSwitch(uint16_t entry) const
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        const Core &c = m_cores[coreIndex(entry)];
        switch (entry & 0xFF00u)
        {
        case 0x1300u: return c.pmon;
        case 0x1400u: return c.non;
        case 0x1500u: return c.kon;
        case 0x1600u: return c.koff;
        case 0x1700u: return c.endx;
        case 0x1800u: return c.vmixl;
        case 0x1900u: return c.vmixel;
        case 0x1A00u: return c.vmixr;
        case 0x1B00u: return c.vmixer;
        default: return 0u;
        }
    }

    void Spu2State::setAddr(uint16_t entry, uint32_t value)
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        Core &c = m_cores[coreIndex(entry)];
        Voice &v = c.voices[voiceIndex(entry)];
        const uint32_t addr = value & 0x00FFFFFFu;
        switch (entry & 0xFF00u)
        {
        case 0x1C00u: c.eea = addr; break;  // SD_A_EEA
        case 0x1D00u: c.esa = addr; break;  // SD_A_ESA
        case 0x1E00u: c.irqa = addr; break; // SD_A_IRQA
        case 0x2000u:                       // SD_VA_SSA
            v.ssa = addr;
            v.lsaxExplicit = false;
            break;
        case 0x2100u:                       // SD_VA_LSAX
            v.lsax = addr;
            v.lsaxExplicit = true;
            break;
        case 0x2200u: v.nax = addr; break;  // SD_VA_NAX
        default: break;
        }
    }

    uint32_t Spu2State::getAddr(uint16_t entry) const
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        const Core &c = m_cores[coreIndex(entry)];
        const Voice &v = c.voices[voiceIndex(entry)];
        switch (entry & 0xFF00u)
        {
        case 0x1C00u: return c.eea;
        case 0x1D00u: return c.esa;
        case 0x1E00u: return c.irqa;
        case 0x2000u: return v.ssa;
        case 0x2100u: return v.lsax;
        case 0x2200u: return v.nax;
        default: return 0u;
        }
    }

    void Spu2State::setCoreAttr(uint16_t entry, uint16_t value)
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        Core &c = m_cores[coreIndex(entry)];
        switch (entry & 0xFFFEu)
        {
        case 0x0002u: c.effectEnable = value; break; // SD_C_EFFECT_ENABLE
        case 0x0004u: c.irqEnable = value; break;    // SD_C_IRQ_ENABLE
        case 0x0006u: c.muteEnable = value; break;   // SD_C_MUTE_ENABLE
        case 0x0008u: c.noiseClk = value; break;     // SD_C_NOISE_CLK
        case 0x000Au: c.spdifMode = value; break;    // SD_C_SPDIF_MODE
        default: break;
        }
    }

    uint16_t Spu2State::getCoreAttr(uint16_t entry) const
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        const Core &c = m_cores[coreIndex(entry)];
        switch (entry & 0xFFFEu)
        {
        case 0x0002u: return c.effectEnable;
        case 0x0004u: return c.irqEnable;
        case 0x0006u: return c.muteEnable;
        case 0x0008u: return c.noiseClk;
        case 0x000Au: return c.spdifMode;
        default: return 0u;
        }
    }

    void Spu2State::setEffectAttr(int32_t core, int32_t mode, int16_t depthL, int16_t depthR, int32_t delay, int32_t feedback)
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        Core &c = m_cores[static_cast<uint32_t>(core) & 1u];
        c.effectAttr.mode = mode;
        c.effectAttr.depthL = depthL;
        c.effectAttr.depthR = depthR;
        c.effectAttr.delay = delay;
        c.effectAttr.feedback = feedback;
    }

    const Spu2State::EffectAttr &Spu2State::getEffectAttr(int32_t core) const
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        return m_cores[static_cast<uint32_t>(core) & 1u].effectAttr;
    }

    void Spu2State::snapshotForMixer(MixerSnapshot &outSnapshot)
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        outSnapshot.cores = m_cores;
        outSnapshot.pendingKon = m_pendingKon;
        outSnapshot.pendingKoff = m_pendingKoff;
        outSnapshot.pendingReset = m_pendingReset;
        outSnapshot.keyOnCount = m_keyOnCount;
        m_pendingKon = {};
        m_pendingKoff = {};
        m_pendingReset = false;
    }

    void Spu2State::commitMixerFeedback(const MixerFeedback &feedback)
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        for (uint32_t ci = 0u; ci < kNumCores; ++ci)
        {
            Core &c = m_cores[ci];
            if (feedback.irqTriggered[ci] && c.irqEnable != 0u)
                m_irqPending[ci] = true;
            const uint32_t retriggered = m_pendingKon[ci];
            c.endx |= (feedback.newlyEndedMask[ci] & ~retriggered);
            for (uint32_t vi = 0u; vi < kNumVoices; ++vi)
            {
                if ((retriggered & (1u << vi)) != 0u)
                    continue;
                Voice &v = c.voices[vi];
                const auto &u = feedback.voices[ci][vi];
                v.envx = u.envx;
                v.nax = u.nax;
                if (u.updateLsax && !v.lsaxExplicit)
                    v.lsax = u.lsax;
            }
        }
    }

    // PERF-AU3: AutoDMA (sceSdBlockTrans) ring buffer + IRQ helpers.
    void Spu2State::startBlockTrans(uint32_t ch, const void *initialData, uint32_t size)
    {
        std::lock_guard<std::mutex> lock(m_autoDmaMutex);
        auto &ring = m_autoDmaRings[ch & 1u];
        ring.active = (size >= 4u);
        ring.readBytePos = 0u;
        if (ring.active)
        {
            ring.buffer.resize(size);
            if (initialData)
                std::memcpy(ring.buffer.data(), initialData, size);
            else
                std::fill(ring.buffer.begin(), ring.buffer.end(), 0u);
        }
        else
        {
            ring.buffer.clear();
        }
    }

    void Spu2State::updateBlockTransHalf(uint32_t ch, uint32_t halfOffset, const void *data, uint32_t halfSize)
    {
        if (!data || halfSize == 0u)
            return;
        std::lock_guard<std::mutex> lock(m_autoDmaMutex);
        auto &ring = m_autoDmaRings[ch & 1u];
        if (!ring.active || halfOffset + halfSize > ring.buffer.size())
            return;
        std::memcpy(ring.buffer.data() + halfOffset, data, halfSize);
    }

    void Spu2State::stopBlockTrans(uint32_t ch)
    {
        std::lock_guard<std::mutex> lock(m_autoDmaMutex);
        auto &ring = m_autoDmaRings[ch & 1u];
        ring.active = false;
        ring.readBytePos = 0u;
    }

    bool Spu2State::readAutoDmaFrames(uint32_t ch, int16_t *outStereo, uint32_t frameCount)
    {
        if (!outStereo || frameCount == 0u)
            return false;
        std::lock_guard<std::mutex> lock(m_autoDmaMutex);
        auto &ring = m_autoDmaRings[ch & 1u];
        if (!ring.active || ring.buffer.size() < 4u)
            return false;
        const uint32_t ringSize = static_cast<uint32_t>(ring.buffer.size()) & ~3u;
        if (ringSize < 4u)
            return false;
        for (uint32_t f = 0u; f < frameCount; ++f)
        {
            const uint32_t pos = ring.readBytePos % ringSize;
            int16_t l = 0;
            int16_t r = 0;
            std::memcpy(&l, ring.buffer.data() + pos, sizeof(int16_t));
            std::memcpy(&r, ring.buffer.data() + pos + 2u, sizeof(int16_t));
            outStereo[f * 2u + 0u] = l;
            outStereo[f * 2u + 1u] = r;
            ring.readBytePos = (pos + 4u) % ringSize;
        }
        return true;
    }

    bool Spu2State::consumeIrqPending(uint32_t core) noexcept
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        bool &pending = m_irqPending[core & 1u];
        const bool was = pending;
        pending = false;
        return was;
    }
}
