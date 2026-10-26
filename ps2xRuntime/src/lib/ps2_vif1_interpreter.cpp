// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include "probe_ae.h"
#include <cstring>
#include <cstdio>
#include <chrono>

extern void recordCompletedIntcCause(uint32_t cause);

namespace probe_ae {
    void onMovieKickMscalCount(uint32_t count);
}

static uint32_t count0x48MscalsInKick(const uint8_t *data, uint32_t sizeBytes) {
    uint32_t count = 0;
    uint32_t pos = 0;
    uint32_t cycle = 0;
    while (pos + 4 <= sizeBytes) {
        uint32_t cmd;
        std::memcpy(&cmd, data + pos, 4);
        pos += 4;
        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        if (opcode == 0) continue;
        if (opcode == 0x01) { cycle = imm; continue; }
        if (opcode == 0x02 || opcode == 0x03 || opcode == 0x04 || opcode == 0x05 || opcode == 0x07) continue;
        if (opcode == 0x10 || opcode == 0x11 || opcode == 0x13) continue;
        if (opcode == 0x14 || opcode == 0x15) {
            if ((imm * 8u) == 0x048u) {
                count++;
            }
            continue;
        }
        if (opcode == 0x17) continue;
        if (opcode == 0x20) { pos += 4; continue; }
        if (opcode == 0x30 || opcode == 0x31) { pos += 16; continue; }
        if (opcode == 0x4A) {
            uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            pos += instructionCount * 8u;
            continue;
        }
        if (opcode == 0x50 || opcode == 0x51) {
            uint32_t qw = (imm == 0u) ? 65536u : static_cast<uint32_t>(imm);
            pos += qw * 16u;
            continue;
        }
        if ((opcode & 0x60) == 0x60) {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl) {
            case 0: bitsPerComponent = 32; break;
            case 1: bitsPerComponent = 16; break;
            case 2: bitsPerComponent = 8; break;
            case 3: bitsPerComponent = (vn == 3) ? 4 : 16; break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = cycle & 0xFFu; if (cl == 0) cl = 1;
            uint32_t wl = (cycle >> 8) & 0xFFu; if (wl == 0) wl = 1;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl) {
                uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t rem = writeVectorCount % wl;
                if (rem > cl) rem = cl;
                sourceVectorCount = fullBlocks * cl + rem;
            }
            uint32_t totalBytes = (sourceVectorCount * bytesPerVector + 3u) & ~3u;
            pos += totalBytes;
            continue;
        }
        break;
    }
    return count;
}

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    probe_ae::onBhVif1Start();
    probe_ae::onBxKick();
    uint32_t mscal0x48Count = count0x48MscalsInKick(data, sizeBytes);
    probe_ae::onMovieKickMscalCount(mscal0x48Count);
    uint32_t kickUnpacksSkipped = 0;
    uint32_t kickBytesSkipped = 0;
    auto tVif1Start = std::chrono::steady_clock::now();

#if defined(PS2X_PROBE_AE) || 1
    if (probe_ae::isEnabled()) {
        probe_ae::onVif1DmaStart(sizeBytes);
        probe_ae::onAwVifKickStart();
    }
#endif

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {

        uint32_t pos_cmd = pos;
        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;
        uint32_t pos_payload = pos;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        struct VcScopeGuard {
            uint32_t pos_cmd;
            uint32_t cmd;
            uint8_t num;
            uint16_t imm;
            bool irq;
            uint32_t pos_payload;
            const uint32_t &pos;
            ~VcScopeGuard() {
#if defined(PS2X_PROBE_AE) || 1
                if (probe_ae::isEnabled()) {
                    uint32_t consumed = (pos > pos_payload) ? (pos - pos_payload) : 0u;
                    probe_ae::onAwVifCode(pos_cmd, cmd, num, imm, irq, consumed);
                }
#endif
            }
        } vcGuard{pos_cmd, cmd, num, imm, irq, pos_payload, pos};

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
        {
            vif1_regs.stat |= (1u << 10); // VIS
            vif1_regs.stat |= (1u << 11); // INT
            recordCompletedIntcCause(5u);
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onVif1Irq();
                probe_ae::onCause5Raise();
            }
#endif
            static int s_traceVif = -1;
            if (s_traceVif == -1) {
                const char *env = std::getenv("PS2X_TRACE_VIF");
                s_traceVif = (env && (env[0] == '1' || env[0] == 't')) ? 1 : 0;
            }
            if (s_traceVif) {
                std::fprintf(stderr, "[vif1:irq] cmd=0x%08x opcode=0x%02x imm=0x%04x\n", cmd, opcode, imm);
                std::fflush(stderr);
            }
        }

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            m_ioRegisters[0x10003C30u] = imm;
            recordCompletedIntcCause(5u);
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onVif1Mark(imm);
                probe_ae::onCause5Raise();
                if (imm == 0x2000u) {
                    probe_ae::onAwMark2000();
                }
            }
#endif
            std::fprintf(stderr, "[vif1:mark] imm=0x%04x irq=%d\n", imm, irq ? 1 : 0);
            std::fflush(stderr);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onBbVif1Mscal(startPC, runItop, runTop, runTop,
                                        vif1_regs.base & 0x3FFu, vif1_regs.ofst & 0x3FFu,
                                        vif1_regs.cycle & 0xFFu, (vif1_regs.cycle >> 8) & 0xFFu,
                                        vif1_regs.mode & 3u);
            }
#endif

            const bool skipMscal = probe_ae::isMovieSkipUnpackEnabled() && probe_ae::isMovieKickNative();
            if (!skipMscal && m_vu1MscalCallback) {
                auto tVu1Start = std::chrono::steady_clock::now();
                m_vu1MscalCallback(startPC, runTop, runItop);
#if defined(PS2X_PROBE_AE) || 1
                if (probe_ae::isEnabled()) {
                    auto tVu1End = std::chrono::steady_clock::now();
                    probe_ae::onAyTimeVu1(std::chrono::duration_cast<std::chrono::microseconds>(tVu1End - tVu1Start).count());
                }
#endif
            }
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            probe_ae::flushMovieBatch(nullptr, this);
            if (m_vu1MscntCallback) {
                auto tVu1Start = std::chrono::steady_clock::now();
                m_vu1MscntCallback(runTop, runItop);
#if defined(PS2X_PROBE_AE) || 1
                if (probe_ae::isEnabled()) {
                    auto tVu1End = std::chrono::steady_clock::now();
                    probe_ae::onAyTimeVu1(std::chrono::duration_cast<std::chrono::microseconds>(tVu1End - tVu1Start).count());
                }
#endif
            }
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            probe_ae::flushMovieBatch(nullptr, this);
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                if (m_vif1PendingPath2ImageQwc != 0u)
                {
                    const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, qwCount);
                    std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
                    const uint64_t imageTag =
                        static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                        ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                        (static_cast<uint64_t>(kGifFmtImage) << 58);
                    std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
                    std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
                    submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, directHl);
                    m_vif1PendingPath2ImageQwc -= chunkQw;
                    if (m_vif1PendingPath2ImageQwc == 0u)
                    {
                        m_vif1PendingPath2DirectHl = false;
                    }
                }
                else
                {
                    submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);

                    const uint32_t pendingImageQw = pendingGifImageQwc(data + pos, qwCount * 16u);
                    if (pendingImageQw != 0u)
                    {
                        m_vif1PendingPath2ImageQwc = pendingImageQw;
                        m_vif1PendingPath2DirectHl = directHl;
                    }
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled() || probe_ae::isMovieHleFrameMode() || probe_ae::isMovieHleVerify()) {
                uint8_t fmt = opcode & 0x0Fu;
                uint32_t usn = (imm & 0x4000u) != 0u ? 1u : 0u;
                uint32_t flg = (imm & 0x8000u) != 0u ? 1u : 0u;
                uint32_t addr = imm & 0x3FFu;
                uint32_t computed_words = totalBytes / 4u;
                if (probe_ae::isEnabled()) {
                    probe_ae::onAwUnpack(fmt, num, cl, wl, usn, flg, addr, computed_words);
                    probe_ae::onAyUnpackCheck(fmt, addr, num, pos_payload, data + pos_payload, sizeBytes);
                }
                probe_ae::onBbRecordUnpack(pos_payload, totalBytes, vuAddr, fmt, num);
            }
#endif

            const bool skipUnpack = probe_ae::isMovieSkipUnpackEnabled() && probe_ae::isMovieKickNative();
            if (skipUnpack)
            {
                kickUnpacksSkipped++;
                kickBytesSkipped += totalBytes;
            }
            else if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;

                static std::atomic<uint64_t> s_ciFastCount{0};
                static std::atomic<uint64_t> s_ciGenericCount{0};
                static std::atomic<uint64_t> s_ciMismCount{0};
                static std::atomic<uint32_t> s_ciMismLogged{0};
                static std::chrono::steady_clock::time_point s_ciLastSecLog{};

                auto ciLogMsg = [](const char *msg) {
                    static FILE *s_f = nullptr;
                    static bool s_tried = false;
                    if (!s_tried) {
                        s_tried = true;
                        s_f = std::fopen("logs/probe_bh_sink.txt", "a");
                    }
                    if (s_f) {
                        std::fputs(msg, s_f);
                        std::fflush(s_f);
                    }
                    std::printf("%s", msg);
                };

                auto isUnpackFastEnabled = []() -> bool {
                    static int s_state = -1;
                    if (s_state == -1) {
                        const char *env = std::getenv("PS2X_UNPACK_FAST");
                        s_state = (env && (env[0] == '0' || env[0] == 'f' || env[0] == 'F')) ? 0 : 1;
                    }
                    return s_state == 1;
                };

                auto isUnpackVerifyEnabled = []() -> bool {
                    static int s_state = -1;
                    if (s_state == -1) {
                        const char *env = std::getenv("PS2X_UNPACK_VERIFY");
                        s_state = (env && (env[0] == '1' || env[0] == 't' || env[0] == 'T')) ? 1 : 0;
                    }
                    return s_state == 1;
                };

                const uint64_t lastVec = static_cast<uint64_t>(vuAddr) + static_cast<uint64_t>(writeVectorCount - 1u) * cl;
                const bool rangeOk = (lastVec < 0x400u) && ((lastVec + 1u) * 16u <= PS2_VU1_DATA_SIZE);
                const bool canFastPath = (cl >= wl && wl == 1u && sourceVectorCount == writeVectorCount &&
                                          vn == 0u && (vl == 0u || vl == 1u || vl == 2u) &&
                                          ((vif1_regs.mode & 3u) == 0u) && rangeOk);

                enum FieldAction : uint8_t {
                    FA_DECODED = 0,
                    FA_ROW     = 1,
                    FA_COL     = 2,
                    FA_PROTECT = 3
                };

                FieldAction fAction[4];
                uint32_t fConst[4];
                bool allDecoded = true;
                bool allProtect = true;

                if (canFastPath) {
                    for (uint32_t f = 0u; f < 4u; ++f) {
                        uint32_t spec = maskEnable ? ((vif1_regs.mask >> (f * 2u)) & 0x3u) : 0u;
                        fAction[f] = static_cast<FieldAction>(spec);
                        if (spec == 0u) {
                            fConst[f] = 0u;
                            allProtect = false;
                        } else if (spec == 1u) {
                            fConst[f] = vif1_regs.row[f];
                            allDecoded = false;
                            allProtect = false;
                        } else if (spec == 2u) {
                            fConst[f] = vif1_regs.col[0];
                            allDecoded = false;
                            allProtect = false;
                        } else {
                            fConst[f] = 0u;
                            allDecoded = false;
                        }
                    }
                }

                auto runFastPath = [&](uint8_t *targetVuData) {
                    if (allProtect) return;

                    if (allDecoded && cl == 1u) {
                        uint32_t *dest = reinterpret_cast<uint32_t*>(targetVuData) + (vuAddr * 4u);
                        if (vl == 2u) {
                            if (zeroExtend) {
                                for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                    uint32_t s = static_cast<uint32_t>(srcBase[i]);
                                    dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                    dest += 4;
                                }
                            } else {
                                for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                    uint32_t s = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcBase[i])));
                                    dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                    dest += 4;
                                }
                            }
                        } else if (vl == 1u) {
                            for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcBase + i * 2u, sizeof(raw));
                                uint32_t s = zeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                                dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                dest += 4;
                            }
                        } else { // vl == 0u
                            for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                uint32_t s = 0;
                                std::memcpy(&s, srcBase + i * 4u, sizeof(s));
                                dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                dest += 4;
                            }
                        }
                    } else {
                        for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                            uint32_t scalar = 0;
                            if (vl == 2u) {
                                uint8_t raw = srcBase[i];
                                scalar = zeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                            } else if (vl == 1u) {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcBase + i * 2u, sizeof(raw));
                                scalar = zeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                            } else {
                                std::memcpy(&scalar, srcBase + i * 4u, sizeof(scalar));
                            }

                            uint32_t *d = reinterpret_cast<uint32_t*>(targetVuData) + ((vuAddr + i * cl) * 4u);
                            if (fAction[0] == FA_DECODED) d[0] = scalar; else if (fAction[0] != FA_PROTECT) d[0] = fConst[0];
                            if (fAction[1] == FA_DECODED) d[1] = scalar; else if (fAction[1] != FA_PROTECT) d[1] = fConst[1];
                            if (fAction[2] == FA_DECODED) d[2] = scalar; else if (fAction[2] != FA_PROTECT) d[2] = fConst[2];
                            if (fAction[3] == FA_DECODED) d[3] = scalar; else if (fAction[3] != FA_PROTECT) d[3] = fConst[3];
                        }
                    }
                };

                const bool verifyActive = isUnpackVerifyEnabled();
                const bool fastActive = isUnpackFastEnabled();

                bool executeGeneric = true;
                std::vector<uint8_t> savedBefore;
                std::vector<uint8_t> fastResult;
                size_t verifyRangeBytes = 0;
                uint8_t *verifyRangeBase = nullptr;

                if (canFastPath && fastActive) {
                    if (verifyActive) {
                        const uint32_t minVec = vuAddr;
                        const uint32_t maxVec = static_cast<uint32_t>(lastVec);
                        verifyRangeBytes = (maxVec - minVec + 1u) * 16u;
                        verifyRangeBase = m_vu1Data + minVec * 16u;

                        savedBefore.assign(verifyRangeBase, verifyRangeBase + verifyRangeBytes);
                        fastResult.resize(verifyRangeBytes);

                        runFastPath(m_vu1Data);
                        std::memcpy(fastResult.data(), verifyRangeBase, verifyRangeBytes);
                        std::memcpy(verifyRangeBase, savedBefore.data(), verifyRangeBytes);

                        executeGeneric = true;
                    } else {
                        runFastPath(m_vu1Data);
                        s_ciFastCount.fetch_add(1, std::memory_order_relaxed);
                        executeGeneric = false;
                    }
                } else {
                    s_ciGenericCount.fetch_add(1, std::memory_order_relaxed);
                }

                if (executeGeneric)
                {
                    uint32_t srcIndex = 0u;
                    for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                    {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }

                if (canFastPath && fastActive && verifyActive) {
                    s_ciFastCount.fetch_add(1, std::memory_order_relaxed);
                    if (std::memcmp(fastResult.data(), verifyRangeBase, verifyRangeBytes) != 0) {
                        s_ciMismCount.fetch_add(1, std::memory_order_relaxed);
                        uint32_t logged = s_ciMismLogged.fetch_add(1, std::memory_order_relaxed);
                        if (logged < 10) {
                            char buf[256];
                            std::snprintf(buf, sizeof(buf), "[probe] ci unpack mismatch fmt=0x%02X num=%u vuAddr=0x%03X cl=%u wl=%u\n",
                                          opcode & 0x0Fu, num, vuAddr, cl, wl);
                            ciLogMsg(buf);
                        }
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_ciLastSecLog).count() >= 1) {
                s_ciLastSecLog = now;
                char buf[256];
                std::snprintf(buf, sizeof(buf), "[probe] ci unpack fast=%llu generic=%llu mism=%llu\n",
                              (unsigned long long)s_ciFastCount.load(std::memory_order_relaxed),
                              (unsigned long long)s_ciGenericCount.load(std::memory_order_relaxed),
                              (unsigned long long)s_ciMismCount.load(std::memory_order_relaxed));
                ciLogMsg(buf);
            }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onVifUnknown(opcode, imm);
            }
#endif
            continue;
        }
    }

#if defined(PS2X_PROBE_AE) || 1
    if (probe_ae::isEnabled()) {
        probe_ae::onVif1DmaEnd();
        probe_ae::onAwVifKickEnd(m_rdram);
        auto tVif1End = std::chrono::steady_clock::now();
        probe_ae::onAyTimeVif1(std::chrono::duration_cast<std::chrono::microseconds>(tVif1End - tVif1Start).count());
    }
#endif
    if (kickUnpacksSkipped > 0) {
        probe_ae::recordBlSkipUnpack(kickUnpacksSkipped, kickBytesSkipped);
    }
    probe_ae::onBhVif1End();
    probe_ae::flushMovieBatch(nullptr, this);
}
