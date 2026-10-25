#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "ps2_runtime.h"

#ifndef PS2_RAM_MASK
#define PS2_RAM_MASK 0x01FFFFFFu
#endif
#ifndef PS2_RAM_SIZE
#define PS2_RAM_SIZE 0x02000000u
#endif

namespace probe_ae {
    inline bool isEnabled() {
        static int s_state = -1;
        if (s_state == -1) {
            const char *env = std::getenv("PS2X_PROBE_AE");
            s_state = (env && (env[0] == '1' || env[0] == 't')) ? 1 : 0;
        }
        return s_state == 1;
    }

    // Task AE Counts
    extern std::atomic<uint32_t> cnt_F;
    extern std::atomic<uint32_t> cnt_ar_G;
    extern std::atomic<uint32_t> cnt_24c24c;
    extern std::atomic<uint32_t> cnt_24c4f8;
    extern std::atomic<uint32_t> cnt_24c620;
    extern std::atomic<uint32_t> cnt_24c64c;
    extern std::atomic<uint32_t> cnt_24c650;
    extern std::atomic<uint32_t> cnt_24c680;

    // Task AE Registers / state
    extern std::atomic<uint32_t> val_state;
    extern std::atomic<uint32_t> val_s0;
    extern std::atomic<uint32_t> val_a1_s4;
    extern std::atomic<uint32_t> val_f72C7A0;
    extern std::atomic<uint32_t> val_v0_1DA880;
    extern std::atomic<uint32_t> val_v0_taken;
    extern std::atomic<uint32_t> val_s4C;
    extern std::atomic<uint32_t> val_s2;
    extern std::atomic<uint32_t> val_s4C_10;
    extern std::atomic<uint32_t> val_s0_70;
    extern std::atomic<uint32_t> val_s48_4;
    extern std::atomic<uint32_t> val_taken_558;
    extern std::atomic<uint32_t> val_f72D3A0;
    extern std::atomic<uint32_t> val_w635480;
    extern std::atomic<uint32_t> val_v0_508684;
    extern std::atomic<uint32_t> val_w72D44C;
    extern std::atomic<uint32_t> val_s1_0C;
    extern std::atomic<uint32_t> val_t7_jalr;
    extern std::atomic<uint32_t> val_s1, val_s2_680, val_s3_680;

    // Function entries
    extern std::atomic<uint32_t> reg_F_a0, reg_F_a1, reg_F_a2, reg_F_s0, reg_F_s1;
    extern std::atomic<uint32_t> reg_G_a0, reg_G_a1;
    extern std::atomic<uint32_t> cnt_G_calls;

    // Task AF Probes
    extern std::atomic<uint32_t> cnt_24ce0c;
    extern std::atomic<uint32_t> val_v24cbfc;
    extern std::atomic<uint32_t> cnt_24ce14_tk;
    extern std::atomic<uint32_t> cnt_24cbfc;
    extern std::atomic<uint32_t> val_cbfc_a0;
    extern std::atomic<uint32_t> val_cbfc_10;
    extern std::atomic<uint32_t> val_cbfc_0c;
    extern std::atomic<uint32_t> val_1da880_1;
    extern std::atomic<uint32_t> val_1da880_2;
    extern std::atomic<uint32_t> cnt_24ce24;
    extern std::atomic<uint32_t> cnt_24cb38;
    extern std::atomic<uint32_t> val_cb38_a0;
    extern std::atomic<uint32_t> val_cb38_8;
    extern std::atomic<uint32_t> val_cb58_7c;
    extern std::atomic<uint32_t> cnt_24cb64;
    extern std::atomic<uint32_t> val_v3f75e8;
    extern std::atomic<uint32_t> cnt_24cbb8;
    extern std::atomic<uint32_t> val_cbb8_t6;
    extern std::atomic<uint32_t> val_s4a4_7c;
    extern std::atomic<uint32_t> val_s4a4_30;

    // Task AG Probes
    extern std::atomic<uint32_t> cnt_24cd04;
    extern std::atomic<uint32_t> val_cd04_a0;
    extern std::atomic<uint32_t> val_cd04_a2;
    extern std::atomic<uint32_t> val_cd04_a3;
    extern std::atomic<uint32_t> val_cd04_ra;
    extern std::atomic<uint32_t> val_cd50_t7;
    extern std::atomic<uint32_t> cnt_cd34_oob;

    extern std::atomic<int32_t> val_3f7a74_v0;
    extern std::atomic<uint32_t> cnt_3f7a74_v0_neg;
    extern std::atomic<uint32_t> val_3f7a74_s1;
    extern std::atomic<uint32_t> val_3f7a74_head;
    extern std::atomic<uint32_t> val_3f7a74_tail;

    extern std::atomic<uint32_t> val_1dc650_slot8;
    extern std::atomic<int32_t> val_1dc578_ret;

    void recordCd04Cmd(uint32_t cmd);
    std::string getAndResetCd04Cmds();

    void recordCd04Call(uint32_t cmd, uint32_t ra);
    void flushFirstCallsIfDue(uint32_t sec);

    void record1dc650A1(uint32_t a1);
    std::string getAndReset1dcA1();

    // Task AH Probes
    extern std::atomic<uint32_t> cnt_438ea8;
    extern std::atomic<uint32_t> cnt_1dcee0;
    extern std::atomic<uint32_t> val_1dcf8c_t6;
    extern std::atomic<uint32_t> val_1dcf8c_s1;
    extern std::atomic<uint32_t> cnt_1dcc70;
    extern std::atomic<int32_t>  val_1dcc70_v0;
    extern std::atomic<uint32_t> val_1dc1ec_t7;
    extern std::atomic<uint32_t> val_1dc1ec_s1;
    extern std::atomic<uint32_t> cnt_intc_dispatch;
    extern std::atomic<uint32_t> s_currentSec;

    // Task AJ Probes
    extern std::atomic<uint32_t> cnt_24c4c0;
    extern std::atomic<uint32_t> val_c4e0_v0;
    extern std::atomic<uint32_t> val_c4e0_s0_84;
    extern std::atomic<uint32_t> cnt_c4e0_lt;
    extern std::atomic<uint32_t> cnt_24c4f0;
    extern std::atomic<uint32_t> val_3f7780_a0;
    extern std::atomic<uint32_t> val_3f7780_a1;
    extern std::atomic<uint64_t> val_3f7780_mask;
    extern std::atomic<uint32_t> cnt_3fbe78;
    extern std::atomic<uint32_t> val_3fbe78_voice;
    extern std::atomic<uint32_t> val_3fbeac_v0;

    void logIntcHandlerDispatch(uint32_t addr);
    void logIntcRegister(bool dmac, uint32_t cause, uint32_t handler);
    void logSifSetCmdBuffer(uint32_t buf);
    void logSifAddCmdHandler(uint32_t cid, uint32_t handler, uint32_t arg);

    void recordRa24c64c(uint32_t ra);
    std::string getAndResetRa24c64c();

    void recordExit(uint32_t addr);
    std::string getAndResetExits();

    void recordTraceLabel(uint32_t labelAddr);
    void dumpTraceIfDue(uint32_t gCalls);

    void printOncePerSec(uint32_t sec, const uint8_t *rdram);

    void onBoFEntry();

    inline void onFEntry(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t s0, uint32_t s1) {
        if (!isEnabled()) return;
        cnt_F.fetch_add(1, std::memory_order_relaxed);
        reg_F_a0.store(a0, std::memory_order_relaxed);
        reg_F_a1.store(a1, std::memory_order_relaxed);
        reg_F_a2.store(a2, std::memory_order_relaxed);
        reg_F_s0.store(s0, std::memory_order_relaxed);
        reg_F_s1.store(s1, std::memory_order_relaxed);
        onBoFEntry();
    }

    inline void onGEntry(uint32_t a0, uint32_t a1) {
        if (!isEnabled()) return;
        reg_G_a0.store(a0, std::memory_order_relaxed);
        reg_G_a1.store(a1, std::memory_order_relaxed);
        uint32_t callNum = cnt_G_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        cnt_ar_G.fetch_add(1, std::memory_order_relaxed);
        dumpTraceIfDue(callNum);
    }

    inline void log1B0(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t t6 = getRegU32(ctx, 14);
        uint32_t s0 = getRegU32(ctx, 16);
        uint32_t s4 = getRegU32(ctx, 20);
        uint32_t byteVal = rdram ? rdram[0x72C7A0u & PS2_RAM_MASK] : 0;
        static uint32_t s_last_f_state = 0xFFFFFFFF;
        if (t6 != s_last_f_state) {
            std::printf("[probe:state] F state %u -> %u\n", s_last_f_state, t6);
            std::fflush(stdout);
            s_last_f_state = t6;
        }
        val_state.store(t6, std::memory_order_relaxed);
        val_s0.store(s0, std::memory_order_relaxed);
        val_a1_s4.store(s4, std::memory_order_relaxed);
        val_f72C7A0.store(byteVal, std::memory_order_relaxed);
    }

    inline void logPoll(R5900Context *ctx, bool taken) {
        if (!isEnabled()) return;
        val_v0_1DA880.store(getRegU32(ctx, 2), std::memory_order_relaxed);
        val_v0_taken.store(taken ? 1 : 0, std::memory_order_relaxed);
    }

    inline void logC500(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t s0 = getRegU32(ctx, 16);
        uint32_t s4C = 0;
        if (rdram && s0 < PS2_RAM_SIZE - 0x60) {
            std::memcpy(&s4C, rdram + ((s0 + 0x4C) & PS2_RAM_MASK), sizeof(s4C));
        }
        val_s4C.store(s4C, std::memory_order_relaxed);
    }

    inline void logC558(R5900Context *ctx, const uint8_t *rdram, bool taken) {
        if (!isEnabled()) return;
        uint32_t s0 = getRegU32(ctx, 16);
        uint32_t s2 = getRegU32(ctx, 18);
        uint32_t s4C = 0, s4C_10 = 0, s0_70 = 0, s48 = 0, s48_4 = 0;
        if (rdram && s0 < PS2_RAM_SIZE - 0x80) {
            std::memcpy(&s4C, rdram + ((s0 + 0x4C) & PS2_RAM_MASK), sizeof(s4C));
            std::memcpy(&s0_70, rdram + ((s0 + 0x70) & PS2_RAM_MASK), sizeof(s0_70));
            std::memcpy(&s48, rdram + ((s0 + 0x48) & PS2_RAM_MASK), sizeof(s48));
            if (s4C != 0 && s4C < PS2_RAM_SIZE - 0x20) {
                std::memcpy(&s4C_10, rdram + ((s4C + 0x10) & PS2_RAM_MASK), sizeof(s4C_10));
            }
            if (s48 != 0 && s48 < PS2_RAM_SIZE - 0x10) {
                std::memcpy(&s48_4, rdram + ((s48 + 4) & PS2_RAM_MASK), sizeof(s48_4));
            }
        }
        val_s2.store(s2, std::memory_order_relaxed);
        val_s4C_10.store(s4C_10, std::memory_order_relaxed);
        val_s0_70.store(s0_70, std::memory_order_relaxed);
        val_s48_4.store(s48_4, std::memory_order_relaxed);
        val_taken_558.store(taken ? 1 : 0, std::memory_order_relaxed);
    }

    inline void logC624(const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t b72D3A0 = rdram ? rdram[0x72D3A0u & PS2_RAM_MASK] : 0;
        uint32_t w635480 = 0;
        if (rdram) {
            std::memcpy(&w635480, rdram + (0x635480u & PS2_RAM_MASK), sizeof(w635480));
        }
        val_f72D3A0.store(b72D3A0, std::memory_order_relaxed);
        val_w635480.store(w635480, std::memory_order_relaxed);
    }

    inline void logC658(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t w72D44C = 0;
        if (rdram) {
            std::memcpy(&w72D44C, rdram + (0x72D44Cu & PS2_RAM_MASK), sizeof(w72D44C));
        }
        val_v0_508684.store(v0, std::memory_order_relaxed);
        val_w72D44C.store(w72D44C, std::memory_order_relaxed);
    }

    inline void logC668(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t s1 = getRegU32(ctx, 17);
        uint32_t b0C = (rdram && s1 < PS2_RAM_SIZE - 0x10) ? rdram[(s1 + 0xC) & PS2_RAM_MASK] : 0;
        val_s1_0C.store(b0C, std::memory_order_relaxed);
    }

    inline void logC680(R5900Context *ctx, uint32_t target) {
        if (!isEnabled()) return;
        val_t7_jalr.store(target, std::memory_order_relaxed);
        val_s1.store(getRegU32(ctx, 17), std::memory_order_relaxed);
        val_s2_680.store(getRegU32(ctx, 18), std::memory_order_relaxed);
        val_s3_680.store(getRegU32(ctx, 19), std::memory_order_relaxed);
    }

    void onBoLabel(uint32_t labelAddr);

    // Task AF inlines
    inline void log24CE0C() {
        if (!isEnabled()) return;
        cnt_24ce0c.fetch_add(1, std::memory_order_relaxed);
        onBoLabel(0x24CE0Cu);
    }

    inline void log24CE14(R5900Context *ctx, bool taken) {
        if (!isEnabled()) return;
        val_v24cbfc.store(getRegU32(ctx, 2), std::memory_order_relaxed);
        if (taken) cnt_24ce14_tk.fetch_add(1, std::memory_order_relaxed);
    }

    inline void log24CBFC(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        cnt_24cbfc.fetch_add(1, std::memory_order_relaxed);
        uint32_t a0 = getRegU32(ctx, 4);
        val_cbfc_a0.store(a0, std::memory_order_relaxed);
        uint32_t w10 = 0, w0c = 0;
        if (rdram && a0 < PS2_RAM_SIZE - 0x20) {
            std::memcpy(&w10, rdram + ((a0 + 0x10) & PS2_RAM_MASK), sizeof(w10));
            std::memcpy(&w0c, rdram + ((a0 + 0xC) & PS2_RAM_MASK), sizeof(w0c));
        }
        val_cbfc_10.store(w10, std::memory_order_relaxed);
        val_cbfc_0c.store(w0c, std::memory_order_relaxed);
    }

    inline void log1DA880_1(R5900Context *ctx) {
        if (!isEnabled()) return;
        val_1da880_1.store(getRegU32(ctx, 2), std::memory_order_relaxed);
    }

    inline void log1DA880_2(R5900Context *ctx) {
        if (!isEnabled()) return;
        val_1da880_2.store(getRegU32(ctx, 2), std::memory_order_relaxed);
    }

    inline void log24CE24() {
        if (!isEnabled()) return;
        cnt_24ce24.fetch_add(1, std::memory_order_relaxed);
    }

    inline void log24CB38(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        cnt_24cb38.fetch_add(1, std::memory_order_relaxed);
        uint32_t a0 = getRegU32(ctx, 4);
        val_cb38_a0.store(a0, std::memory_order_relaxed);
        uint32_t w8 = 0;
        if (rdram && a0 < PS2_RAM_SIZE - 0x10) {
            std::memcpy(&w8, rdram + ((a0 + 8) & PS2_RAM_MASK), sizeof(w8));
        }
        val_cb38_8.store(w8, std::memory_order_relaxed);
    }

    inline void log24CB58(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t b7c = (rdram && a0 < PS2_RAM_SIZE - 0x80) ? rdram[(a0 + 0x7C) & PS2_RAM_MASK] : 0;
        val_cb58_7c.store(b7c, std::memory_order_relaxed);
    }

    inline void log24CB64() {
        if (!isEnabled()) return;
        cnt_24cb64.fetch_add(1, std::memory_order_relaxed);
    }

    inline void log24CB6C(R5900Context *ctx) {
        if (!isEnabled()) return;
        val_v3f75e8.store(getRegU32(ctx, 2), std::memory_order_relaxed);
    }

    inline void log24CBB8(R5900Context *ctx) {
        if (!isEnabled()) return;
        cnt_24cbb8.fetch_add(1, std::memory_order_relaxed);
        val_cbb8_t6.store(getRegU32(ctx, 14), std::memory_order_relaxed);
    }

    inline void logState6(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        onBoLabel(0x24C4A4u);
        uint32_t s0 = getRegU32(ctx, 16);
        uint32_t b7c = 0, w30 = 0;
        if (rdram && s0 < PS2_RAM_SIZE - 0x80) {
            b7c = rdram[(s0 + 0x7C) & PS2_RAM_MASK];
            std::memcpy(&w30, rdram + ((s0 + 0x30) & PS2_RAM_MASK), sizeof(w30));
        }
        val_s4a4_7c.store(b7c, std::memory_order_relaxed);
        val_s4a4_30.store(w30, std::memory_order_relaxed);
    }

    // Task AG inlines
    inline void log24CD04Entry(R5900Context *ctx) {
        if (!isEnabled()) return;
        cnt_24cd04.fetch_add(1, std::memory_order_relaxed);
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        uint32_t ra = getRegU32(ctx, 31);
        val_cd04_a0.store(a0, std::memory_order_relaxed);
        val_cd04_a2.store(a2, std::memory_order_relaxed);
        val_cd04_a3.store(a3, std::memory_order_relaxed);
        val_cd04_ra.store(ra, std::memory_order_relaxed);
        recordCd04Cmd(a1);
        recordCd04Call(a1, ra);
    }

    inline void log24CD34Oob() {
        if (!isEnabled()) return;
        cnt_cd34_oob.fetch_add(1, std::memory_order_relaxed);
    }

    inline void log24CD50(uint32_t jumpTarget) {
        if (!isEnabled()) return;
        val_cd50_t7.store(jumpTarget, std::memory_order_relaxed);
    }

    inline void log3F7A74(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        int32_t v0 = (int32_t)getRegU32(ctx, 2);
        val_3f7a74_v0.store(v0, std::memory_order_relaxed);
        if (v0 < 0) {
            cnt_3f7a74_v0_neg.fetch_add(1, std::memory_order_relaxed);
        }
        uint32_t s1 = getRegU32(ctx, 17);
        val_3f7a74_s1.store(s1, std::memory_order_relaxed);
        uint32_t head = 0, tail = 0;
        if (rdram && s1 < PS2_RAM_SIZE - 0x230) {
            std::memcpy(&head, rdram + ((s1 + 0x228u) & PS2_RAM_MASK), sizeof(head));
            std::memcpy(&tail, rdram + ((s1 + 0x22Cu) & PS2_RAM_MASK), sizeof(tail));
        }
        val_3f7a74_head.store(head, std::memory_order_relaxed);
        val_3f7a74_tail.store(tail, std::memory_order_relaxed);
    }

    inline void log1DC650Entry(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        record1dc650A1(a1);
        if (a1 == 1) {
            uint32_t addr = a0 + (1u << 4) + 0x3Cu;
            uint32_t w = 0;
            if (rdram && addr < PS2_RAM_SIZE - 4) {
                std::memcpy(&w, rdram + (addr & PS2_RAM_MASK), sizeof(w));
            }
            val_1dc650_slot8.store(w, std::memory_order_relaxed);
        }
    }

    inline void log1DC670Ret(R5900Context *ctx) {
        if (!isEnabled()) return;
        int32_t ret = (int32_t)getRegU32(ctx, 2);
        val_1dc578_ret.store(ret, std::memory_order_relaxed);
    }

    // Task AH inlines
    inline void log438EA8Entry() {
        if (!isEnabled()) return;
        cnt_438ea8.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_sifFirstCount{0};
        uint32_t n = s_sifFirstCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            uint32_t sec = s_currentSec.load(std::memory_order_relaxed);
            std::printf("[probe] sif_first t=%u n=%u\n", sec, n);
            std::fflush(stdout);
        }
    }

    inline void log1DCEE0Entry() {
        if (!isEnabled()) return;
        cnt_1dcee0.fetch_add(1, std::memory_order_relaxed);
    }

    inline void log1DCF8C(R5900Context *ctx) {
        if (!isEnabled()) return;
        val_1dcf8c_t6.store(getRegU32(ctx, 14), std::memory_order_relaxed);
        val_1dcf8c_s1.store(getRegU32(ctx, 17), std::memory_order_relaxed);
    }

    inline void log1DCC70(R5900Context *ctx) {
        if (!isEnabled()) return;
        cnt_1dcc70.fetch_add(1, std::memory_order_relaxed);
        val_1dcc70_v0.store((int32_t)getRegU32(ctx, 2), std::memory_order_relaxed);
    }

    inline void log1DC1EC(R5900Context *ctx) {
        if (!isEnabled()) return;
        val_1dc1ec_t7.store(getRegU32(ctx, 15), std::memory_order_relaxed);
        val_1dc1ec_s1.store(getRegU32(ctx, 17), std::memory_order_relaxed);
    }

    // Task AJ inlines
    inline void log24C4C0() {
        if (!isEnabled()) return;
        cnt_24c4c0.fetch_add(1, std::memory_order_relaxed);
        onBoLabel(0x24C4C0u);
    }

    inline void log24C4E0(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t s0 = getRegU32(ctx, 16);
        uint32_t s0_84 = 0;
        if (rdram && s0 < PS2_RAM_SIZE - 0x90) {
            std::memcpy(&s0_84, rdram + ((s0 + 0x84u) & PS2_RAM_MASK), sizeof(s0_84));
        }
        val_c4e0_v0.store(v0, std::memory_order_relaxed);
        val_c4e0_s0_84.store(s0_84, std::memory_order_relaxed);
        if (v0 < s0_84) {
            cnt_c4e0_lt.fetch_add(1, std::memory_order_relaxed);
        }
    }

    inline void log24C4F0() {
        if (!isEnabled()) return;
        cnt_24c4f0.fetch_add(1, std::memory_order_relaxed);
    }

    inline void log3F7780Entry(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint64_t mask = 0;
        if (rdram && a0 < PS2_RAM_SIZE - 0xA8) {
            std::memcpy(&mask, rdram + ((a0 + 0xA0u) & PS2_RAM_MASK), sizeof(mask));
        }
        val_3f7780_a0.store(a0, std::memory_order_relaxed);
        val_3f7780_a1.store(a1, std::memory_order_relaxed);
        val_3f7780_mask.store(mask, std::memory_order_relaxed);
    }

    inline void log3FBE78(R5900Context *ctx) {
        if (!isEnabled()) return;
        cnt_3fbe78.fetch_add(1, std::memory_order_relaxed);
        val_3fbe78_voice.store(getRegU32(ctx, 4), std::memory_order_relaxed);
    }

    inline void log3FBEAC(R5900Context *ctx) {
        if (!isEnabled()) return;
        val_3fbeac_v0.store(getRegU32(ctx, 2), std::memory_order_relaxed);
    }

    // Task AN Probes
    extern std::atomic<uint32_t> cnt_24c5a0;
    extern std::atomic<uint32_t> cnt_24c5ac;
    extern std::atomic<uint32_t> cnt_3f7e38;
    extern std::atomic<uint32_t> cnt_31a240;
    extern std::atomic<uint32_t> cnt_31a2c0;
    extern std::atomic<uint32_t> val_s4C_refill;

    void log24C5A0(R5900Context *ctx, const uint8_t *rdram);
    void log24C5AC(R5900Context *ctx, const uint8_t *rdram, uint32_t pc);
    void log24C5E8(R5900Context *ctx, const uint8_t *rdram);
    void log24C24C(R5900Context *ctx, const uint8_t *rdram);
    void log31A240Entry(R5900Context *ctx, const uint8_t *rdram);
    void log31A2C0Ret(R5900Context *ctx);

    // Task AO Probes
    extern std::atomic<uint32_t> val_last_cmd;
    extern std::atomic<uint32_t> cnt_c8_24cdcc;
    extern std::atomic<uint32_t> cnt_c8_24cdfc;
    extern std::atomic<uint32_t> cnt_c8_24c7dc;
    extern std::atomic<uint32_t> cnt_c8_24c8b4;
    extern std::atomic<uint32_t> cnt_c8_cmd8_ret;

    void log24CD38(R5900Context *ctx);
    void log24CDBC(R5900Context *ctx, const uint8_t *rdram);
    void log24CDCC();
    void log24CDFC();
    void log24CD80(R5900Context *ctx);

    void log24C7DCEntry(R5900Context *ctx);
    void log24C828(R5900Context *ctx);
    void log24C8B4();
    void log24C900(R5900Context *ctx);
    void log24C914(R5900Context *ctx);

    // Task AP Probes
    void log24C8BC(R5900Context *ctx, const uint8_t *rdram);
    void log24C8E4(R5900Context *ctx, const uint8_t *rdram);
    void log24C8F4(R5900Context *ctx);

    // Task AR Probes
    void on31A3E8();
    void on35F4C4();
    void on15B750();
    void on31A52C();
    void on4E2730();
    void on4E914C();
    void logCause3Dispatch(const std::vector<uint32_t> &handlers);
    void logIntcHandlerReturn(uint32_t addr, uint32_t ret);
    void logRmIntc(const char *op, uint32_t cause, uint32_t ra);

    // Task AS Probes
    void recordSemaSignal(int semaId, uint32_t ra);
    void recordSemaWait(int semaId, uint32_t ra);
    void recordSemaPoll(int semaId, uint32_t ra);
    void getSemaLastRas(int semaId, uint32_t *lastSignalRa, uint32_t *lastWaitRa);
    void recordMainThreadDispatch(uint32_t pc);
    std::string getRecentMainDispatches();
    void dumpTaskASInfo(uint32_t sec, bool isFirstStuckSec);

    // Task AT Probes
    void dumpTaskATInfo(uint32_t sec, const uint8_t *rdram);

    // Task AU Probes
    void on3D7C08(R5900Context *ctx, const uint8_t *rdram);
    void on36C3B4(R5900Context *ctx, const uint8_t *rdram);
    void on165EFC(R5900Context *ctx, const uint8_t *rdram);

    // Task AV Probes
    extern std::atomic<uint32_t> cnt_vif1_kicks_started;
    extern std::atomic<uint32_t> cnt_vif1_kicks_finished;
    extern std::atomic<uint32_t> cnt_vif1_irq;
    extern std::atomic<uint32_t> cnt_cause5_raises;
    extern std::atomic<uint32_t> cnt_404230_loop;

    void onVif1DmaStart(uint32_t sizeBytes);
    void onVif1DmaEnd();
    void onVif1Irq();
    void onVif1Mark(uint16_t imm);
    void onCause5Raise();
    void on2843D4MarkRead(uint32_t markVal);
    void on404230LoopEntry();
    void onVifUnknown(uint8_t opcode, uint16_t imm);

    std::string getAndResetVifMarkHist();
    std::string getAndResetH2843D4Hist(uint32_t &outTotal);
    void dumpTaskAVInfo(uint32_t sec, const uint8_t *rdram);

    // Task AW Probes
    void onAwDma1KickStart(uint32_t tadr, uint32_t chcr);
    void onAwDma1Tag(uint32_t addr, uint32_t id, uint32_t qwc, uint32_t next, uint32_t dataAddr, const uint32_t *words, uint32_t wordCount);
    void onAwVifKickStart();
    void onAwMark2000();
    void onAwVifCode(uint32_t off, uint32_t cmd, uint8_t num, uint16_t imm, bool irq, uint32_t consumed);
    void onAwUnpack(uint8_t fmt, uint8_t num, uint32_t cl, uint32_t wl, uint32_t usn, uint32_t flg, uint32_t addr, uint32_t words);
    void onAwVifKickEnd(const uint8_t *rdram);

    // Task AY Probes
    struct AySpan {
        uint32_t bufOffset;
        uint32_t byteCount;
        uint32_t srcGuestAddr;
    };
    void onAyChainStart(uint32_t tagCount, const std::vector<AySpan> &spans);
    void onAyUnpackCheck(uint8_t fmt, uint32_t vuAddr, uint8_t num, uint32_t posPayload, const uint8_t *payload, uint32_t sizeBytes);
    void onAyRaw8(uint32_t callNum, uint32_t dst, uint32_t a2, const uint8_t *pDst);

    void onAyTimeVif1(uint64_t us);
    void onAyTimeVu1(uint64_t us);
    void onAyTimeGif(uint64_t us);
    void onAyTimeChain(uint64_t us);
    void onAyKick();
    void dumpAyTimeIfDue(uint32_t sec);

    void onAyPrim(const char *type, int x0, int y0, int x1, int y1, uint32_t rgba, const char *tex, uint64_t alpha, uint64_t test, uint32_t fbp);
    void onAyCheckWhiteStrip(uint32_t frame, uint32_t fbp, uint32_t fbw, uint32_t psm, const uint8_t *pixels, uint32_t width, uint32_t height);

    // Task AZ Probes
    void onAzPop(uint32_t head, uint32_t tail, uint32_t addr, uint32_t size, const uint8_t *payload, uint32_t outFrames);
    void onAzRecordFramesOut(uint32_t count);
    void onAzRaw8Call(bool haveFrame, uint32_t width, uint32_t height, const uint8_t *mb0);
    void dumpAzRingIfDue(uint32_t sec, const uint8_t *rdram);

    // Task BA Probes
    void onBaRows(bool haveFrame, uint32_t mbCols, uint32_t mbRows, uint32_t dst, const uint8_t *rdram);
    void onBaMscalStart(uint32_t startPC, const int32_t *vi);
    void onBaMscntStart(const int32_t *vi);
    void onBaInstruction(uint32_t pc, uint32_t upper, uint32_t lower);
    void onBaXgkick(uint32_t qwords, uint64_t xgkickNs, uint32_t prims);
    void onBaRunEnd(uint32_t instructionsExecuted, uint64_t loopNs);
    void dumpBaVu1IfDue(uint32_t sec);

    // Task BB Probes
    void onBbChainStart(uint32_t tadr, uint32_t tagCount, const std::vector<AySpan> &spans);
    void onBbVif1Mscal(uint32_t startPC, uint32_t itop, uint32_t top, uint32_t tops,
                       uint32_t base, uint32_t ofst, uint32_t cl, uint32_t wl, uint32_t mode);
    void onBbRecordUnpack(uint32_t posPayload, uint32_t totalBytes, uint32_t vuAddr, uint8_t fmt, uint8_t num);
    void onBbRunStart(uint32_t startPC, const uint8_t *vuCode, uint32_t codeSize,
                      const uint8_t *vuData, uint32_t dataSize,
                      const float vf[32][4], const int32_t vi[16],
                      const float acc[4], float q, float p, float i);
    void onBbXgkickPacket(const uint8_t *data, uint32_t byteCount);
    void onBbRunEnd(const uint8_t *vuData, uint32_t dataSize);
    void onGsWriteRegister(uint8_t regAddr, uint64_t value);
    void onGsPrivRegister(uint32_t addr, uint64_t value);
    void onBbCheckRaw8(bool validFrame, uint32_t numMbs, uint32_t dst, const uint8_t *rdram);
    void setRdram(const uint8_t *rdram);

    // Task BF Probes & Movie HLE
    bool isMovieHleActive();
    bool isMovieHleVerify();
    bool isMovieHleFrameMode();
    bool handleMovieHleMscal(uint32_t startPC, const uint8_t *vuCode, uint32_t codeSize, const uint8_t *vuData, uint32_t dataSize, uint32_t top, class GS &gs, class PS2Memory *memory);
    inline bool handleMovieHleMscal(uint32_t startPC, const uint8_t *vuCode, uint32_t codeSize, class GS &gs, class PS2Memory *memory) {
        return handleMovieHleMscal(startPC, vuCode, codeSize, nullptr, 0, 0, gs, memory);
    }

    // Task BH Probes
    bool isMovieActivePublic();
    void bhInitStartup();
    void onBhVif1Start();
    void onBhVif1End();
    void onBhSubmitGifStart();
    void onBhSubmitGifEnd();
    void onBhGsBlockWait(uint64_t waitUs);
    void onBhRaw8FeedTime(uint64_t us);
    void onBhRaw8DecTime(uint64_t us);
    void onBhRaw8YccTime(uint64_t us);
    void onBhRaw8Call();
    void onBhMovieHleTime(uint64_t us);
    void onBhSchedWait(uint64_t waitUs);
    void onBhDmaPollTime(uint64_t us);
    void onBhCid1Time(uint64_t us);
    void onBhIntcHandler(uint32_t cause, uint64_t execUs);
    void onBhGsPacket(uint64_t packetUs, uint32_t primType, uint32_t pixelCount, bool texOn, bool blendOn, uint32_t tw, uint32_t th);
    void onBhGsRasterTime(uint64_t us);
    void onBhRecordQueueDepth(uint32_t depth);
    void onBhSleepSiteHit(int siteId, uint64_t us);
    void onBhIteration();
    void bhTickCheckPerSec(const uint8_t *rdram);

    // Task BL
    extern std::atomic<uint32_t> cnt_bl_3cd320;
    extern std::atomic<uint32_t> cnt_bl_3ca818;
    extern std::atomic<uint32_t> cnt_bl_435a00;
    extern std::atomic<uint32_t> cnt_bl_3cbfd0;
    extern std::atomic<uint32_t> cnt_bl_3cd288;

    void onBlCid1Req(uint32_t opcode, uint32_t slotPtr, uint32_t seq, uint32_t fd, uint32_t dst, uint32_t size, uint32_t flags, const char *path);
    void onBlCid1Reply(uint32_t slotPtr, uint32_t seq, uint32_t result, uint32_t written);
    void onBlStallDiag(const uint8_t *rdram, const class EeScheduler *scheduler, uint32_t sec, uint32_t curScene);
    void onBlStuckSec(uint32_t sec, uint32_t mainPc, uint32_t mainRa);

    bool isMovieKickNative();
    bool isMovieSkipUnpackEnabled();
    void recordBlSkipUnpack(uint32_t skippedCount, uint32_t skippedBytes);

    // Task BM
    void onBmCid1Req(uint32_t opcode, uint32_t seq);
    void onBmCid1(uint32_t op, uint32_t seq, uint32_t dev30_before, uint32_t dev30_after);
    void setScheduler(const class EeScheduler *sched);

    // Task BN
    struct BnHook_24BFF8 {
        const uint8_t *rdram;
        R5900Context *ctx;
        BnHook_24BFF8(const uint8_t *r, R5900Context *c);
        ~BnHook_24BFF8();
    };
    struct BnHook_1DA880 {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, a1, a2, a3, dev30_in;
        BnHook_1DA880(const uint8_t *r, R5900Context *c);
        ~BnHook_1DA880();
    };
    struct BnHook_1DB798 {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, a1, dev30_in;
        BnHook_1DB798(const uint8_t *r, R5900Context *c);
        ~BnHook_1DB798();
    };
    struct BnHook_1DB164 {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, a1, a2, dev30_in;
        uint32_t ra_in, sp_in;
        BnHook_1DB164(const uint8_t *r, R5900Context *c);
        ~BnHook_1DB164();
    };
    struct BnHook_1DBA64 {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, dev30_in;
        uint32_t ra_in, sp_in;
        BnHook_1DBA64(const uint8_t *r, R5900Context *c);
        ~BnHook_1DBA64();
    };
    struct BnHook_1DBAA0 {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, a1, dev30_in;
        uint32_t ra_in, sp_in;
        BnHook_1DBAA0(const uint8_t *r, R5900Context *c);
        ~BnHook_1DBAA0();
    };
    struct BnHook_24CBFC {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, dev30_in;
        BnHook_24CBFC(const uint8_t *r, R5900Context *c);
        ~BnHook_24CBFC();
    };

    // Task BO
    struct BoHook_1DC67C {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, a1, a2, a3, t0, dev30_in;
        uint32_t a1_in[4];
        uint32_t ra_in, sp_in;
        BoHook_1DC67C(const uint8_t *r, R5900Context *c);
        ~BoHook_1DC67C();
    };
    struct BoHook_1DC578 {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, a1, dev30_in;
        uint32_t ra_in, sp_in;
        BoHook_1DC578(const uint8_t *r, R5900Context *c);
        ~BoHook_1DC578();
    };
    struct BoHook_1DD010 {
        const uint8_t *rdram;
        R5900Context *ctx;
        uint32_t a0, a1, dev30_in;
        uint32_t ra_in, sp_in;
        BoHook_1DD010(const uint8_t *r, R5900Context *c);
        ~BoHook_1DD010();
    };

    void on24CD58(R5900Context *ctx);
    void on24CDAC(R5900Context *ctx);
    void onBoCid1Req(uint32_t opcode, uint32_t fd, uint32_t slotPtr, uint32_t seq);

    // Task BQ Probes
    void onBqTickSec(const uint8_t *rdram, double ee_ms);
    void onBqDumpRpcAndSg2(const uint8_t *rdram);
    void onBqGetStats(uint32_t &sif_pending, uint32_t &sg2_polls, uint32_t &cid1_req);
    void recordSemaSignalBQ(int semaId, uint32_t ra, int threadId);
    void recordSemaWaitBQ(int semaId, uint32_t ra, int threadId);

    // Task BT Probes
    void onBtFunctionEntry(uint32_t addr, const struct R5900Context *ctx, const uint8_t *rdram);
    void onBtTickSec(const uint8_t *rdram);

    // Task BU Probes
    struct BuPreState {
        uint32_t preState;
        uint32_t a0;
        uint32_t a1;
        uint32_t ra;
        int32_t slotC_id;
        int32_t slot10_id;
        uint32_t slotC_w[4];
        uint32_t slot10_w[4];
    };
    void onBuPumpEntry(const struct R5900Context *ctx, const uint8_t *rdram, BuPreState *pre);
    void onBuPumpExit(const struct R5900Context *ctx, const uint8_t *rdram, const BuPreState *pre);
    void onBuTickSec(const uint8_t *rdram);

    // Task BV Probes
    void onBvReadSubmit(const struct R5900Context *ctx, const uint8_t *rdram);
    void onBvTickSec();
    void recordBvMainWait(int kind, int id, uint32_t ra, double duration_ms);
    void recordBvIntcDelivery(uint32_t cause);
    uint64_t probe_getCid1Fd3Offset();

    // Task BW Probes
    void onBwFramePresented(uint32_t width, uint32_t height, const uint8_t *rgba, const uint8_t *rdram, const class PS2Runtime *rt);
    void onBwMscalBlock(int32_t mbX, int32_t mbY);
    void onBwDecPicture(int pict_type, int width, int height);
    void onBwDecError(const char *err);
    void onBwFeedChunk(const uint8_t *data, size_t size);
    void onBwRaw8End(bool ok, int pops, uint32_t bytesIn, uint32_t underflow);
    void onBwTickSec();

    // Task BX Probes
    void onBxKick();
    void onBxNativeMscal(int32_t mbX, int32_t mbY, uint32_t top);
    void onBxOrigRunStart(uint32_t startPC, uint32_t top, const uint8_t *vuData, uint32_t dataSize);
    void onBxOrigRunXgkick(const uint8_t *data, uint32_t byteCount);
    void onBxOrigRunEnd();
    void onBxPresentFrame(uint32_t frameNum, uint32_t displayFbp, uint32_t sourceFbp, bool preferred,
                          uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t currentTick);
    void onBxDrawPrimitive(const struct GSPrimitiveBatch &batch);
    void onBxTickSec(const uint8_t *rdram);

    // Task BZ Probes
    void onBzInitEntry(const struct R5900Context *ctx, const uint8_t *rdram);
    void onBzInitExit(const struct R5900Context *ctx, const uint8_t *rdram);
    void onBzRaw8Call(uint32_t frameCount, uint32_t mbCapacity);
    void onBzNativeBlock(int32_t mbX, int32_t mbY);
    void onBzOrigRunBlock();
    void onBzTickSec();

    // Task CA Probes
    void recordCaMscal(uint32_t pc);
    void checkCaDump(uint32_t startPC, uint32_t top, const uint8_t *vuData, uint32_t dataSize);
    void onCaTickSec();

    // Task CB Probes
    struct CbPumpPre {
        uint32_t pre58;
    };
    struct CbDispPre {
        uint32_t a0, a1, a2, a3;
    };
    void onCbDispEntry(const struct R5900Context *ctx, const uint8_t *rdram, CbDispPre *pre);
    void onCbDispExit(const struct R5900Context *ctx, const uint8_t *rdram, const CbDispPre *pre);
    void onCbPumpEntry(const struct R5900Context *ctx, const uint8_t *rdram, CbPumpPre *pre);
    void onCbPumpExit(const struct R5900Context *ctx, const uint8_t *rdram, const CbPumpPre *pre);
    void onCbRingCall(const struct R5900Context *ctx, const uint8_t *rdram);
    void onCbCid1Submit(const struct R5900Context *ctx, const uint8_t *rdram);

    // Task CC Probes
    void onCc3F7780Exit(const struct R5900Context *ctx, const uint8_t *rdram, uint32_t a0);
    void onCc3F7850Entry(const struct R5900Context *ctx, const uint8_t *rdram);
    void onCc3FBE78Entry(const struct R5900Context *ctx);
    void onCc3FBEACExit(const struct R5900Context *ctx);
    void onCcSg2Call(uint32_t op, uint32_t arg, uint32_t ret);
    void onCcTickSec();

    // Task CE: Frame-locked guest time (PS2X_VTIME)
    bool isVTimeEnabled();
    bool onCeVblankProduced(uint32_t cause);
    void onCeVblankDelivered(uint32_t cause);
    uint64_t getCeDeliveredVblanks();
    void onCeTickSec();
    void recordMainGuestThread();
    void cgStartSamplerIfEnabled();
}
