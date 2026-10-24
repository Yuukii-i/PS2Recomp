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

    void recordRa24c64c(uint32_t ra);
    std::string getAndResetRa24c64c();

    void recordExit(uint32_t addr);
    std::string getAndResetExits();

    void recordTraceLabel(uint32_t labelAddr);
    void dumpTraceIfDue(uint32_t gCalls);

    void printOncePerSec(uint32_t sec, const uint8_t *rdram);

    inline void onFEntry(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t s0, uint32_t s1) {
        if (!isEnabled()) return;
        cnt_F.fetch_add(1, std::memory_order_relaxed);
        reg_F_a0.store(a0, std::memory_order_relaxed);
        reg_F_a1.store(a1, std::memory_order_relaxed);
        reg_F_a2.store(a2, std::memory_order_relaxed);
        reg_F_s0.store(s0, std::memory_order_relaxed);
        reg_F_s1.store(s1, std::memory_order_relaxed);
    }

    inline void onGEntry(uint32_t a0, uint32_t a1) {
        if (!isEnabled()) return;
        reg_G_a0.store(a0, std::memory_order_relaxed);
        reg_G_a1.store(a1, std::memory_order_relaxed);
        uint32_t callNum = cnt_G_calls.fetch_add(1, std::memory_order_relaxed) + 1;
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

    // Task AF inlines
    inline void log24CE0C() {
        if (!isEnabled()) return;
        cnt_24ce0c.fetch_add(1, std::memory_order_relaxed);
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
        uint32_t s0 = getRegU32(ctx, 16);
        uint32_t b7c = 0, w30 = 0;
        if (rdram && s0 < PS2_RAM_SIZE - 0x80) {
            b7c = rdram[(s0 + 0x7C) & PS2_RAM_MASK];
            std::memcpy(&w30, rdram + ((s0 + 0x30) & PS2_RAM_MASK), sizeof(w30));
        }
        val_s4a4_7c.store(b7c, std::memory_order_relaxed);
        val_s4a4_30.store(w30, std::memory_order_relaxed);
    }
}
