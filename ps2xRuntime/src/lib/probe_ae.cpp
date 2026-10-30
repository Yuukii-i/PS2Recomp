#include "probe_ae.h"
#include <thread>

extern std::atomic<uint64_t> g_profPrims;
extern std::atomic<uint64_t> g_profPixels;
extern std::atomic<uint32_t> g_probe_F;
extern std::atomic<uint32_t> g_probe_G;
extern std::atomic<uint32_t> g_probe_4e2730;
extern std::atomic<uint32_t> g_probe_31a514;
extern std::atomic<uint32_t> g_trackedCurVtable;
extern std::atomic<uint32_t> g_trackedCurScene;
extern void ps2xSafeLog(const char *fmt, ...);

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char*, char*, unsigned long);
#endif

#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"
#include <mutex>
#include <map>
#include <deque>
#include <vector>
#include <array>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <chrono>
#include <unordered_set>
#include <set>
extern "C" void ps2_vu1_dumpDyPerSec(const uint8_t *rdram);

namespace probe_ae {
    extern std::atomic<uint32_t> s_duDecodesSinceLastPresent;
    extern std::atomic<uint32_t> s_duDecodesThisSec;
    extern std::atomic<uint32_t> s_duPresentsThisSec;
}
extern "C" uint32_t ps2_getDuDecodesSinceLast() {
    return probe_ae::s_duDecodesSinceLastPresent.exchange(0, std::memory_order_relaxed);
}
extern "C" void ps2_onDuPresent() {
    probe_ae::s_duPresentsThisSec.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void ps2_getDkThread1Info(int *outCurTh, const char **outTh1Status, uint32_t *outTh1Pc, uint32_t *outTh1Ra);


void writeUncompressedPng(uint32_t w, uint32_t h, const uint8_t *rgba, const char *filename);

namespace probe_ae {
    // Task AE
    std::atomic<uint32_t> cnt_F{0};
    std::atomic<uint32_t> cnt_24c24c{0};
    std::atomic<uint32_t> cnt_24c4f8{0};
    std::atomic<uint32_t> cnt_24c620{0};
    std::atomic<uint32_t> cnt_24c64c{0};
    std::atomic<uint32_t> cnt_24c650{0};
    std::atomic<uint32_t> cnt_24c680{0};

    std::atomic<uint32_t> val_state{0};
    std::atomic<uint32_t> val_s0{0};
    std::atomic<uint32_t> val_a1_s4{0};
    std::atomic<uint32_t> val_f72C7A0{0};
    std::atomic<uint32_t> val_v0_1DA880{0};
    std::atomic<uint32_t> val_v0_taken{0};
    std::atomic<uint32_t> val_s4C{0};
    std::atomic<uint32_t> val_s2{0};
    std::atomic<uint32_t> val_s4C_10{0};
    std::atomic<uint32_t> val_s0_70{0};
    std::atomic<uint32_t> val_s48_4{0};
    std::atomic<uint32_t> val_taken_558{0};
    std::atomic<uint32_t> val_f72D3A0{0};
    std::atomic<uint32_t> val_w635480{0};
    std::atomic<uint32_t> val_v0_508684{0};
    std::atomic<uint32_t> val_w72D44C{0};
    std::atomic<uint32_t> val_s1_0C{0};
    std::atomic<uint32_t> val_t7_jalr{0};
    std::atomic<uint32_t> val_s1{0}, val_s2_680{0}, val_s3_680{0};

    static std::atomic<bool> s_hasSeenFState9{false};
    static const uint8_t *s_cachedRdram = nullptr;
    static std::chrono::steady_clock::time_point s_lastRaw8ValidTime{};
    static std::atomic<bool> s_hasSeenValidRaw8{false};

    void setRdram(const uint8_t *rdram) {
        if (rdram) s_cachedRdram = rdram;
    }

    std::atomic<uint32_t> reg_F_a0{0};
    std::atomic<uint32_t> reg_F_a1{0};
    std::atomic<uint32_t> reg_F_a2{0};
    std::atomic<uint32_t> reg_F_s0{0};
    std::atomic<uint32_t> reg_F_s1{0};
    std::atomic<uint32_t> reg_G_a0{0};
    std::atomic<uint32_t> reg_G_a1{0};
    std::atomic<uint32_t> cnt_G_calls{0};

    // Task AR atomics
    std::atomic<uint32_t> cnt_ar_31a3e8{0};
    std::atomic<uint32_t> cnt_ar_35f4c4{0};
    std::atomic<uint32_t> cnt_ar_15b750{0};
    std::atomic<uint32_t> cnt_ar_31a52c{0};
    std::atomic<uint32_t> cnt_ar_4e2730{0};
    std::atomic<uint32_t> cnt_ar_4e914c{0};
    std::atomic<uint32_t> cnt_ar_G{0};
    std::atomic<uint32_t> cnt_ar_cause3_dispatches{0};
    std::atomic<uint32_t> val_ar_ret_31a3e8{0};
    std::atomic<uint32_t> val_ar_ret_35f4c4{0};

    // Task AU
    std::atomic<uint32_t> cnt_au_3d7c08{0};
    std::atomic<uint32_t> cnt_au_36c3b4{0};
    std::atomic<uint32_t> cnt_au_165efc{0};
    std::atomic<uint32_t> cnt_ar_4e914c_total{0};

    // Task AF
    std::atomic<uint32_t> cnt_24ce0c{0};
    std::atomic<uint32_t> val_v24cbfc{0};
    std::atomic<uint32_t> cnt_24ce14_tk{0};
    std::atomic<uint32_t> cnt_24cbfc{0};
    std::atomic<uint32_t> val_cbfc_a0{0};
    std::atomic<uint32_t> val_cbfc_10{0};
    std::atomic<uint32_t> val_cbfc_0c{0};
    std::atomic<uint32_t> val_1da880_1{0};
    std::atomic<uint32_t> val_1da880_2{0};
    std::atomic<uint32_t> cnt_24ce24{0};
    std::atomic<uint32_t> cnt_24cb38{0};
    std::atomic<uint32_t> val_cb38_a0{0};
    std::atomic<uint32_t> val_cb38_8{0};
    std::atomic<uint32_t> val_cb58_7c{0};
    std::atomic<uint32_t> cnt_24cb64{0};
    std::atomic<uint32_t> val_v3f75e8{0};
    std::atomic<uint32_t> cnt_24cbb8{0};
    std::atomic<uint32_t> val_cbb8_t6{0};
    std::atomic<uint32_t> val_s4a4_7c{0};
    std::atomic<uint32_t> val_s4a4_30{0};

    // Task AG
    std::atomic<uint32_t> cnt_24cd04{0};
    std::atomic<uint32_t> val_cd04_a0{0};
    std::atomic<uint32_t> val_cd04_a2{0};
    std::atomic<uint32_t> val_cd04_a3{0};
    std::atomic<uint32_t> val_cd04_ra{0};
    std::atomic<uint32_t> val_cd50_t7{0};
    std::atomic<uint32_t> cnt_cd34_oob{0};

    std::atomic<int32_t> val_3f7a74_v0{0};
    std::atomic<uint32_t> cnt_3f7a74_v0_neg{0};
    std::atomic<uint32_t> val_3f7a74_s1{0};
    std::atomic<uint32_t> val_3f7a74_head{0};
    std::atomic<uint32_t> val_3f7a74_tail{0};

    std::atomic<uint32_t> val_1dc650_slot8{0};
    std::atomic<int32_t> val_1dc578_ret{0};

    // Task AH
    std::atomic<uint32_t> cnt_438ea8{0};
    std::atomic<uint32_t> cnt_1dcee0{0};
    std::atomic<uint32_t> val_1dcf8c_t6{0};
    std::atomic<uint32_t> val_1dcf8c_s1{0};
    std::atomic<uint32_t> cnt_1dcc70{0};
    std::atomic<int32_t>  val_1dcc70_v0{0};
    std::atomic<uint32_t> val_1dc1ec_t7{0};
    std::atomic<uint32_t> val_1dc1ec_s1{0};
    std::atomic<uint32_t> cnt_intc_dispatch{0};
    std::atomic<uint32_t> s_currentSec{0};

    // Task AJ
    std::atomic<uint32_t> cnt_24c4c0{0};
    std::atomic<uint32_t> val_c4e0_v0{0};
    std::atomic<uint32_t> val_c4e0_s0_84{0};
    std::atomic<uint32_t> cnt_c4e0_lt{0};
    std::atomic<uint32_t> cnt_24c4f0{0};
    std::atomic<uint32_t> val_3f7780_a0{0};
    std::atomic<uint32_t> val_3f7780_a1{0};
    std::atomic<uint64_t> val_3f7780_mask{0};
    std::atomic<uint32_t> cnt_3fbe78{0};
    std::atomic<uint32_t> val_3fbe78_voice{0};
    std::atomic<uint32_t> val_3fbeac_v0{0};

    // Task AN
    std::atomic<uint32_t> cnt_24c5a0{0};
    std::atomic<uint32_t> cnt_24c5ac{0};
    std::atomic<uint32_t> cnt_3f7e38{0};
    std::atomic<uint32_t> cnt_31a240{0};
    std::atomic<uint32_t> cnt_31a2c0{0};
    std::atomic<uint32_t> val_s4C_refill{0};

    static std::mutex s_seenIntcMutex;
    static std::vector<uint32_t> s_seenIntcHandlers;

    void logIntcHandlerDispatch(uint32_t addr) {
        if (!isEnabled()) return;
        cnt_intc_dispatch.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(s_seenIntcMutex);
        if (std::find(s_seenIntcHandlers.begin(), s_seenIntcHandlers.end(), addr) == s_seenIntcHandlers.end()) {
            s_seenIntcHandlers.push_back(addr);
            std::printf("[probe] intc_handler addr=0x%08X\n", addr);
            std::fflush(stdout);
        }
    }

    void logIntcRegister(bool dmac, uint32_t cause, uint32_t handler) {
        if (!isEnabled()) return;
        std::printf("[probe] intc_register dmac=%d cause=%u handler=0x%08X\n", dmac ? 1 : 0, cause, handler);
        std::fflush(stdout);
    }

    void logSifSetCmdBuffer(uint32_t buf) {
        if (!isEnabled()) return;
        std::printf("[probe] sif_set_cmd_buffer ptr=0x%08X\n", buf);
        std::fflush(stdout);
    }

    void logSifAddCmdHandler(uint32_t cid, uint32_t handler, uint32_t arg) {
        if (!isEnabled()) return;
        std::printf("[probe] sif_add_cmd_handler cid=%u handler=0x%08X arg=0x%08X\n", cid, handler, arg);
        std::fflush(stdout);
    }

    static std::mutex s_cd04Mutex;
    static std::map<uint32_t, uint32_t> s_cd04CmdCounts;

    void recordCd04Cmd(uint32_t cmd) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_cd04Mutex);
        s_cd04CmdCounts[cmd]++;
    }

    std::string getAndResetCd04Cmds() {
        std::lock_guard<std::mutex> lock(s_cd04Mutex);
        if (s_cd04CmdCounts.empty()) return "none";
        std::string res;
        for (auto const & [cmd, count] : s_cd04CmdCounts) {
            if (!res.empty()) res += ",";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%u:%u", cmd, count);
            res += buf;
        }
        s_cd04CmdCounts.clear();
        return res;
    }

    static std::mutex s_firstCallsMutex;
    static std::vector<std::pair<uint32_t, uint32_t>> s_firstCalls;
    static bool s_firstCallsPrinted = false;

    void recordCd04Call(uint32_t cmd, uint32_t ra) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_firstCallsMutex);
        if (s_firstCallsPrinted) return;
        if (s_firstCalls.size() < 40) {
            s_firstCalls.push_back({cmd, ra});
        }
        if (s_firstCalls.size() == 40) {
            std::string line = "[probe] first_calls ";
            for (size_t i = 0; i < s_firstCalls.size(); ++i) {
                if (i > 0) line += ",";
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%u@%X", s_firstCalls[i].first, s_firstCalls[i].second);
                line += buf;
            }
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
            s_firstCallsPrinted = true;
        }
    }

    void flushFirstCallsIfDue(uint32_t sec) {
        if (!isEnabled()) return;
        if (sec < 30) return;
        std::lock_guard<std::mutex> lock(s_firstCallsMutex);
        if (s_firstCallsPrinted) return;
        std::string line = "[probe] first_calls ";
        if (s_firstCalls.empty()) {
            line += "none";
        } else {
            for (size_t i = 0; i < s_firstCalls.size(); ++i) {
                if (i > 0) line += ",";
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%u@%X", s_firstCalls[i].first, s_firstCalls[i].second);
                line += buf;
            }
        }
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        s_firstCallsPrinted = true;
    }

    static std::mutex s_1dcMutex;
    static std::map<uint32_t, uint32_t> s_1dcA1Counts;

    void record1dc650A1(uint32_t a1) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_1dcMutex);
        s_1dcA1Counts[a1]++;
    }

    std::string getAndReset1dcA1() {
        std::lock_guard<std::mutex> lock(s_1dcMutex);
        if (s_1dcA1Counts.empty()) return "none";
        std::string res;
        for (auto const & [a1, count] : s_1dcA1Counts) {
            if (!res.empty()) res += ",";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%u:%u", a1, count);
            res += buf;
        }
        s_1dcA1Counts.clear();
        return res;
    }

    static std::mutex s_raMutex;
    static std::deque<uint32_t> s_lastRa;

    void recordRa24c64c(uint32_t ra) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_raMutex);
        if (std::find(s_lastRa.begin(), s_lastRa.end(), ra) == s_lastRa.end()) {
            if (s_lastRa.size() >= 4) s_lastRa.pop_front();
            s_lastRa.push_back(ra);
        }
    }

    std::string getAndResetRa24c64c() {
        std::lock_guard<std::mutex> lock(s_raMutex);
        char buf[128] = {0};
        std::string res;
        for (uint32_t ra : s_lastRa) {
            std::snprintf(buf, sizeof(buf), "0x%x ", ra);
            res += buf;
        }
        s_lastRa.clear();
        return res;
    }

    static std::mutex s_exitMutex;
    static std::map<uint32_t, uint32_t> s_exitCounts;

    void recordExit(uint32_t addr) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_exitMutex);
        s_exitCounts[addr]++;
    }

    std::string getAndResetExits() {
        std::lock_guard<std::mutex> lock(s_exitMutex);
        char buf[128] = {0};
        std::string res;
        for (auto const & [addr, count] : s_exitCounts) {
            if (!res.empty()) res += " ";
            std::snprintf(buf, sizeof(buf), "exit@0x%08X=%u", addr, count);
            res += buf;
        }
        s_exitCounts.clear();
        return res;
    }

    static std::mutex s_traceMutex;
    static std::vector<uint32_t> s_ringBuffer(64, 0);
    static size_t s_ringIdx = 0;
    static size_t s_ringCount = 0;

    void recordTraceLabel(uint32_t labelAddr) {
        if (!isEnabled()) return;
        if (labelAddr == 0x24C1DCu) onBoLabel(0x24C1DCu);
        std::lock_guard<std::mutex> lock(s_traceMutex);
        s_ringBuffer[s_ringIdx] = labelAddr;
        s_ringIdx = (s_ringIdx + 1) % 64;
        if (s_ringCount < 64) s_ringCount++;
    }

    void dumpTraceIfDue(uint32_t gCalls) {
        if (gCalls != 30 && gCalls != 300) return;
        std::lock_guard<std::mutex> lock(s_traceMutex);
        std::string out = "[trace] ";
        char buf[32];
        size_t start = (s_ringCount < 64) ? 0 : s_ringIdx;
        for (size_t i = 0; i < s_ringCount; ++i) {
            size_t idx = (start + i) % 64;
            std::snprintf(buf, sizeof(buf), "0x%X ", s_ringBuffer[idx]);
            out += buf;
        }
        std::printf("%s (after G call %u)\n", out.c_str(), gCalls);
        std::fflush(stdout);
    }

    void printOncePerSec(uint32_t sec, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t f_cnt = cnt_F.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c24c = cnt_24c24c.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c4f8 = cnt_24c4f8.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c620 = cnt_24c620.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c64c = cnt_24c64c.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c650 = cnt_24c650.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c680 = cnt_24c680.exchange(0, std::memory_order_relaxed);

        uint32_t c_24ce0c = cnt_24ce0c.exchange(0, std::memory_order_relaxed);
        uint32_t c_24ce14_tk = cnt_24ce14_tk.exchange(0, std::memory_order_relaxed);
        uint32_t c_24cbfc = cnt_24cbfc.exchange(0, std::memory_order_relaxed);
        uint32_t c_24ce24 = cnt_24ce24.exchange(0, std::memory_order_relaxed);
        uint32_t c_24cb38 = cnt_24cb38.exchange(0, std::memory_order_relaxed);
        uint32_t c_24cb64 = cnt_24cb64.exchange(0, std::memory_order_relaxed);
        uint32_t c_24cbb8 = cnt_24cbb8.exchange(0, std::memory_order_relaxed);

        uint32_t st_word = 0;
        if (rdram) {
            s_cachedRdram = rdram;
            std::memcpy(&st_word, rdram + (0x772188u & PS2_RAM_MASK), sizeof(st_word));
            if (st_word == 9) {
                s_hasSeenFState9.store(true, std::memory_order_relaxed);
            }
        }

        std::string raList = getAndResetRa24c64c();
        std::string exitList = getAndResetExits();

        s_currentSec.store(sec, std::memory_order_relaxed);

        uint32_t c_24c4c0 = cnt_24c4c0.exchange(0, std::memory_order_relaxed);
        uint32_t c_lt_c4e0 = cnt_c4e0_lt.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c4f0 = cnt_24c4f0.exchange(0, std::memory_order_relaxed);
        uint32_t c_3fbe78 = cnt_3fbe78.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c5a0 = cnt_24c5a0.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c5ac = cnt_24c5ac.exchange(0, std::memory_order_relaxed);
        uint32_t c_3f7e38 = cnt_3f7e38.exchange(0, std::memory_order_relaxed);
        uint32_t c_31a240 = cnt_31a240.exchange(0, std::memory_order_relaxed);
        uint32_t c_31a2c0 = cnt_31a2c0.exchange(0, std::memory_order_relaxed);
        uint32_t s4c_refill = val_s4C_refill.load(std::memory_order_relaxed);
        uint32_t c8_cdcc = cnt_c8_24cdcc.exchange(0, std::memory_order_relaxed);
        uint32_t c8_cdfc = cnt_c8_24cdfc.exchange(0, std::memory_order_relaxed);
        uint32_t c8_24c7dc = cnt_c8_24c7dc.exchange(0, std::memory_order_relaxed);
        uint32_t c8_24c8b4 = cnt_c8_24c8b4.exchange(0, std::memory_order_relaxed);
        uint32_t c8_ret = cnt_c8_cmd8_ret.exchange(0, std::memory_order_relaxed);

        uint32_t ctx_88 = 0, ctx_C8 = 0, ctx_CC = 0, ctx_D8 = 0;
        uint32_t ctx_E0 = 0, ctx_E4 = 0, ctx_E8 = 0, ctx_EC = 0;
        if (rdram) {
            std::memcpy(&ctx_88, rdram + (0x772188u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&ctx_C8, rdram + (0x7721C8u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&ctx_CC, rdram + (0x7721CCu & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&ctx_D8, rdram + (0x7721D8u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&ctx_E0, rdram + (0x7721E0u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&ctx_E4, rdram + (0x7721E4u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&ctx_E8, rdram + (0x7721E8u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&ctx_EC, rdram + (0x7721ECu & PS2_RAM_MASK), sizeof(uint32_t));
        }
        uint32_t w74b140 = 0, w74b144 = 0, w74b148 = 0;
        uint64_t m1900ea0 = 0;
        uint32_t s0_84_mem = 0, s0_7c_mem = 0;
        if (rdram) {
            std::memcpy(&w74b140, rdram + (0x74B140u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&w74b144, rdram + (0x74B144u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&w74b148, rdram + (0x74B148u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&m1900ea0, rdram + (0x1900EA0u & PS2_RAM_MASK), sizeof(uint64_t));
            std::memcpy(&s0_84_mem, rdram + (0x772204u & PS2_RAM_MASK), sizeof(uint32_t));
            s0_7c_mem = rdram[0x7721FCu & PS2_RAM_MASK];
        }
        uint32_t c_438ea8 = cnt_438ea8.exchange(0, std::memory_order_relaxed);
        uint32_t c_1dcee0 = cnt_1dcee0.exchange(0, std::memory_order_relaxed);
        uint32_t c_1dcc70 = cnt_1dcc70.exchange(0, std::memory_order_relaxed);
        uint32_t c_intc = cnt_intc_dispatch.exchange(0, std::memory_order_relaxed);

        uint32_t w771bd4 = 0, w76a518 = 0, w77218c = 0, w771bc4 = 0;
        uint32_t pkt[4] = {0, 0, 0, 0};
        if (rdram) {
            std::memcpy(&w771bd4, rdram + (0x771BD4u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&w76a518, rdram + (0x76A518u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&w77218c, rdram + (0x77218Cu & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&w771bc4, rdram + (0x771BC4u & PS2_RAM_MASK), sizeof(uint32_t));
            uint32_t pktAddr = w76a518 & PS2_RAM_MASK;
            if (pktAddr <= PS2_RAM_SIZE - 16) {
                std::memcpy(pkt, rdram + pktAddr, sizeof(pkt));
            }
        }

        std::string cd04Cmds = getAndResetCd04Cmds();
        std::string a1_1dc = getAndReset1dcA1();
        uint32_t c_24cd04 = cnt_24cd04.exchange(0, std::memory_order_relaxed);
        uint32_t c_oob = cnt_cd34_oob.exchange(0, std::memory_order_relaxed);
        uint32_t neg_v0 = cnt_3f7a74_v0_neg.exchange(0, std::memory_order_relaxed);

        flushFirstCallsIfDue(sec);

        // Print unified [probe] line with old fields + Task AF + Task AG + Task AH fields
        std::printf("[probe] t=%u F=%u state=%u st=%u s0=%X a1=%X v0_1DA880=%u(tk=%u) f72C7A0=%u f72D3A0=%u w635480=%u s4C=%X s2=%X s4C10=%X s0_70=%X s484=%X tk558=%u v0_508684=%X w72D44C=%X s1_0C=%X t7=%X c24C24C=%u c24C4F8=%u c24C620=%u c24C64C=%u c24C650=%u c24C680=%u c24CE0C=%u v24CBFC=%u(tk=%u) c24CBFC=%u a0_cbfc=%X cbfc10=%X cbfc0C=%X a1DA880=%u,%u c24CE24=%u c24CB38=%u a0_cb38=%X cb38_8=%u b7C=%u c24CB64=%u v3F75E8=%u c24CBB8=%u t6_cbb8=%u s6_7C=%u s6_30=%X ra=[%s] exits=%s c24CD04=%u cmds=%s last_a0=%X last_a2=%X last_a3=%X last_ra=%X t7_cd50=%X oob_cmd=%u v0_3f7=%d neg_v0=%u s1_3f7=%X head_3f7=%u tail_3f7=%u slot8_1dc=%X ret_1dc578=%d a1_1dc=%s c438EA8=%u c1DCEE0=%u t6_1dcf8c=%X s1_1dcf8c=%X w771bd4=%X w76a518=%X pkt=%X,%X,%X,%X w77218c=%X w771bc4=%X c1DCC70=%u v0_1dcc=%d t7_1dc1ec=%X s1_1dc1ec=%X c_intc=%u c24C4C0=%u v0_c4e0=%u s0_84=%X lt_c4e0=%u c24C4F0=%u a0_3f7780=%X a1_3f7780=%X mask_3f7780=%llX c3FBE78=%u v_3fbe78=%u v0_3fbe=%X w74B140=%X,%X,%X m1900EA0=%llX s0_84_mem=%X s0_7C_mem=%X c24C5A0=%u c24C5AC=%u c3F7E38=%u c31A240=%u c31A2C0=%u s4C_refill=%X c8_cdcc=%u c8_cdfc=%u c8_7dc=%u c8_8b4=%u c8_ret=%u ctx_88=%X ctx_C8=%X ctx_CC=%X ctx_D8=%X ctx_E0=%X ctx_E4=%X ctx_E8=%X ctx_EC=%X\n",
                    sec, f_cnt, val_state.load(), st_word, val_s0.load(), val_a1_s4.load(),
                    val_v0_1DA880.load(), val_v0_taken.load(),
                    val_f72C7A0.load(), val_f72D3A0.load(), val_w635480.load(),
                    val_s4C.load(), val_s2.load(), val_s4C_10.load(), val_s0_70.load(), val_s48_4.load(), val_taken_558.load(),
                    val_v0_508684.load(), val_w72D44C.load(), val_s1_0C.load(), val_t7_jalr.load(),
                    c_24c24c, c_24c4f8, c_24c620, c_24c64c, c_24c650, c_24c680,
                    c_24ce0c, val_v24cbfc.load(), c_24ce14_tk, c_24cbfc, val_cbfc_a0.load(), val_cbfc_10.load(), val_cbfc_0c.load(),
                    val_1da880_1.load(), val_1da880_2.load(), c_24ce24, c_24cb38, val_cb38_a0.load(), val_cb38_8.load(), val_cb58_7c.load(),
                    c_24cb64, val_v3f75e8.load(), c_24cbb8, val_cbb8_t6.load(), val_s4a4_7c.load(), val_s4a4_30.load(),
                    raList.c_str(), exitList.empty() ? "none" : exitList.c_str(),
                    c_24cd04, cd04Cmds.c_str(), val_cd04_a0.load(), val_cd04_a2.load(), val_cd04_a3.load(), val_cd04_ra.load(),
                    val_cd50_t7.load(), c_oob, val_3f7a74_v0.load(), neg_v0, val_3f7a74_s1.load(), val_3f7a74_head.load(), val_3f7a74_tail.load(),
                    val_1dc650_slot8.load(), val_1dc578_ret.load(), a1_1dc.c_str(),
                    c_438ea8, c_1dcee0, val_1dcf8c_t6.load(), val_1dcf8c_s1.load(),
                    w771bd4, w76a518, pkt[0], pkt[1], pkt[2], pkt[3], w77218c, w771bc4,
                    c_1dcc70, val_1dcc70_v0.load(), val_1dc1ec_t7.load(), val_1dc1ec_s1.load(), c_intc,
                    c_24c4c0, val_c4e0_v0.load(), val_c4e0_s0_84.load(), c_lt_c4e0, c_24c4f0,
                    val_3f7780_a0.load(), val_3f7780_a1.load(), (unsigned long long)val_3f7780_mask.load(),
                    c_3fbe78, val_3fbe78_voice.load(), val_3fbeac_v0.load(),
                    w74b140, w74b144, w74b148, (unsigned long long)m1900ea0, s0_84_mem, s0_7c_mem,
                    c_24c5a0, c_24c5ac, c_3f7e38, c_31a240, c_31a2c0, s4c_refill, c8_cdcc, c8_cdfc, c8_24c7dc, c8_24c8b4, c8_ret, ctx_88, ctx_C8, ctx_CC, ctx_D8, ctx_E0, ctx_E4, ctx_E8, ctx_EC);

        uint32_t ar_31a3e8 = cnt_ar_31a3e8.exchange(0, std::memory_order_relaxed);
        uint32_t ar_35f4c4 = cnt_ar_35f4c4.exchange(0, std::memory_order_relaxed);
        uint32_t ar_15b750 = cnt_ar_15b750.exchange(0, std::memory_order_relaxed);
        uint32_t ar_31a52c = cnt_ar_31a52c.exchange(0, std::memory_order_relaxed);
        uint32_t ar_4e2730 = cnt_ar_4e2730.exchange(0, std::memory_order_relaxed);
        uint32_t ar_4e914c = cnt_ar_4e914c.exchange(0, std::memory_order_relaxed);
        uint32_t ar_G = cnt_ar_G.exchange(0, std::memory_order_relaxed);
        uint32_t ar_c3 = cnt_ar_cause3_dispatches.exchange(0, std::memory_order_relaxed);
        uint32_t ar_ret_31a = val_ar_ret_31a3e8.load(std::memory_order_relaxed);
        uint32_t ar_ret_35f = val_ar_ret_35f4c4.load(std::memory_order_relaxed);

        uint64_t w696fd0 = 0, w696fd8 = 0, w686fd0 = 0, w686fd8 = 0;
        if (rdram) {
            std::memcpy(&w696fd0, rdram + (0x696FD0u & PS2_RAM_MASK), sizeof(uint64_t));
            std::memcpy(&w696fd8, rdram + (0x696FD8u & PS2_RAM_MASK), sizeof(uint64_t));
            std::memcpy(&w686fd0, rdram + (0x686FD0u & PS2_RAM_MASK), sizeof(uint64_t));
            std::memcpy(&w686fd8, rdram + (0x686FD8u & PS2_RAM_MASK), sizeof(uint64_t));
        }

        std::printf("[probe] ar sec=%u 69fd0=0x%llX 69fd8=0x%llX (68fd0=0x%llX 68fd8=0x%llX) cause3_disp=%u 31a3e8=%u(ret=%u) 35f4c4=%u(ret=%u) 15b750=%u 31a52c=%u 4e2730=%u G_4e2df8=%u 4e914c=%u\n",
                    sec,
                    (unsigned long long)w696fd0, (unsigned long long)w696fd8,
                    (unsigned long long)w686fd0, (unsigned long long)w686fd8,
                    ar_c3, ar_31a3e8, ar_ret_31a, ar_35f4c4, ar_ret_35f,
                    ar_15b750, ar_31a52c, ar_4e2730, ar_G, ar_4e914c);
        std::fflush(stdout);

        uint32_t au_3d7c08 = cnt_au_3d7c08.exchange(0, std::memory_order_relaxed);
        uint32_t au_36c3b4 = cnt_au_36c3b4.exchange(0, std::memory_order_relaxed);
        uint32_t au_165efc = cnt_au_165efc.exchange(0, std::memory_order_relaxed);
        std::printf("[probe] au cnt 3d7c08=%u 36c3b4=%u 165efc=%u\n", au_3d7c08, au_36c3b4, au_165efc);
        std::fflush(stdout);

        static uint32_t s_stuckStreak = 0;
        static uint32_t s_stuckReportCount = 0;

        if (ar_4e914c == 0) {
            s_stuckStreak++;
        } else {
            s_stuckStreak = 0;
        }

        if (s_stuckStreak >= 2) {
            if (s_stuckReportCount < 3) {
                s_stuckReportCount++;
                dumpTaskASInfo(sec, s_stuckReportCount == 1);
            }
        }

        if (s_stuckStreak == 3 || s_stuckStreak == 6) {
            dumpTaskATInfo(sec, rdram);
        }

        dumpTaskAVInfo(sec, rdram);
        dumpAyTimeIfDue(sec);
        dumpAzRingIfDue(sec, rdram);
        dumpBaVu1IfDue(sec);
    }

    void log24C5A0(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        cnt_24c5a0.fetch_add(1, std::memory_order_relaxed);
        uint32_t t5 = getRegU32(ctx, 13);
        uint32_t t7 = getRegU32(ctx, 15);
        uint32_t s1 = getRegU32(ctx, 17);
        uint32_t s3 = getRegU32(ctx, 19);
        uint32_t s4 = getRegU32(ctx, 20);
        uint32_t s0 = getRegU32(ctx, 16);

        auto readWord = [rdram](uint32_t addr) -> uint32_t {
            uint32_t w = 0;
            if (rdram && (addr & PS2_RAM_MASK) <= PS2_RAM_SIZE - 4) {
                std::memcpy(&w, rdram + (addr & PS2_RAM_MASK), sizeof(w));
            }
            return w;
        };

        uint32_t s48 = readWord(s0 + 0x48);
        uint32_t s4C = readWord(s0 + 0x4C);
        uint32_t s58 = readWord(s0 + 0x58);
        uint32_t s5C = readWord(s0 + 0x5C);
        uint32_t s60 = readWord(s0 + 0x60);
        uint32_t s64 = readWord(s0 + 0x64);
        uint32_t s68 = readWord(s0 + 0x68);
        uint32_t s6C = readWord(s0 + 0x6C);
        uint32_t s70 = readWord(s0 + 0x70);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            std::printf("[probe] f9 pc=0x24C5A0 n=%u t5=%u t7=%u s1=0x%X s3=0x%X s4=0x%X s48=0x%X s4C=0x%X s58=0x%X s5C=%u s60=%u s64=%u s68=0x%X s6C=0x%X s70=%u\n",
                        n, t5, t7, s1, s3, s4, s48, s4C, s58, s5C, s60, s64, s68, s6C, s70);
            std::fflush(stdout);
        }
    }

    void log24C5AC(R5900Context *ctx, const uint8_t *rdram, uint32_t pc) {
        if (!isEnabled()) return;
        cnt_24c5ac.fetch_add(1, std::memory_order_relaxed);
        uint32_t t5 = getRegU32(ctx, 13);
        uint32_t t7 = getRegU32(ctx, 15);
        uint32_t s1 = getRegU32(ctx, 17);
        uint32_t s3 = getRegU32(ctx, 19);
        uint32_t s4 = getRegU32(ctx, 20);
        uint32_t s0 = getRegU32(ctx, 16);

        auto readWord = [rdram](uint32_t addr) -> uint32_t {
            uint32_t w = 0;
            if (rdram && (addr & PS2_RAM_MASK) <= PS2_RAM_SIZE - 4) {
                std::memcpy(&w, rdram + (addr & PS2_RAM_MASK), sizeof(w));
            }
            return w;
        };

        uint32_t s48 = readWord(s0 + 0x48);
        uint32_t s4C = readWord(s0 + 0x4C);
        uint32_t s58 = readWord(s0 + 0x58);
        uint32_t s5C = readWord(s0 + 0x5C);
        uint32_t s60 = readWord(s0 + 0x60);
        uint32_t s64 = readWord(s0 + 0x64);
        uint32_t s68 = readWord(s0 + 0x68);
        uint32_t s6C = readWord(s0 + 0x6C);
        uint32_t s70 = readWord(s0 + 0x70);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            std::printf("[probe] f10 pc=0x%X n=%u t5=%u t7=%u s1=0x%X s3=0x%X s4=0x%X s48=0x%X s4C=0x%X s58=0x%X s5C=%u s60=%u s64=%u s68=0x%X s6C=0x%X s70=%u\n",
                        pc, n, t5, t7, s1, s3, s4, s48, s4C, s58, s5C, s60, s64, s68, s6C, s70);
            std::fflush(stdout);
        }
    }

    void log24C5E8(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        cnt_3f7e38.fetch_add(1, std::memory_order_relaxed);
        uint32_t s0 = getRegU32(ctx, 16);
        uint32_t s4C = 0;
        if (rdram && ((s0 + 0x4C) & PS2_RAM_MASK) <= PS2_RAM_SIZE - 4) {
            std::memcpy(&s4C, rdram + ((s0 + 0x4C) & PS2_RAM_MASK), sizeof(s4C));
        }
        uint32_t t7 = getRegU32(ctx, 15);
        val_s4C_refill.store(s4C, std::memory_order_relaxed);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            std::printf("[probe] f9 pc=0x24C5E8 n=%u s0_4C=0x%X t7=0x%X\n", n, s4C, t7);
            std::fflush(stdout);
        }
    }

    void log24C24C(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t s0 = getRegU32(ctx, 16);
        uint32_t hdr_ptr = 0;
        if (rdram && ((s0 + 0x1C) & PS2_RAM_MASK) <= PS2_RAM_SIZE - 4) {
            std::memcpy(&hdr_ptr, rdram + ((s0 + 0x1C) & PS2_RAM_MASK), sizeof(hdr_ptr));
        }

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            uint32_t hdr[16] = {0};
            if (rdram && (hdr_ptr & PS2_RAM_MASK) <= PS2_RAM_SIZE - 0x40) {
                std::memcpy(hdr, rdram + (hdr_ptr & PS2_RAM_MASK), sizeof(hdr));
            }
            char magicStr[5] = {0};
            std::memcpy(magicStr, &hdr[0], 4);
            std::printf("[probe] f9 pc=0x24C24C n=%u hdr_ptr=0x%08X magic=0x%08X('%s') frames=%u off18=0x%X off30=0x%X\n",
                        n, hdr_ptr, hdr[0], magicStr, hdr[2], hdr[6], hdr[12]);
            std::printf("[probe] f9 hdr hex: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
                        hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7],
                        hdr[8], hdr[9], hdr[10], hdr[11], hdr[12], hdr[13], hdr[14], hdr[15]);
            std::fflush(stdout);
        }
    }

    void log31A240Entry(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        cnt_31a240.fetch_add(1, std::memory_order_relaxed);
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);

        auto readWord = [rdram](uint32_t addr) -> uint32_t {
            uint32_t w = 0;
            if (rdram && (addr & PS2_RAM_MASK) <= PS2_RAM_SIZE - 4) {
                std::memcpy(&w, rdram + (addr & PS2_RAM_MASK), sizeof(w));
            }
            return w;
        };

        uint32_t s1_1F0 = readWord(a0 + 0x1F0);
        uint32_t s1_1F4 = readWord(a0 + 0x1F4);
        uint32_t s1_1F8 = readWord(a0 + 0x1F8);
        uint32_t s1_1FC = readWord(a0 + 0x1FC);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            std::printf("[probe] 31a240 entry n=%u a1=%u s1=0x%X s1_1F0=%u s1_1F4=%u s1_1F8=%u s1_1FC=%u\n",
                        n, a1, a0, s1_1F0, s1_1F4, s1_1F8, s1_1FC);
            std::fflush(stdout);
        }
    }

    void log31A2C0Ret(R5900Context *ctx) {
        if (!isEnabled()) return;
        cnt_31a2c0.fetch_add(1, std::memory_order_relaxed);
        int32_t v0 = (int32_t)getRegU32(ctx, 2);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            std::printf("[probe] 31a2c0 ret n=%u v0=%d\n", n, v0);
            std::fflush(stdout);
        }
    }

    std::atomic<uint32_t> val_last_cmd{0};
    std::atomic<uint32_t> cnt_c8_24cdcc{0};
    std::atomic<uint32_t> cnt_c8_24cdfc{0};
    std::atomic<uint32_t> cnt_c8_24c7dc{0};
    std::atomic<uint32_t> cnt_c8_24c8b4{0};
    std::atomic<uint32_t> cnt_c8_cmd8_ret{0};

    void log24CD38(R5900Context *ctx) {
        if (!isEnabled()) return;
        uint32_t a0 = val_cd04_a0.load(std::memory_order_relaxed);
        uint32_t s1 = getRegU32(ctx, 17);
        uint32_t cmd = getRegU32(ctx, 16) + 1;
        val_last_cmd.store(cmd, std::memory_order_relaxed);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24cd38 n=%u a0=0x%X s1=0x%X cmd=%u\n", n, a0, s1, cmd);
            std::fflush(stdout);
        }
    }

    void log24CDBC(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t s1 = getRegU32(ctx, 17);
        auto readWord = [rdram](uint32_t addr) -> uint32_t {
            uint32_t w = 0;
            if (rdram && (addr & PS2_RAM_MASK) <= PS2_RAM_SIZE - 4) {
                std::memcpy(&w, rdram + (addr & PS2_RAM_MASK), sizeof(w));
            }
            return w;
        };

        uint32_t state = readWord(s1 + 8);
        int32_t s64 = (int32_t)readWord(s1 + 0x64);
        uint32_t s6C = readWord(s1 + 0x6C);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24cdbc n=%u s1=0x%X state=%u s64=%d s6C=0x%X\n", n, s1, state, s64, s6C);
            std::fflush(stdout);
        }
    }

    void log24CDCC() {
        if (!isEnabled()) return;
        cnt_c8_24cdcc.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24cdcc n=%u (state==8 abort)\n", n);
            std::fflush(stdout);
        }
    }

    void log24CDFC() {
        if (!isEnabled()) return;
        cnt_c8_24cdfc.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24cdfc n=%u (jal 24C7DC)\n", n);
            std::fflush(stdout);
        }
    }

    void log24CD80(R5900Context *ctx) {
        if (!isEnabled()) return;
        uint32_t cmd = val_last_cmd.load(std::memory_order_relaxed);
        int32_t t5 = (int32_t)getRegU32(ctx, 13);
        if (cmd == 8) {
            cnt_c8_cmd8_ret.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<uint32_t> s_count{0};
            uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 12) {
                std::printf("[probe] c8 24cd80 ret cmd=8 n=%u t5=%d\n", n, t5);
                std::fflush(stdout);
            }
        }
    }

    void log24C7DCEntry(R5900Context *ctx) {
        if (!isEnabled()) return;
        cnt_c8_24c7dc.fetch_add(1, std::memory_order_relaxed);
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24c7dc entry n=%u a0=0x%X a1=0x%X\n", n, a0, a1);
            std::fflush(stdout);
        }
    }

    void log24C828(R5900Context *ctx) {
        if (!isEnabled()) return;
        int32_t v0 = (int32_t)getRegU32(ctx, 2);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24c828 n=%u 24CBFC_v0=%d\n", n, v0);
            std::fflush(stdout);
        }
    }

    void log24C8B4() {
        if (!isEnabled()) return;
        cnt_c8_24c8b4.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24c8b4 entry n=%u\n", n);
            std::fflush(stdout);
        }
    }

    void log24C900(R5900Context *ctx) {
        if (!isEnabled()) return;
        uint32_t t7 = getRegU32(ctx, 15);
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24c900 n=%u t7=0x%08X (ctx+0x48)\n", n, t7);
            std::fflush(stdout);
        }
    }

    void log24C914(R5900Context *ctx) {
        if (!isEnabled()) return;
        uint32_t t5 = getRegU32(ctx, 13);
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12) {
            std::printf("[probe] c8 24c914 n=%u t5=0x%08X (ctx+0x58)\n", n, t5);
            std::fflush(stdout);
        }
    }

    void log24C8BC(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4) {
            int32_t t6 = (int32_t)getRegU32(ctx, 14);
            int32_t t7 = (int32_t)getRegU32(ctx, 15);
            uint32_t s0 = getRegU32(ctx, 16);
            uint32_t s0_74 = 0;
            if (rdram && (s0 + 0x74u) < PS2_RAM_SIZE - 4) {
                std::memcpy(&s0_74, rdram + ((s0 + 0x74u) & PS2_RAM_MASK), sizeof(s0_74));
            }
            std::printf("[probe] 24c8bc n=%u t6=%d t7=%d s0_74=%u slt=%d\n",
                        n, t6, t7, s0_74, (t6 < t7) ? 1 : 0);
            std::fflush(stdout);
        }
    }

    void log24C8E4(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4) {
            uint32_t t4 = getRegU32(ctx, 12);
            uint32_t s0 = getRegU32(ctx, 16);
            uint32_t s0_50 = 0;
            if (rdram && (s0 + 0x50u) < PS2_RAM_SIZE - 4) {
                std::memcpy(&s0_50, rdram + ((s0 + 0x50u) & PS2_RAM_MASK), sizeof(s0_50));
            }
            char hexBytes[33] = {0};
            if (rdram && t4 < PS2_RAM_SIZE - 16) {
                const uint8_t *p = rdram + (t4 & PS2_RAM_MASK);
                for (int i = 0; i < 16; ++i) {
                    std::snprintf(hexBytes + i * 2, 3, "%02x", p[i]);
                }
            } else {
                std::snprintf(hexBytes, sizeof(hexBytes), "OOB");
            }
            std::printf("[probe] 24c8e4 n=%u t4=0x%08X s0_50=%u bytes=%s\n",
                        n, t4, s0_50, hexBytes);
            std::fflush(stdout);
        }
    }

    void log24C8F4(R5900Context *ctx) {
        if (!isEnabled()) return;
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4) {
            uint32_t t7 = getRegU32(ctx, 15);
            std::printf("[probe] 24c8f4 n=%u t7_cmp=%u (1=equal)\n", n, t7);
            std::fflush(stdout);
        }
    }

    // Task AR
    void on31A3E8() {
        if (!isEnabled()) return;
        cnt_ar_31a3e8.fetch_add(1, std::memory_order_relaxed);
    }
    void on35F4C4() {
        if (!isEnabled()) return;
        cnt_ar_35f4c4.fetch_add(1, std::memory_order_relaxed);
    }
    void on15B750() {
        if (!isEnabled()) return;
        cnt_ar_15b750.fetch_add(1, std::memory_order_relaxed);
    }
    void on31A52C() {
        if (!isEnabled()) return;
        cnt_ar_31a52c.fetch_add(1, std::memory_order_relaxed);
    }
    void on4E2730() {
        if (!isEnabled()) return;
        cnt_ar_4e2730.fetch_add(1, std::memory_order_relaxed);
    }
    void on4E914C() {
        if (!isEnabled()) return;
        cnt_ar_4e914c.fetch_add(1, std::memory_order_relaxed);
        cnt_ar_4e914c_total.fetch_add(1, std::memory_order_relaxed);
    }

    void logCause3Dispatch(const std::vector<uint32_t> &handlers) {
        if (!isEnabled()) return;
        cnt_ar_cause3_dispatches.fetch_add(1, std::memory_order_relaxed);
        static std::mutex s_cause3Mutex;
        static std::vector<uint32_t> s_lastCause3Handlers;
        static bool s_cause3First = true;
        std::lock_guard<std::mutex> lock(s_cause3Mutex);
        if (s_cause3First || s_lastCause3Handlers != handlers) {
            s_cause3First = false;
            s_lastCause3Handlers = handlers;
            std::string hList;
            for (size_t i = 0; i < handlers.size(); ++i) {
                char b[32];
                std::snprintf(b, sizeof(b), "%s0x%08X", (i > 0 ? "," : ""), handlers[i]);
                hList += b;
            }
            std::printf("[probe] ar intc cause=3 handlers=[%s]\n", hList.c_str());
            std::fflush(stdout);
        }
    }

    void logIntcHandlerReturn(uint32_t addr, uint32_t ret) {
        if (!isEnabled()) return;
        if (addr == 0x31A3E8u) {
            val_ar_ret_31a3e8.store(ret, std::memory_order_relaxed);
        } else if (addr == 0x35F4C4u) {
            val_ar_ret_35f4c4.store(ret, std::memory_order_relaxed);
        }
    }

    void logRmIntc(const char *op, uint32_t cause, uint32_t ra) {
        if (!isEnabled()) return;
        std::printf("[probe] ar rmintc op=%s cause=%u ra=0x%08X\n", op, cause, ra);
        std::fflush(stdout);
    }

    // Task AS Probes
    struct SemaProbeInfo {
        uint32_t lastSignalRa = 0;
        uint32_t lastWaitRa = 0;
    };
    static std::mutex s_semaMutex;
    static std::unordered_map<int, SemaProbeInfo> s_semaInfo;

    void recordSemaSignal(int semaId, uint32_t ra) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_semaMutex);
        s_semaInfo[semaId].lastSignalRa = ra;
    }

    void recordSemaWait(int semaId, uint32_t ra) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_semaMutex);
        s_semaInfo[semaId].lastWaitRa = ra;
    }

    void recordSemaPoll(int semaId, uint32_t ra) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_semaMutex);
        s_semaInfo[semaId].lastWaitRa = ra;
    }

    void getSemaLastRas(int semaId, uint32_t *lastSignalRa, uint32_t *lastWaitRa) {
        std::lock_guard<std::mutex> lock(s_semaMutex);
        auto it = s_semaInfo.find(semaId);
        if (it != s_semaInfo.end()) {
            if (lastSignalRa) *lastSignalRa = it->second.lastSignalRa;
            if (lastWaitRa) *lastWaitRa = it->second.lastWaitRa;
        } else {
            if (lastSignalRa) *lastSignalRa = 0;
            if (lastWaitRa) *lastWaitRa = 0;
        }
    }

    static std::mutex s_ringMutex;
    static uint32_t s_mainRing[16] = {0};
    static uint32_t s_mainRingIdx = 0;

    static std::mutex s_brRingMutex;
    static uint32_t s_brMainRing[24] = {0};
    static uint32_t s_brMainRingIdx = 0;

    void recordMainThreadDispatch(uint32_t pc) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_ringMutex);
        s_mainRing[s_mainRingIdx++ % 16] = pc;
        {
            std::lock_guard<std::mutex> brLock(s_brRingMutex);
            s_brMainRing[s_brMainRingIdx++ % 24] = pc;
        }
    }

    std::string getRecentMainDispatches() {
        std::lock_guard<std::mutex> lock(s_ringMutex);
        std::string s;
        uint32_t count = std::min(s_mainRingIdx, 12u);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = (s_mainRingIdx >= 12) ? (s_mainRingIdx - 12 + i) : i;
            uint32_t pc = s_mainRing[idx % 16];
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s0x%08X", (i > 0 ? " " : ""), pc);
            s += buf;
        }
        return s;
    }

    // Task AU
    static std::string getAsciiStrCap80(uint32_t addr, const uint8_t *rdram) {
        if (!rdram || addr < 0x00100000u || addr >= 0x02000000u) return "<not-ram>";
        std::string s;
        for (uint32_t i = 0; i < 80; ++i) {
            uint32_t curr = addr + i;
            if ((curr & PS2_RAM_MASK) >= PS2_RAM_SIZE) break;
            uint8_t c = rdram[curr & PS2_RAM_MASK];
            if (c == 0) break;
            if (c >= 32 && c <= 126) {
                s += static_cast<char>(c);
            } else {
                s += '.';
            }
        }
        return s.empty() ? "<empty>" : s;
    }

    static std::atomic<uint32_t> s_count3D7C08_ever{0};
    static std::atomic<uint32_t> s_count3D7C08_st9{0};

    void on3D7C08(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        if (rdram) s_cachedRdram = rdram;
        cnt_au_3d7c08.fetch_add(1, std::memory_order_relaxed);

        uint32_t f_state = 0;
        if (rdram && (0x772188u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
            std::memcpy(&f_state, rdram + (0x772188u & PS2_RAM_MASK), 4);
        }
        if (f_state == 9) {
            s_hasSeenFState9.store(true, std::memory_order_relaxed);
        }

        uint32_t n_ever = s_count3D7C08_ever.fetch_add(1, std::memory_order_relaxed) + 1;
        bool is_st9 = s_hasSeenFState9.load(std::memory_order_relaxed);
        uint32_t n_st9 = is_st9 ? (s_count3D7C08_st9.fetch_add(1, std::memory_order_relaxed) + 1) : 0;

        if (n_ever <= 3 || (is_st9 && n_st9 <= 3)) {
            uint32_t a0 = getRegU32(ctx, 4);
            uint32_t a1 = getRegU32(ctx, 5);
            uint32_t a2 = getRegU32(ctx, 6);
            uint32_t a3 = getRegU32(ctx, 7);
            uint32_t ra = getRegU32(ctx, 31);
            uint32_t sp = getRegU32(ctx, 29);
            uint32_t s0 = getRegU32(ctx, 16);
            uint32_t s1 = getRegU32(ctx, 17);
            uint32_t s2 = getRegU32(ctx, 18);
            uint32_t s3 = getRegU32(ctx, 19);
            uint32_t cnt4E = cnt_ar_4e914c_total.load(std::memory_order_relaxed);

            std::printf("[probe] au 3d7c08 n_ever=%u n_st9=%u f_state=%u cnt4E=%u ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X\n",
                        n_ever, n_st9, f_state, cnt4E, ra, sp, a0, a1, a2, a3, s0, s1, s2, s3);
            if (rdram) {
                std::printf("[probe] au 3d7c08 str a0='%s' a1='%s' a2='%s' a3='%s'\n",
                            getAsciiStrCap80(a0, rdram).c_str(), getAsciiStrCap80(a1, rdram).c_str(),
                            getAsciiStrCap80(a2, rdram).c_str(), getAsciiStrCap80(a3, rdram).c_str());

                // Stack walk of 0x1800 bytes upward (cap 60 lines)
                uint32_t retLines = 0;
                for (uint32_t off = 0; off <= 0x1800; off += 4) {
                    if (retLines >= 60) break;
                    uint32_t currAddr = sp + off;
                    if ((currAddr & PS2_RAM_MASK) + 4 > PS2_RAM_SIZE) continue;
                    uint32_t w = 0;
                    std::memcpy(&w, rdram + (currAddr & PS2_RAM_MASK), 4);

                    if (w >= 0x00100000u && w < 0x00600000u) {
                        uint32_t insnAddr = w - 8;
                        if (insnAddr >= 0x00100000u && (insnAddr & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                            uint32_t insn = 0;
                            std::memcpy(&insn, rdram + (insnAddr & PS2_RAM_MASK), 4);

                            if ((insn & 0xFC000000u) == 0x0C000000u) {
                                uint32_t callee = ((w - 4) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
                                std::printf("[probe] au ret w=0x%08X at sp+0x%X callee=0x%08X\n", w, off, callee);
                                retLines++;
                            } else if ((insn & 0xFC00003Fu) == 0x00000009u) {
                                std::printf("[probe] au ret w=0x%08X at sp+0x%X callee=jalr\n", w, off);
                                retLines++;
                            }
                        }
                    }
                }
            }
            std::fflush(stdout);
        }
    }

    void on36C3B4(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        cnt_au_36c3b4.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            uint32_t a0 = getRegU32(ctx, 4);
            uint32_t a1 = getRegU32(ctx, 5);
            uint32_t a2 = getRegU32(ctx, 6);
            uint32_t a3 = getRegU32(ctx, 7);
            uint32_t ra = getRegU32(ctx, 31);
            uint32_t sp = getRegU32(ctx, 29);
            uint32_t f_state = 0;
            if (rdram && (0x772188u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                s_cachedRdram = rdram;
                std::memcpy(&f_state, rdram + (0x772188u & PS2_RAM_MASK), 4);
                if (f_state == 9) {
                    s_hasSeenFState9.store(true, std::memory_order_relaxed);
                }
            }
            uint32_t cnt4E = cnt_ar_4e914c_total.load(std::memory_order_relaxed);
            std::printf("[probe] au 36c3b4 n=%u f_state=%u cnt4E=%u ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X\n",
                        n, f_state, cnt4E, ra, sp, a0, a1, a2, a3);
            if (rdram) {
                std::printf("[probe] au 36c3b4 str a0='%s' a1='%s' a2='%s' a3='%s'\n",
                            getAsciiStrCap80(a0, rdram).c_str(), getAsciiStrCap80(a1, rdram).c_str(),
                            getAsciiStrCap80(a2, rdram).c_str(), getAsciiStrCap80(a3, rdram).c_str());
            }
            std::fflush(stdout);
        }
    }

    void on165EFC(R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled()) return;
        cnt_au_165efc.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            uint32_t a0 = getRegU32(ctx, 4);
            uint32_t a1 = getRegU32(ctx, 5);
            uint32_t a2 = getRegU32(ctx, 6);
            uint32_t a3 = getRegU32(ctx, 7);
            uint32_t ra = getRegU32(ctx, 31);
            uint32_t sp = getRegU32(ctx, 29);
            uint32_t f_state = 0;
            if (rdram && (0x772188u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                s_cachedRdram = rdram;
                std::memcpy(&f_state, rdram + (0x772188u & PS2_RAM_MASK), 4);
                if (f_state == 9) {
                    s_hasSeenFState9.store(true, std::memory_order_relaxed);
                }
            }
            uint32_t cnt4E = cnt_ar_4e914c_total.load(std::memory_order_relaxed);
            std::printf("[probe] au 165efc n=%u f_state=%u cnt4E=%u ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X\n",
                        n, f_state, cnt4E, ra, sp, a0, a1, a2, a3);
            if (rdram) {
                std::printf("[probe] au 165efc str a0='%s' a1='%s' a2='%s' a3='%s'\n",
                            getAsciiStrCap80(a0, rdram).c_str(), getAsciiStrCap80(a1, rdram).c_str(),
                            getAsciiStrCap80(a2, rdram).c_str(), getAsciiStrCap80(a3, rdram).c_str());
            }
            std::fflush(stdout);
        }
    }

    // Task AV Probes
    std::atomic<uint32_t> cnt_vif1_kicks_started{0};
    std::atomic<uint32_t> cnt_vif1_kicks_finished{0};
    std::atomic<uint32_t> cnt_vif1_irq{0};
    std::atomic<uint32_t> cnt_cause5_raises{0};
    std::atomic<uint32_t> cnt_404230_loop{0};

    static std::mutex s_vifMarkMutex;
    static std::unordered_map<uint16_t, uint32_t> s_vifMarkCounts;

    static std::mutex s_h2843d4Mutex;
    static std::unordered_map<uint32_t, uint32_t> s_h2843d4Counts;
    static std::atomic<uint32_t> s_h2843d4Total{0};

    void onVif1DmaStart(uint32_t sizeBytes) {
        if (!isEnabled()) return;
        (void)sizeBytes;
        cnt_vif1_kicks_started.fetch_add(1, std::memory_order_relaxed);
    }

    void onVif1DmaEnd() {
        if (!isEnabled()) return;
        cnt_vif1_kicks_finished.fetch_add(1, std::memory_order_relaxed);
    }

    void onVif1Irq() {
        if (!isEnabled()) return;
        cnt_vif1_irq.fetch_add(1, std::memory_order_relaxed);
    }

    void onVif1Mark(uint16_t imm) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_vifMarkMutex);
        s_vifMarkCounts[imm]++;
    }

    void onCause5Raise() {
        if (!isEnabled()) return;
        cnt_cause5_raises.fetch_add(1, std::memory_order_relaxed);
    }

    void on2843D4MarkRead(uint32_t markVal) {
        if (!isEnabled()) return;
        s_h2843d4Total.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(s_h2843d4Mutex);
        s_h2843d4Counts[markVal]++;
    }

    void on404230LoopEntry() {
        if (!isEnabled()) return;
        cnt_404230_loop.fetch_add(1, std::memory_order_relaxed);
    }

    void onVifUnknown(uint8_t opcode, uint16_t imm) {
        if (!isEnabled()) return;
        static std::atomic<uint32_t> s_unknownCount{0};
        uint32_t n = s_unknownCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10) {
            std::printf("[probe] av vif_unknown op=0x%02X imm=0x%04X\n", opcode, imm);
            std::fflush(stdout);
        }
    }

    std::string getAndResetVifMarkHist() {
        std::lock_guard<std::mutex> lock(s_vifMarkMutex);
        if (s_vifMarkCounts.empty()) return "none";
        std::vector<std::pair<uint16_t, uint32_t>> vec(s_vifMarkCounts.begin(), s_vifMarkCounts.end());
        std::sort(vec.begin(), vec.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });
        std::string s;
        size_t count = std::min<size_t>(vec.size(), 6);
        for (size_t i = 0; i < count; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s0x%04X:%u", (i > 0 ? "," : ""), vec[i].first, vec[i].second);
            s += buf;
        }
        s_vifMarkCounts.clear();
        return s;
    }

    std::string getAndResetH2843D4Hist(uint32_t &outTotal) {
        outTotal = s_h2843d4Total.exchange(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(s_h2843d4Mutex);
        if (s_h2843d4Counts.empty()) return "none";
        std::vector<std::pair<uint32_t, uint32_t>> vec(s_h2843d4Counts.begin(), s_h2843d4Counts.end());
        std::sort(vec.begin(), vec.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });
        std::string s;
        size_t count = std::min<size_t>(vec.size(), 6);
        for (size_t i = 0; i < count; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s0x%04X:%u", (i > 0 ? "," : ""), vec[i].first, vec[i].second);
            s += buf;
        }
        s_h2843d4Counts.clear();
        return s;
    }

    // Task AW Probes
    struct AwPkt {
        uint32_t addr;
        uint32_t words[8];
        uint32_t numWords;
    };

    static std::mutex s_awMutex;
    static uint32_t s_awTotalKicks = 0;
    static uint32_t s_awNoMarkCount = 0;
    static bool s_awDumpedCmp = false;

    // Current kick
    static uint32_t s_currKickNum = 0;
    static bool s_currKickHadMark2000 = false;
    static std::vector<std::string> s_currKickTrace;
    static std::vector<AwPkt> s_currKickPackets;
    static std::vector<std::string> s_currKickUnpacks;

    // Previous normal kick (with MARK 0x2000)
    static bool s_hasPrevNormalKick = false;
    static uint32_t s_prevKickNum = 0;
    static std::vector<std::string> s_prevKickTrace;
    static std::vector<AwPkt> s_prevKickPackets;
    static std::vector<std::string> s_prevKickUnpacks;

    void onAwDma1KickStart(uint32_t tadr, uint32_t chcr) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_awMutex);
        s_currKickNum = ++s_awTotalKicks;
        s_currKickHadMark2000 = false;
        s_currKickTrace.clear();
        s_currKickPackets.clear();
        s_currKickUnpacks.clear();

        char buf[128];
        std::snprintf(buf, sizeof(buf), "[probe] aw kick# %u tadr=0x%08X chcr=0x%08X", s_currKickNum, tadr, chcr);
        s_currKickTrace.push_back(buf);
    }

    void onAwDma1Tag(uint32_t addr, uint32_t id, uint32_t qwc, uint32_t next, uint32_t dataAddr, const uint32_t *words, uint32_t wordCount) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_awMutex);
        if (s_currKickTrace.size() < 96) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[probe] aw tag addr=0x%08X id=%u qwc=0x%X next=0x%08X", addr, id, qwc, next);
            s_currKickTrace.push_back(buf);
        }
        if (wordCount > 0 && s_currKickPackets.size() < 30) {
            AwPkt pkt{};
            pkt.addr = dataAddr;
            pkt.numWords = std::min<uint32_t>(8u, wordCount);
            for (uint32_t i = 0; i < pkt.numWords; ++i) {
                pkt.words[i] = words[i];
            }
            s_currKickPackets.push_back(pkt);
        }
    }

    void onAwVifKickStart() {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_awMutex);
        if (s_currKickTrace.empty()) {
            s_currKickNum = ++s_awTotalKicks;
            s_currKickHadMark2000 = false;
            s_currKickTrace.clear();
            s_currKickPackets.clear();
            s_currKickUnpacks.clear();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[probe] aw kick# %u tadr=0x00000000 chcr=0x00000000", s_currKickNum);
            s_currKickTrace.push_back(buf);
        }
    }

    void onAwMark2000() {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_awMutex);
        s_currKickHadMark2000 = true;
    }

    void onAwVifCode(uint32_t off, uint32_t cmd, uint8_t num, uint16_t imm, bool irq, uint32_t consumed) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_awMutex);
        if (s_currKickTrace.size() < 96) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[probe] aw vc off=0x%X cmd=0x%08X num=0x%02X imm=0x%04X irq=%d consumed_bytes=%u",
                          off, cmd, (uint32_t)num, (uint32_t)imm, irq ? 1 : 0, consumed);
            s_currKickTrace.push_back(buf);
        }
    }

    void onAwUnpack(uint8_t fmt, uint8_t num, uint32_t cl, uint32_t wl, uint32_t usn, uint32_t flg, uint32_t addr, uint32_t words) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_awMutex);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[probe] aw unpack fmt=0x%X num=%u cl=%u wl=%u usn=%u flg=%u addr=0x%03X words=%u",
                      (uint32_t)fmt, (uint32_t)num, cl, wl, usn, flg, addr, words);
        s_currKickUnpacks.push_back(buf);
    }

    void onAwVifKickEnd(const uint8_t *rdram) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_awMutex);

        uint32_t f_state = 0;
        if (rdram && (0x772188u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
            s_cachedRdram = rdram;
            std::memcpy(&f_state, rdram + (0x772188u & PS2_RAM_MASK), 4);
        }
        if (f_state == 9) {
            s_hasSeenFState9.store(true, std::memory_order_relaxed);
        }

        if (!s_currKickHadMark2000 && f_state == 9) {
            if (s_awNoMarkCount < 3) {
                s_awNoMarkCount++;

                // First time: dump previous normal kick for comparison
                if (!s_awDumpedCmp && s_hasPrevNormalKick) {
                    s_awDumpedCmp = true;
                    for (const auto &line : s_prevKickTrace) {
                        if (line.rfind("[probe] aw ", 0) == 0) {
                            std::printf("[probe] aw cmp %s\n", line.c_str() + 11);
                        } else {
                            std::printf("[probe] aw cmp %s\n", line.c_str());
                        }
                    }
                    for (const auto &pkt : s_prevKickPackets) {
                        char hexBuf[256];
                        int n = std::snprintf(hexBuf, sizeof(hexBuf), "[probe] aw cmp pkt addr=0x%08X:", pkt.addr);
                        for (uint32_t i = 0; i < pkt.numWords; ++i) {
                            n += std::snprintf(hexBuf + n, sizeof(hexBuf) - n, " %08X", pkt.words[i]);
                        }
                        std::printf("%s\n", hexBuf);
                    }
                    for (const auto &upk : s_prevKickUnpacks) {
                        if (upk.rfind("[probe] aw ", 0) == 0) {
                            std::printf("[probe] aw cmp %s\n", upk.c_str() + 11);
                        } else {
                            std::printf("[probe] aw cmp %s\n", upk.c_str());
                        }
                    }
                }

                // Dump this no-MARK kick
                for (const auto &line : s_currKickTrace) {
                    std::printf("%s\n", line.c_str());
                }
                for (const auto &pkt : s_currKickPackets) {
                    char hexBuf[256];
                    int n = std::snprintf(hexBuf, sizeof(hexBuf), "[probe] aw pkt addr=0x%08X:", pkt.addr);
                    for (uint32_t i = 0; i < pkt.numWords; ++i) {
                        n += std::snprintf(hexBuf + n, sizeof(hexBuf) - n, " %08X", pkt.words[i]);
                    }
                    std::printf("%s\n", hexBuf);
                }
                for (const auto &upk : s_currKickUnpacks) {
                    std::printf("%s\n", upk.c_str());
                }
                std::fflush(stdout);
            }
        } else if (s_currKickHadMark2000) {
            // Normal kick with MARK 0x2000
            s_prevKickNum = s_currKickNum;
            s_prevKickTrace = s_currKickTrace;
            s_prevKickPackets = s_currKickPackets;
            s_prevKickUnpacks = s_currKickUnpacks;
            s_hasPrevNormalKick = true;
        }
    }

    // Task AY State
    static std::mutex s_ayMutex;

    // 1. GREEN
    static uint32_t s_ayChainCountAfterF9 = 0;
    static bool s_ayCurrentChainEligible = false;
    static uint32_t s_ayUnpackPrintCountThisChain = 0;
    static std::vector<AySpan> s_aySpans;

    void onAyChainStart(uint32_t tagCount, const std::vector<AySpan> &spans) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_ayMutex);
        s_aySpans = spans;
        s_ayUnpackPrintCountThisChain = 0;

        if (s_hasSeenFState9.load(std::memory_order_relaxed) && tagCount > 2000) {
            s_ayChainCountAfterF9++;
            s_ayCurrentChainEligible = (s_ayChainCountAfterF9 <= 2);
        } else {
            s_ayCurrentChainEligible = false;
        }
    }

    void onAyUnpackCheck(uint8_t fmt, uint32_t vuAddr, uint8_t num, uint32_t posPayload, const uint8_t *payload, uint32_t sizeBytes) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_ayMutex);
        if (!s_ayCurrentChainEligible || fmt != 2) return;
        if (s_ayUnpackPrintCountThisChain >= 8) return;

        uint32_t n = ++s_ayUnpackPrintCountThisChain;

        uint32_t srcGuest = 0;
        for (const auto &sp : s_aySpans) {
            if (posPayload >= sp.bufOffset && posPayload < sp.bufOffset + sp.byteCount) {
                srcGuest = sp.srcGuestAddr + (posPayload - sp.bufOffset);
                break;
            }
        }

        char hexBytes[33] = {0};
        for (int i = 0; i < 16; ++i) {
            if (posPayload + i < sizeBytes && payload) {
                std::snprintf(hexBytes + i * 2, 3, "%02x", payload[i]);
            } else {
                std::snprintf(hexBytes + i * 2, 3, "00");
            }
        }

        std::printf("[probe] ay unpack# %u addr=0x%03X num=%u src=0x%08X bytes=%s\n",
                    n, vuAddr, (uint32_t)num, srcGuest, hexBytes);
        std::fflush(stdout);
    }

    void onAyRaw8(uint32_t callNum, uint32_t dst, uint32_t a2, const uint8_t *pDst) {
        if (!isEnabled()) return;
        if (callNum >= 6) return;

        char hexDst[33] = {0};
        char hexDst100[33] = {0};
        uint32_t nonzero1800 = 0;
        if (pDst) {
            for (int i = 0; i < 16; ++i) {
                std::snprintf(hexDst + i * 2, 3, "%02x", pDst[i]);
                std::snprintf(hexDst100 + i * 2, 3, "%02x", pDst[0x100 + i]);
            }
            for (int i = 0; i < 0x1800; ++i) {
                if (pDst[i] != 0) nonzero1800++;
            }
        }
        std::printf("[probe] ay raw8 n=%u a1(dst)=0x%08X a2=%u dst_16=%s dst_100_16=%s nonzero_1800=%u\n",
                    callNum, dst, a2, hexDst, hexDst100, nonzero1800);
        std::fflush(stdout);
    }

    // 2. TIME
    static std::atomic<uint64_t> s_ayTimeVif1Us{0};
    static std::atomic<uint64_t> s_ayTimeVu1Us{0};
    static std::atomic<uint32_t> s_ayCountVu1{0};
    static std::atomic<uint64_t> s_ayMaxVu1Us{0};
    static std::atomic<uint64_t> s_ayTimeGifUs{0};
    static std::atomic<uint64_t> s_ayTimeChainUs{0};
    static std::atomic<uint32_t> s_ayKicks{0};

    void onAyTimeVif1(uint64_t us) {
        if (!isEnabled()) return;
        s_ayTimeVif1Us.fetch_add(us, std::memory_order_relaxed);
    }

    void onAyTimeVu1(uint64_t us) {
        if (!isEnabled()) return;
        s_ayTimeVu1Us.fetch_add(us, std::memory_order_relaxed);
        s_ayCountVu1.fetch_add(1, std::memory_order_relaxed);
        uint64_t curMax = s_ayMaxVu1Us.load(std::memory_order_relaxed);
        while (us > curMax && !s_ayMaxVu1Us.compare_exchange_weak(curMax, us, std::memory_order_relaxed)) {}
    }

    void onAyTimeGif(uint64_t us) {
        if (!isEnabled()) return;
        s_ayTimeGifUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onAyTimeChain(uint64_t us) {
        if (!isEnabled()) return;
        s_ayTimeChainUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onAyKick() {
        if (!isEnabled()) return;
        s_ayKicks.fetch_add(1, std::memory_order_relaxed);
    }

    void dumpAyTimeIfDue(uint32_t sec) {
        if (!isEnabled()) return;
        (void)sec;
        double vif1Ms = s_ayTimeVif1Us.exchange(0, std::memory_order_relaxed) / 1000.0;
        double vu1Ms = s_ayTimeVu1Us.exchange(0, std::memory_order_relaxed) / 1000.0;
        uint32_t vu1N = s_ayCountVu1.exchange(0, std::memory_order_relaxed);
        double vu1MaxMs = s_ayMaxVu1Us.exchange(0, std::memory_order_relaxed) / 1000.0;
        double gifMs = s_ayTimeGifUs.exchange(0, std::memory_order_relaxed) / 1000.0;
        double chainMs = s_ayTimeChainUs.exchange(0, std::memory_order_relaxed) / 1000.0;
        uint32_t kicks = s_ayKicks.exchange(0, std::memory_order_relaxed);

        std::printf("[probe] ay time vif1=%.2f vu1=%.2f(n=%u,max=%.2f) gif=%.2f chain=%.2f kicks=%u\n",
                    vif1Ms, vu1Ms, vu1N, vu1MaxMs, gifMs, chainMs, kicks);
        std::fflush(stdout);
    }

    // 3. WHITE STRIP
    struct AyPrimHistory {
        char type[16];
        int x0, y0, x1, y1;
        uint32_t rgba;
        char tex[32];
        uint64_t alpha;
        uint64_t test;
        uint32_t fbp;
    };
    static std::mutex s_ayWhiteMutex;
    static AyPrimHistory s_ayPrimRing[16];
    static uint32_t s_ayPrimRingCount = 0;
    static uint32_t s_ayLastWhiteSec = 0xFFFFFFFFu;
    static uint32_t s_ayWhiteReportsThisSec = 0;

    void onAyPrim(const char *type, int x0, int y0, int x1, int y1, uint32_t rgba, const char *tex, uint64_t alpha, uint64_t test, uint32_t fbp) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_ayWhiteMutex);
        uint32_t idx = (s_ayPrimRingCount++) % 16;
        auto &p = s_ayPrimRing[idx];
        std::strncpy(p.type, type ? type : "unknown", sizeof(p.type) - 1);
        p.type[sizeof(p.type) - 1] = '\0';
        p.x0 = x0; p.y0 = y0; p.x1 = x1; p.y1 = y1;
        p.rgba = rgba;
        std::strncpy(p.tex, tex ? tex : "none", sizeof(p.tex) - 1);
        p.tex[sizeof(p.tex) - 1] = '\0';
        p.alpha = alpha;
        p.test = test;
        p.fbp = fbp;
    }

    void onAyCheckWhiteStrip(uint32_t frame, uint32_t fbp, uint32_t fbw, uint32_t psm, const uint8_t *pixels, uint32_t width, uint32_t height) {
        if (!isEnabled()) return;
        if (!pixels || width < 321 || height < 441) return;

        size_t idx = (440u * width + 320u) * 4u;
        uint8_t r = pixels[idx + 0];
        uint8_t g = pixels[idx + 1];
        uint8_t b = pixels[idx + 2];

        if (r == 255 && g == 255 && b == 255) {
            std::lock_guard<std::mutex> lock(s_ayWhiteMutex);
            uint32_t sec = s_currentSec.load(std::memory_order_relaxed);
            if (sec != s_ayLastWhiteSec) {
                s_ayLastWhiteSec = sec;
                s_ayWhiteReportsThisSec = 0;
            }
            if (s_ayWhiteReportsThisSec < 5) {
                s_ayWhiteReportsThisSec++;
                std::printf("[probe] ay white frame=%u fbp=0x%X fbw=%u psm=0x%X\n", frame, fbp, fbw, psm);
                uint32_t total = std::min<uint32_t>(s_ayPrimRingCount, 16u);
                uint32_t toPrint = std::min<uint32_t>(total, 6u);
                for (uint32_t i = 0; i < toPrint; ++i) {
                    uint32_t pIdx = (s_ayPrimRingCount - toPrint + i) % 16;
                    const auto &p = s_ayPrimRing[pIdx];
                    std::printf("[probe] ay prim type=%s bbox=(%d,%d)-(%d,%d) rgbaq=0x%08X tex=%s alpha=0x%llX test=0x%llX fbp=0x%X\n",
                                p.type, p.x0, p.y0, p.x1, p.y1, p.rgba, p.tex,
                                (unsigned long long)p.alpha, (unsigned long long)p.test, p.fbp);
                }
                std::fflush(stdout);
            }
        }
    }

    // Task AZ Probes
    static std::atomic<uint32_t> s_azPops{0};
    static std::atomic<uint32_t> s_azFramesOut{0};
    static std::atomic<uint32_t> s_azRaw8Calls{0};
    static std::atomic<uint32_t> s_azRaw8Have{0};

    void onAzPop(uint32_t head, uint32_t tail, uint32_t addr, uint32_t size, const uint8_t *payload, uint32_t outFrames) {
        if (!isEnabled()) return;
        s_azPops.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_azPopCount{0};
        uint32_t n = s_azPopCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 12) {
            char hex[17] = {0};
            for (int i = 0; i < 8; ++i) {
                uint8_t b = (payload && (uint32_t)i < size) ? payload[i] : 0;
                std::snprintf(hex + i * 2, 3, "%02x", b);
            }
            std::printf("[probe] az pop head=%u tail=%u addr=0x%08X size=0x%X first8=%s out_frames=%u\n",
                        head, tail, addr, size, hex, outFrames);
            std::fflush(stdout);
        }
    }

    void onAzRecordFramesOut(uint32_t count) {
        if (!isEnabled()) return;
        s_azFramesOut.fetch_add(count, std::memory_order_relaxed);
    }

    void onAzRaw8Call(bool haveFrame, uint32_t width, uint32_t height, const uint8_t *mb0) {
        if (!isEnabled()) return;
        if (haveFrame) {
            s_hasSeenFState9.store(true, std::memory_order_relaxed);
            s_lastRaw8ValidTime = std::chrono::steady_clock::now();
            s_hasSeenValidRaw8.store(true, std::memory_order_relaxed);
        }
        s_azRaw8Calls.fetch_add(1, std::memory_order_relaxed);
        if (haveFrame) {
            s_azRaw8Have.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<uint32_t> s_azRaw8FrameCount{0};
            uint32_t n = s_azRaw8FrameCount.fetch_add(1, std::memory_order_relaxed);
            if (n < 4 && mb0) {
                char yHex[17] = {0}, cbHex[17] = {0}, crHex[17] = {0};
                for (int i = 0; i < 8; ++i) {
                    std::snprintf(yHex + i * 2, 3, "%02x", mb0[i]);
                    std::snprintf(cbHex + i * 2, 3, "%02x", mb0[256 + i]);
                    std::snprintf(crHex + i * 2, 3, "%02x", mb0[320 + i]);
                }
                std::printf("[probe] az raw8 mb0 Y[0..8]=%s Cb0=%s Cr0=%s w=%u h=%u\n",
                            yHex, cbHex, crHex, width, height);
                std::fflush(stdout);
            }
        }
    }

    void dumpAzRingIfDue(uint32_t sec, const uint8_t *rdram) {
        if (!isEnabled()) return;
        (void)sec;
        uint32_t tail = 0, head = 0;
        if (rdram) {
            std::memcpy(&tail, rdram + (0x696E88u & PS2_RAM_MASK), sizeof(uint32_t));
            std::memcpy(&head, rdram + (0x696E8Cu & PS2_RAM_MASK), sizeof(uint32_t));
        }
        uint32_t pops = s_azPops.exchange(0, std::memory_order_relaxed);
        uint32_t framesOut = s_azFramesOut.exchange(0, std::memory_order_relaxed);
        uint32_t raw8Calls = s_azRaw8Calls.exchange(0, std::memory_order_relaxed);
        uint32_t raw8Have = s_azRaw8Have.exchange(0, std::memory_order_relaxed);

        std::printf("[probe] az ring head=%u tail=%u pops=%u frames_out=%u raw8_calls=%u raw8_have=%u\n",
                    head, tail, pops, framesOut, raw8Calls, raw8Have);
        std::fflush(stdout);
    }

    // Task BA Probes
    void onBaRows(bool haveFrame, uint32_t mbCols, uint32_t mbRows, uint32_t dst, const uint8_t *rdram) {
        if (!isEnabled()) return;
        if (rdram) s_cachedRdram = rdram;
        if (haveFrame) {
            s_hasSeenFState9.store(true, std::memory_order_relaxed);
            s_lastRaw8ValidTime = std::chrono::steady_clock::now();
            s_hasSeenValidRaw8.store(true, std::memory_order_relaxed);
        }
        if (!haveFrame || !rdram || mbCols == 0 || mbRows == 0) return;
        static std::atomic<uint32_t> s_baRowsCount{0};
        uint32_t n = s_baRowsCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 3) {
            std::string line = "[probe] ba rows frame=" + std::to_string(n) + " means=";
            for (uint32_t r = 0; r < mbRows; ++r) {
                uint64_t sumY = 0;
                uint32_t count = 0;
                for (uint32_t c = 0; c < mbCols; ++c) {
                    uint32_t mbIdx = c * mbRows + r;
                    const uint8_t *mb = rdram + ((dst + mbIdx * 384u) & PS2_RAM_MASK);
                    for (uint32_t i = 0; i < 256u; ++i) {
                        sumY += mb[i];
                        count++;
                    }
                }
                double mean = count > 0 ? (double)sumY / count : 0.0;
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.1f%s", mean, (r + 1 < mbRows) ? " " : "");
                line += buf;
            }
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    }

    static std::atomic<uint32_t> s_baRuns{0};
    static std::atomic<uint64_t> s_baInstrs{0};
    static std::atomic<uint32_t> s_baXgkick{0};
    static std::atomic<uint32_t> s_baGifQw{0};
    static std::atomic<uint32_t> s_baPrims{0};
    static std::atomic<uint64_t> s_baLoopNs{0};
    static std::atomic<uint64_t> s_baXgkickNs{0};
    static std::atomic<uint32_t> s_baMaxInst{0};

    static std::mutex s_baPcMutex;
    static std::map<uint32_t, uint32_t> s_baPcHistogram;

    // First 40 instructions of first movie kick
    struct BaInstDump {
        uint32_t pc;
        uint32_t upper;
        uint32_t lower;
    };
    static std::mutex s_baFirstDumpMutex;
    static bool s_baRecordingFirstDump = false;
    static bool s_baFirstDumpComplete = false;
    static int32_t s_baFirstVi[16]{};
    static std::vector<BaInstDump> s_baFirst40Inst;

    void onBaMscalStart(uint32_t startPC, const int32_t *vi) {
        if (!isEnabled()) return;
        s_baRuns.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(s_baPcMutex);
            s_baPcHistogram[startPC]++;
        }

        if (!s_baFirstDumpComplete && (val_state.load(std::memory_order_relaxed) == 9 || s_hasSeenFState9.load(std::memory_order_relaxed) || s_ayChainCountAfterF9 > 0)) {
            std::lock_guard<std::mutex> lock(s_baFirstDumpMutex);
            if (!s_baFirstDumpComplete && !s_baRecordingFirstDump) {
                s_baRecordingFirstDump = true;
                if (vi) {
                    for (int i = 0; i < 16; ++i) s_baFirstVi[i] = vi[i];
                }
            }
        }
    }

    void onBaMscntStart(const int32_t *vi) {
        if (!isEnabled()) return;
        s_baRuns.fetch_add(1, std::memory_order_relaxed);
        (void)vi;
    }

    void onBaInstruction(uint32_t pc, uint32_t upper, uint32_t lower) {
        if (!isEnabled()) return;
        if (s_baRecordingFirstDump) {
            std::lock_guard<std::mutex> lock(s_baFirstDumpMutex);
            if (s_baRecordingFirstDump && s_baFirst40Inst.size() < 40) {
                s_baFirst40Inst.push_back({pc, upper, lower});
            }
        }
    }

    void onBaXgkick(uint32_t qwords, uint64_t xgkickNs, uint32_t prims) {
        if (!isEnabled()) return;
        s_baXgkick.fetch_add(1, std::memory_order_relaxed);
        s_baGifQw.fetch_add(qwords, std::memory_order_relaxed);
        s_baPrims.fetch_add(prims, std::memory_order_relaxed);
        s_baXgkickNs.fetch_add(xgkickNs, std::memory_order_relaxed);
    }

    void onBaRunEnd(uint32_t instructionsExecuted, uint64_t loopNs) {
        if (!isEnabled()) return;
        s_baInstrs.fetch_add(instructionsExecuted, std::memory_order_relaxed);
        s_baLoopNs.fetch_add(loopNs, std::memory_order_relaxed);

        uint32_t currentMax = s_baMaxInst.load(std::memory_order_relaxed);
        while (instructionsExecuted > currentMax &&
               !s_baMaxInst.compare_exchange_weak(currentMax, instructionsExecuted, std::memory_order_relaxed)) {}

        if (s_baRecordingFirstDump) {
            std::lock_guard<std::mutex> lock(s_baFirstDumpMutex);
            if (s_baRecordingFirstDump && !s_baFirstDumpComplete) {
                s_baRecordingFirstDump = false;
                s_baFirstDumpComplete = true;

                std::printf("[probe] ba first40 vi:");
                for (int i = 0; i < 16; ++i) {
                    std::printf(" VI%02d=0x%04X", i, static_cast<uint16_t>(s_baFirstVi[i]));
                }
                std::printf("\n");

                for (size_t i = 0; i < s_baFirst40Inst.size(); ++i) {
                    std::printf("[probe] ba first40 #%02u pc=0x%03X %08X:%08X\n",
                                static_cast<unsigned>(i),
                                s_baFirst40Inst[i].pc,
                                s_baFirst40Inst[i].upper,
                                s_baFirst40Inst[i].lower);
                }
                std::fflush(stdout);
            }
        }
    }

    void dumpBaVu1IfDue(uint32_t sec) {
        if (!isEnabled()) return;
        (void)sec;

        uint32_t runs = s_baRuns.exchange(0, std::memory_order_relaxed);
        uint64_t instrs = s_baInstrs.exchange(0, std::memory_order_relaxed);
        uint32_t xgkick = s_baXgkick.exchange(0, std::memory_order_relaxed);
        uint32_t gifQw = s_baGifQw.exchange(0, std::memory_order_relaxed);
        uint32_t prims = s_baPrims.exchange(0, std::memory_order_relaxed);
        uint64_t loopNs = s_baLoopNs.exchange(0, std::memory_order_relaxed);
        uint64_t xgkickNs = s_baXgkickNs.exchange(0, std::memory_order_relaxed);
        uint32_t maxInst = s_baMaxInst.exchange(0, std::memory_order_relaxed);

        std::vector<std::pair<uint32_t, uint32_t>> topPcs;
        {
            std::lock_guard<std::mutex> lock(s_baPcMutex);
            for (auto const & [pc, count] : s_baPcHistogram) {
                topPcs.push_back({count, pc});
            }
            s_baPcHistogram.clear();
        }
        std::sort(topPcs.rbegin(), topPcs.rend());
        std::string pcHist;
        size_t nTop = std::min<size_t>(topPcs.size(), 8u);
        for (size_t i = 0; i < nTop; ++i) {
            if (!pcHist.empty()) pcHist += " ";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%03X=%u", topPcs[i].second, topPcs[i].first);
            pcHist += buf;
        }
        if (pcHist.empty()) pcHist = "none";

        double loopMs = loopNs / 1000000.0;
        double xgkickMs = xgkickNs / 1000000.0;

        std::printf("[probe] ba vu1 runs=%u instrs=%llu xgkick=%u gif_qw=%u prims=%u loop_ms=%.2f xgkick_ms=%.2f (loop_ns=%llu xgkick_ns=%llu) max_inst=%u top_pc=%s\n",
                    runs, (unsigned long long)instrs, xgkick, gifQw, prims,
                    loopMs, xgkickMs, (unsigned long long)loopNs, (unsigned long long)xgkickNs,
                    maxInst, pcHist.c_str());
        std::fflush(stdout);
    }

    // Task BB Implementation
    struct BbUnpackInfo {
        uint32_t srcGuest;
        uint32_t len;
        uint32_t vuAddr;
        uint8_t fmt;
        uint8_t num;
    };

    struct BbVif1Regs {
        uint32_t itop = 0;
        uint32_t top = 0;
        uint32_t tops = 0;
        uint32_t base = 0;
        uint32_t ofst = 0;
        uint32_t cl = 0;
        uint32_t wl = 0;
        uint32_t mode = 0;
    };

    static std::mutex s_bbMutex;
    static bool s_bbIsMovieKick = false;
    static uint32_t s_bbCurrentKickIndex = 0;
    static uint32_t s_bbCurrentTadr = 0;
    static uint32_t s_bbCurrentKickTagCount = 0;
    static std::vector<AySpan> s_bbSpans;
    static uint32_t s_bbMscalCountAfterF9 = 0;
    static int s_bbDumpedRunsCount = 0;
    static int s_bbActiveRecordingRunIndex = -1;
    static int s_brightDumpedCount = 0;
    static uint32_t s_lastBrightMscal = 0;
    static bool s_vu1CodeDumped = false;
    static bool s_bbActiveRecordingRun = false;
    static std::vector<BbUnpackInfo> s_bbPrevMscalUnpacks;
    static uint32_t s_bbLastRaw8Dst = 0;
    static bool s_bbDumpedFrame0Ycc = false;
    static BbVif1Regs s_bbLastVif1Regs{};
    static std::vector<BbUnpackInfo> s_bbUnpacksSinceLastMscal;

    static std::array<uint64_t, 128> s_bbGsRegs{};
    static uint64_t s_bbDispfb1 = 0;
    static uint64_t s_bbDisplay1 = 0;
    static uint64_t s_bbDispfb2 = 0;
    static uint64_t s_bbDisplay2 = 0;

    static std::vector<uint8_t> s_bbVuCode(16384, 0);
    static std::vector<uint8_t> s_bbRunDataStart(16384, 0);
    static std::vector<uint8_t> s_bbRunVf(512, 0);
    static std::vector<uint8_t> s_bbRunVi(64, 0);
    static std::vector<uint8_t> s_bbRunAcc(16, 0);
    static float s_bbRunQ = 0.0f;
    static float s_bbRunP = 0.0f;
    static float s_bbRunI = 0.0f;
    static std::vector<std::vector<uint8_t>> s_bbRunXgkickPackets;

    static uint32_t s_bbRunMscalCount = 0;
    static uint32_t s_bbRunKickIndex = 0;
    static uint32_t s_bbRunTadr = 0;
    static BbVif1Regs s_bbRunVif1{};
    static std::vector<BbUnpackInfo> s_bbRunUnpacks;
    static uint32_t s_bbRunRaw8Dst = 0;
    static std::array<uint64_t, 128> s_bbRunGsRegs{};
    static uint64_t s_bbRunDispfb1 = 0;
    static uint64_t s_bbRunDisplay1 = 0;
    static uint64_t s_bbRunDispfb2 = 0;
    static uint64_t s_bbRunDisplay2 = 0;

    static void ensureDumpDir() {
        static bool s_done = false;
        if (!s_done) {
            s_done = true;
            std::error_code ec;
            std::filesystem::create_directories("logs/vu1dump", ec);
        }
    }

    struct ProbeInitDirs {
        ProbeInitDirs() {
            std::error_code ec;
            std::filesystem::create_directories("logs/vu1dump", ec);
        }
    };
    static ProbeInitDirs s_probeInitDirs;

    static std::atomic<uint32_t> s_bbGateDiagCount{0};
    static std::atomic<uint32_t> s_movieKickMscalIndex{0};
    static std::atomic<uint32_t> s_movieKickFirstUnpackSrc{0};
    // Task BJ file sink and logger
    static std::mutex s_bhSinkMutex;
    static FILE* bhSink() {
        static FILE* s_file = nullptr;
        static bool s_tried = false;
        if (!s_tried) {
            s_tried = true;
            std::error_code ec;
            std::filesystem::create_directories("logs", ec);
            s_file = std::fopen("logs/probe_bh_sink.txt", "w"); // PROBE-IL: one run per file (was "a")
        }
        return s_file;
    }

    void bhLog(const char *fmt, ...) {
        char buf[4096];
        va_list args;
        va_start(args, fmt);
        int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        if (len > 0) {
            size_t writeLen = (len < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(len) : (sizeof(buf) - 1);
            {
                std::lock_guard<std::mutex> lock(s_bhSinkMutex);
                FILE *f = bhSink();
                if (f) {
                    std::fwrite(buf, 1, writeLen, f);
                    std::fflush(f);
                }
            }
            ps2xSafeLog("%s", buf);
        }
    }

    static inline bool isBhProbeEnabled() {
        static const bool s_v = []() -> bool {
            const char *env = std::getenv("PS2X_PROBE_AE");
            if (env && (env[0] == '1' || env[0] == 't' || env[0] == 'y' || env[0] == 'T' || env[0] == 'Y')) {
                return true;
            }
#ifdef _WIN32
            char winBuf[16] = {0};
            if (GetEnvironmentVariableA("PS2X_PROBE_AE", winBuf, sizeof(winBuf)) > 0) {
                if (winBuf[0] == '1' || winBuf[0] == 't' || winBuf[0] == 'y' || winBuf[0] == 'T' || winBuf[0] == 'Y') {
                    return true;
                }
            }
#endif
            const char *envBh = std::getenv("PS2X_PROBE_BH");
            if (envBh && (envBh[0] == '1' || envBh[0] == 't' || envBh[0] == 'y' || envBh[0] == 'T' || envBh[0] == 'Y')) {
                return true;
            }
            return isEnabled();
        }();
        return s_v;
    }

    enum class KickDecision {
        Undecided,
        Native,
        Interpret
    };
    static std::atomic<KickDecision> s_kickDecision{KickDecision::Undecided};

    bool isMovieKickNative() {
        return (s_kickDecision.load(std::memory_order_relaxed) == KickDecision::Native);
    }

    bool isMovieSkipUnpackEnabled() {
        static int s_skip = -1;
        if (s_skip == -1) {
            const char *env = std::getenv("PS2X_MOVIE_SKIP_UNPACK");
            s_skip = (env && env[0] == '0') ? 0 : 1; // default ON (1)
        }
        return s_skip == 1;
    }

    void recordBlSkipUnpack(uint32_t skippedCount, uint32_t skippedBytes) {
        static std::atomic<uint32_t> s_blSkipKickCount{0};
        uint32_t k = s_blSkipKickCount.fetch_add(1, std::memory_order_relaxed);
        if (k < 3) {
            bhLog("[hle] skip_unpack kick unpacks_skipped=%u bytes=%u\n", skippedCount, skippedBytes);
        }
    }

    void flushMovieBatch(GS *gs, PS2Memory *memory);
    void invalidateMovieSnapCache();

    void onBbChainStart(uint32_t tadr, uint32_t tagCount, const std::vector<AySpan> &spans) {
        flushMovieBatch(nullptr, nullptr);
        invalidateMovieSnapCache();
        s_movieKickMscalIndex.store(0, std::memory_order_relaxed);
        s_movieKickFirstUnpackSrc.store(0, std::memory_order_relaxed);
        s_kickDecision.store(KickDecision::Undecided, std::memory_order_relaxed);
        if (!isEnabled() && !isMovieHleActive() && !isMovieHleVerify()) return;
        ensureDumpDir();
        std::lock_guard<std::mutex> lock(s_bbMutex);
        s_bbCurrentKickIndex++;
        s_bbCurrentTadr = tadr;
        s_bbCurrentKickTagCount = tagCount;
        s_bbSpans = spans;
        if (!s_hasSeenFState9.load(std::memory_order_relaxed) && s_cachedRdram) {
            uint32_t st = 0;
            std::memcpy(&st, s_cachedRdram + (0x772188u & PS2_RAM_MASK), 4);
            if (st == 9) {
                s_hasSeenFState9.store(true, std::memory_order_relaxed);
            }
        }
        s_bbIsMovieKick = (s_hasSeenFState9.load(std::memory_order_relaxed) && tagCount > 2000);
    }

    void onBbVif1Mscal(uint32_t startPC, uint32_t itop, uint32_t top, uint32_t tops,
                       uint32_t base, uint32_t ofst, uint32_t cl, uint32_t wl, uint32_t mode) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_bbMutex);
        if (!s_hasSeenFState9.load(std::memory_order_relaxed) && s_cachedRdram) {
            uint32_t st = 0;
            std::memcpy(&st, s_cachedRdram + (0x772188u & PS2_RAM_MASK), 4);
            if (st == 9) {
                s_hasSeenFState9.store(true, std::memory_order_relaxed);
            }
        }
        if (s_hasSeenFState9.load(std::memory_order_relaxed)) {
            s_bbMscalCountAfterF9++;
        }
        s_bbLastVif1Regs = { itop, top, tops, base, ofst, cl, wl, mode };
    }

    bool isMovieHleFrameMode();

    void onBbRecordUnpack(uint32_t posPayload, uint32_t totalBytes, uint32_t vuAddr, uint8_t fmt, uint8_t num) {
        if (!isEnabled() && !isMovieHleFrameMode() && !isMovieHleVerify()) return;
        std::lock_guard<std::mutex> lock(s_bbMutex);
        uint32_t srcGuest = 0;
        const auto &spans = !s_bbSpans.empty() ? s_bbSpans : s_aySpans;
        bool isOrdered = true;
        for (size_t i = 1; i < spans.size(); ++i) {
            if (spans[i].bufOffset < spans[i - 1].bufOffset) {
                isOrdered = false;
                break;
            }
        }
        if (isOrdered && !spans.empty()) {
            auto it = std::upper_bound(spans.begin(), spans.end(), posPayload,
                                       [](uint32_t val, const AySpan &sp) {
                                           return val < sp.bufOffset;
                                       });
            if (it != spans.begin()) {
                --it;
                if (posPayload >= it->bufOffset && posPayload < it->bufOffset + it->byteCount) {
                    srcGuest = it->srcGuestAddr + (posPayload - it->bufOffset);
                }
            }
        } else {
            for (const auto &sp : spans) {
                if (posPayload >= sp.bufOffset && posPayload < sp.bufOffset + sp.byteCount) {
                    srcGuest = sp.srcGuestAddr + (posPayload - sp.bufOffset);
                    break;
                }
            }
        }
        if (fmt == 2 && num == 0 && totalBytes == 256) {
            uint32_t expected = 0;
            s_movieKickFirstUnpackSrc.compare_exchange_strong(expected, srcGuest);
        }
        if (isEnabled()) {
            s_bbUnpacksSinceLastMscal.push_back({srcGuest, totalBytes, vuAddr, fmt, num});
        }
    }

    struct VerifyWorstPoint {
        int px = 0, py = 0;
        uint8_t Y = 0, Cb = 0, Cr = 0;
        uint8_t outR = 0, outG = 0, outB = 0;
        uint8_t natR = 0, natG = 0, natB = 0;
        int err = 0;
    };

    static uint32_t s_verifyRunsCount = 0;
    static uint64_t s_verifyPointsCount = 0;
    static uint64_t s_verifyTotalDiffR = 0;
    static uint64_t s_verifyTotalDiffG = 0;
    static uint64_t s_verifyTotalDiffB = 0;
    static int s_verifyMaxR = 0;
    static int s_verifyMaxG = 0;
    static int s_verifyMaxB = 0;
    static int s_numWorstPoints = 0;
    static VerifyWorstPoint s_worstPoints[3]{};
    static auto s_verifyLastSecTime = std::chrono::steady_clock::now();

    void onBbRunStart(uint32_t startPC, const uint8_t *vuCode, uint32_t codeSize,
                      const uint8_t *vuData, uint32_t dataSize,
                      const float vf[32][4], const int32_t vi[16],
                      const float acc[4], float q, float p, float i) {
        if (!isEnabled() && !isMovieHleVerify()) return;
        ensureDumpDir();
        std::lock_guard<std::mutex> lock(s_bbMutex);

        if (!s_hasSeenFState9.load(std::memory_order_relaxed) && s_cachedRdram) {
            uint32_t st = 0;
            std::memcpy(&st, s_cachedRdram + (0x772188u & PS2_RAM_MASK), 4);
            if (st == 9) {
                s_hasSeenFState9.store(true, std::memory_order_relaxed);
            }
        }

        const bool f9 = s_hasSeenFState9.load(std::memory_order_relaxed);
        const bool movieKick = s_bbIsMovieKick;

        if (f9 && isEnabled()) {
            uint32_t n = s_bbGateDiagCount.fetch_add(1, std::memory_order_relaxed);
            if (n < 6) {
                std::printf("[probe] bb gate n=%u startPC=0x%03X f9=%d movieKick=%d dumped=%d\n",
                            n, startPC, f9 ? 1 : 0, movieKick ? 1 : 0, s_bbDumpedRunsCount);
                std::fflush(stdout);
            }
        }

        const bool gate = (isMovieHleVerify() && startPC == 0x048) || (isEnabled() && f9 && (startPC == 0x048) && (s_brightDumpedCount < 8));
        if (gate) {
            s_bbActiveRecordingRun = true;

            if (vuData && dataSize >= 16384) {
                std::memcpy(s_bbRunDataStart.data(), vuData, 16384);
            } else {
                std::memset(s_bbRunDataStart.data(), 0, 16384);
            }

            if (vf) std::memcpy(s_bbRunVf.data(), vf, 512);
            if (vi) std::memcpy(s_bbRunVi.data(), vi, 64);
            if (acc) std::memcpy(s_bbRunAcc.data(), acc, 16);
            s_bbRunQ = q;
            s_bbRunP = p;
            s_bbRunI = i;

            if (!s_vu1CodeDumped && vuCode && codeSize >= 16384) {
                std::memcpy(s_bbVuCode.data(), vuCode, 16384);
            }

            s_bbRunMscalCount = s_bbMscalCountAfterF9;
            s_bbRunKickIndex = s_bbCurrentKickIndex;
            s_bbRunTadr = s_bbCurrentTadr;
            s_bbRunVif1 = s_bbLastVif1Regs;
            s_bbRunUnpacks = s_bbUnpacksSinceLastMscal;
            s_bbRunRaw8Dst = s_bbLastRaw8Dst;
            s_bbRunGsRegs = s_bbGsRegs;
            s_bbRunDispfb1 = s_bbDispfb1;
            s_bbRunDisplay1 = s_bbDisplay1;
            s_bbRunDispfb2 = s_bbDispfb2;
            s_bbRunDisplay2 = s_bbDisplay2;

            s_bbRunXgkickPackets.clear();
        } else {
            s_bbActiveRecordingRun = false;
        }
    }

    void onBbXgkickPacket(const uint8_t *data, uint32_t byteCount) {
        if (!isEnabled() && !isMovieHleVerify()) return;
        std::lock_guard<std::mutex> lock(s_bbMutex);
        if (s_bbActiveRecordingRun && data && byteCount > 0) {
            s_bbRunXgkickPackets.emplace_back(data, data + byteCount);
        }
    }

    void onBbRunEnd(const uint8_t *vuData, uint32_t dataSize) {
        if (!isEnabled() && !isMovieHleVerify()) return;
        std::lock_guard<std::mutex> lock(s_bbMutex);

        const bool wasRecording = s_bbActiveRecordingRun;
        s_bbActiveRecordingRun = false;

        s_bbPrevMscalUnpacks = s_bbRunUnpacks;
        s_bbUnpacksSinceLastMscal.clear();

        if (isMovieHleVerify() && s_cachedRdram && wasRecording) {
            int vx0 = -1, vy0 = -1;
            for (const auto &pkt : s_bbRunXgkickPackets) {
                if (pkt.size() < 48) continue;
                size_t numVerts = (pkt.size() - 16) / 32;
                for (size_t v = 0; v < numVerts; ++v) {
                    size_t xyzOff = 16 + v * 32 + 16;
                    if (xyzOff + 4 <= pkt.size()) {
                        uint16_t x_raw = static_cast<uint16_t>(pkt[xyzOff]) | (static_cast<uint16_t>(pkt[xyzOff + 1]) << 8);
                        uint16_t y_raw = static_cast<uint16_t>(pkt[xyzOff + 2]) | (static_cast<uint16_t>(pkt[xyzOff + 3]) << 8);
                        vx0 = static_cast<int>(x_raw >> 4);
                        vy0 = static_cast<int>(y_raw >> 4);
                        break;
                    }
                }
                if (vx0 >= 0) break;
            }

            if (vx0 >= 0 && vy0 >= 0) {
                int mbx = vx0 / 16;
                int mby = vy0 / 16;
                uint32_t idx = static_cast<uint32_t>(mbx * 24 + mby);
                uint32_t base = s_movieKickFirstUnpackSrc.load(std::memory_order_relaxed);
                if (base == 0) base = s_bbLastRaw8Dst;
                uint32_t mb_base = (base + idx * 384u) & PS2_RAM_MASK;

                if (mb_base + 384u <= PS2_RAM_SIZE) {
                    const uint8_t *mbY = s_cachedRdram + mb_base;
                    const uint8_t *mbCb = s_cachedRdram + mb_base + 256u;
                    const uint8_t *mbCr = s_cachedRdram + mb_base + 320u;

                    for (const auto &pkt : s_bbRunXgkickPackets) {
                        if (pkt.size() < 48) continue;
                        size_t numVerts = (pkt.size() - 16) / 32;
                        for (size_t v = 0; v < numVerts; ++v) {
                            size_t rgbaOff = 16 + v * 32;
                            size_t xyzOff = rgbaOff + 16;
                            uint8_t outR = pkt[rgbaOff + 0];
                            uint8_t outG = pkt[rgbaOff + 1];
                            uint8_t outB = pkt[rgbaOff + 2];
                            uint16_t x_raw = static_cast<uint16_t>(pkt[xyzOff]) | (static_cast<uint16_t>(pkt[xyzOff + 1]) << 8);
                            uint16_t y_raw = static_cast<uint16_t>(pkt[xyzOff + 2]) | (static_cast<uint16_t>(pkt[xyzOff + 3]) << 8);
                            int px = (static_cast<int>(x_raw >> 4)) - vx0;
                            int py = (static_cast<int>(y_raw >> 4)) - vy0;
                            if (px >= 0 && px < 16 && py >= 0 && py < 16) {
                                uint8_t Y = mbY[py * 16 + px];
                                uint32_t ch_idx = (py / 2) * 8 + (px / 2);
                                uint8_t Cb = mbCb[ch_idx];
                                uint8_t Cr = mbCr[ch_idx];

                                float y_f = static_cast<float>(Y);
                                float cb_f = static_cast<float>(static_cast<int>(Cb) - 128);
                                float cr_f = static_cast<float>(static_cast<int>(Cr) - 128);

                                float r_f = y_f + 1.402f * cr_f;
                                float g_f = y_f - 0.344136f * cb_f - 0.714136f * cr_f;
                                float b_f = y_f + 1.772f * cb_f;

                                uint8_t natR = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(r_f)), 0, 255));
                                uint8_t natG = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(g_f)), 0, 255));
                                uint8_t natB = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(b_f)), 0, 255));

                                int diffR = std::abs((int)outR - (int)natR);
                                int diffG = std::abs((int)outG - (int)natG);
                                int diffB = std::abs((int)outB - (int)natB);

                                s_verifyPointsCount++;
                                s_verifyTotalDiffR += diffR;
                                s_verifyTotalDiffG += diffG;
                                s_verifyTotalDiffB += diffB;
                                if (diffR > s_verifyMaxR) s_verifyMaxR = diffR;
                                if (diffG > s_verifyMaxG) s_verifyMaxG = diffG;
                                if (diffB > s_verifyMaxB) s_verifyMaxB = diffB;

                                int totalErr = diffR + diffG + diffB;
                                if (s_numWorstPoints < 3 || totalErr > s_worstPoints[2].err) {
                                    VerifyWorstPoint wp{px, py, Y, Cb, Cr, outR, outG, outB, natR, natG, natB, totalErr};
                                    int insertPos = std::min(s_numWorstPoints, 2);
                                    while (insertPos > 0 && totalErr > s_worstPoints[insertPos - 1].err) {
                                        if (insertPos < 3) s_worstPoints[insertPos] = s_worstPoints[insertPos - 1];
                                        insertPos--;
                                    }
                                    s_worstPoints[insertPos] = wp;
                                    if (s_numWorstPoints < 3) s_numWorstPoints++;
                                }
                            }
                        }
                    }
                    s_verifyRunsCount++;
                }
            }

            auto now = std::chrono::steady_clock::now();
            double secDiff = std::chrono::duration<double>(now - s_verifyLastSecTime).count();
            if (secDiff >= 1.0 && s_verifyPointsCount > 0) {
                double meanR = (double)s_verifyTotalDiffR / s_verifyPointsCount;
                double meanG = (double)s_verifyTotalDiffG / s_verifyPointsCount;
                double meanB = (double)s_verifyTotalDiffB / s_verifyPointsCount;
                std::printf("[probe] bf verify runs=%u meanabs=%.2f,%.2f,%.2f max=%d,%d,%d over %llu\n",
                            s_verifyRunsCount, meanR, meanG, meanB,
                            s_verifyMaxR, s_verifyMaxG, s_verifyMaxB,
                            (unsigned long long)s_verifyPointsCount);
                for (int i = 0; i < s_numWorstPoints; ++i) {
                    const auto &w = s_worstPoints[i];
                    std::printf("  worst#%d: px=%d py=%d Y=%u Cb=%u Cr=%u out=(%u,%u,%u) native=(%u,%u,%u)\n",
                                i + 1, w.px, w.py, w.Y, w.Cb, w.Cr,
                                w.outR, w.outG, w.outB, w.natR, w.natG, w.natB);
                }
                std::fflush(stdout);

                s_verifyRunsCount = 0;
                s_verifyPointsCount = 0;
                s_verifyTotalDiffR = s_verifyTotalDiffG = s_verifyTotalDiffB = 0;
                s_verifyMaxR = s_verifyMaxG = s_verifyMaxB = 0;
                s_numWorstPoints = 0;
                s_verifyLastSecTime = now;
            }
        }

        if (!isEnabled()) return;
        if (!wasRecording) return;

        // Compute max(R,G,B) over the 256 emitted points and find (x0, y0)
        uint8_t maxColor = 0;
        int x0 = -1, y0 = -1;
        for (const auto &pkt : s_bbRunXgkickPackets) {
            if (pkt.size() < 48) continue;
            size_t numVerts = (pkt.size() - 16) / 32;
            for (size_t v = 0; v < numVerts; ++v) {
                size_t rgbaOff = 16 + v * 32;
                size_t xyzOff = rgbaOff + 16;
                uint8_t r = pkt[rgbaOff + 0];
                uint8_t g = pkt[rgbaOff + 1];
                uint8_t b = pkt[rgbaOff + 2];
                if (r > maxColor) maxColor = r;
                if (g > maxColor) maxColor = g;
                if (b > maxColor) maxColor = b;
                if (x0 < 0 && xyzOff + 4 <= pkt.size()) {
                    uint16_t x_raw = static_cast<uint16_t>(pkt[xyzOff]) | (static_cast<uint16_t>(pkt[xyzOff + 1]) << 8);
                    uint16_t y_raw = static_cast<uint16_t>(pkt[xyzOff + 2]) | (static_cast<uint16_t>(pkt[xyzOff + 3]) << 8);
                    x0 = static_cast<int>(x_raw >> 4);
                    y0 = static_cast<int>(y_raw >> 4);
                }
            }
        }

        const bool canDump = (s_brightDumpedCount < 8) &&
                             (s_brightDumpedCount == 0 || s_bbRunMscalCount >= s_lastBrightMscal + 25);

        if (maxColor >= 64 && canDump) {
            int k = s_brightDumpedCount++;
            s_lastBrightMscal = s_bbRunMscalCount;

            std::printf("[probe] bb bright k=%d mscal#=%u max=%u x0=%d y0=%d\n",
                        k, s_bbRunMscalCount, (unsigned)maxColor, x0, y0);
            std::fflush(stdout);

            std::vector<uint8_t> runDataEnd(16384, 0);
            if (vuData && dataSize >= 16384) {
                std::memcpy(runDataEnd.data(), vuData, 16384);
            }

            std::error_code ec;
            std::filesystem::create_directories("logs/vu1dump", ec);

            if (!s_vu1CodeDumped) {
                std::ofstream codeOut("logs/vu1dump/vu1code.bin", std::ios::binary);
                if (codeOut) {
                    codeOut.write(reinterpret_cast<const char*>(s_bbVuCode.data()), 16384);
                    codeOut.flush();
                    codeOut.close();
                    s_vu1CodeDumped = true;
                }
            }

            size_t binTotalBytes = 0;
            char binPath[64];
            std::snprintf(binPath, sizeof(binPath), "logs/vu1dump/bright%d.bin", k);
            {
                std::ofstream binOut(binPath, std::ios::binary);
                if (binOut) {
                    binOut.write(reinterpret_cast<const char*>(s_bbRunDataStart.data()), 16384);
                    binOut.write(reinterpret_cast<const char*>(runDataEnd.data()), 16384);
                    binOut.write(reinterpret_cast<const char*>(s_bbRunVf.data()), 512);
                    binOut.write(reinterpret_cast<const char*>(s_bbRunVi.data()), 64);
                    binOut.write(reinterpret_cast<const char*>(s_bbRunAcc.data()), 16);
                    binOut.write(reinterpret_cast<const char*>(&s_bbRunQ), 4);
                    binOut.write(reinterpret_cast<const char*>(&s_bbRunP), 4);
                    binOut.write(reinterpret_cast<const char*>(&s_bbRunI), 4);
                    binTotalBytes = 16384 + 16384 + 512 + 64 + 16 + 4 + 4 + 4;
                    for (const auto &pkt : s_bbRunXgkickPackets) {
                        uint32_t count = static_cast<uint32_t>(pkt.size());
                        binOut.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
                        binTotalBytes += sizeof(uint32_t);
                        if (count > 0) {
                            binOut.write(reinterpret_cast<const char*>(pkt.data()), count);
                            binTotalBytes += count;
                        }
                    }
                    binOut.flush();
                    binOut.close();
                }
            }

            char txtPath[64];
            std::snprintf(txtPath, sizeof(txtPath), "logs/vu1dump/bright%d.txt", k);
            {
                std::ofstream txtOut(txtPath);
                if (txtOut) {
                    txtOut << "=== Bright Run " << k << " Header ===\n";
                    txtOut << "MSCAL count since F state 9: " << s_bbRunMscalCount << "\n";
                    txtOut << "Kick index: " << s_bbRunKickIndex << "\n";
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "0x%08X", s_bbRunTadr);
                    txtOut << "tadr: " << buf << "\n";
                    txtOut << "VIF1 regs:\n";
                    std::snprintf(buf, sizeof(buf), "  ITOP=0x%03X TOP=0x%03X TOPS=0x%03X BASE=0x%03X OFST=0x%03X CL=%u WL=%u mode=%u\n",
                                  s_bbRunVif1.itop, s_bbRunVif1.top, s_bbRunVif1.tops,
                                  s_bbRunVif1.base, s_bbRunVif1.ofst,
                                  s_bbRunVif1.cl, s_bbRunVif1.wl, s_bbRunVif1.mode);
                    txtOut << buf;

                    std::snprintf(buf, sizeof(buf), "First RAW8 destination buffer address (last a1): 0x%08X\n", s_bbRunRaw8Dst);
                    txtOut << buf;

                    int mbCol = (x0 >= 0) ? (x0 / 16) : -1;
                    int mbRow = (y0 >= 0) ? (y0 / 16) : -1;
                    txtOut << "MB: col=" << mbCol << " row=" << mbRow << " (x0=" << x0 << " y0=" << y0 << ") maxRGB=" << (unsigned)maxColor << "\n";

                    auto findYCbCr = [](const std::vector<BbUnpackInfo> &list, uint32_t &y, uint32_t &cb, uint32_t &cr) {
                        y = 0; cb = 0; cr = 0;
                        for (const auto &up : list) {
                            if (up.len == 256 && y == 0) y = up.srcGuest;
                            else if (up.len == 64 && cb == 0) cb = up.srcGuest;
                            else if (up.len == 64 && cr == 0) cr = up.srcGuest;
                        }
                    };

                    uint32_t curY = 0, curCb = 0, curCr = 0;
                    findYCbCr(s_bbRunUnpacks, curY, curCb, curCr);

                    uint32_t prevY = 0, prevCb = 0, prevCr = 0;
                    findYCbCr(s_bbPrevMscalUnpacks, prevY, prevCb, prevCr);

                    auto hexDump = [&](uint32_t guestAddr, size_t count) -> std::string {
                        std::string h;
                        if (!s_cachedRdram || (guestAddr & PS2_RAM_MASK) + count > PS2_RAM_SIZE || guestAddr == 0) {
                            return "<invalid or unmapped>";
                        }
                        const uint8_t *ptr = s_cachedRdram + (guestAddr & PS2_RAM_MASK);
                        char hbuf[4];
                        for (size_t i = 0; i < count; ++i) {
                            std::snprintf(hbuf, sizeof(hbuf), "%02x", ptr[i]);
                            h += hbuf;
                        }
                        return h;
                    };

                    txtOut << "Consumed YCbCr MB (current MSCAL UNPACKs):\n";
                    std::snprintf(buf, sizeof(buf), "  srcY=0x%08X (256 B), srcCb=0x%08X (64 B), srcCr=0x%08X (64 B)\n", curY, curCb, curCr);
                    txtOut << buf;
                    txtOut << "Raw Y (256 B hex):\n  " << hexDump(curY, 256) << "\n";
                    txtOut << "Raw Cb (64 B hex):\n  " << hexDump(curCb, 64) << "\n";
                    txtOut << "Raw Cr (64 B hex):\n  " << hexDump(curCr, 64) << "\n";

                    if (prevY != 0 || prevCb != 0 || prevCr != 0) {
                        txtOut << "Consumed YCbCr MB (previous MSCAL UNPACKs, if double-buffered):\n";
                        std::snprintf(buf, sizeof(buf), "  srcY=0x%08X (256 B), srcCb=0x%08X (64 B), srcCr=0x%08X (64 B)\n", prevY, prevCb, prevCr);
                        txtOut << buf;
                        txtOut << "Raw Y (256 B hex):\n  " << hexDump(prevY, 256) << "\n";
                        txtOut << "Raw Cb (64 B hex):\n  " << hexDump(prevCb, 64) << "\n";
                        txtOut << "Raw Cr (64 B hex):\n  " << hexDump(prevCr, 64) << "\n";
                    }

                    txtOut << "UNPACKs for this MSCAL (count=" << s_bbRunUnpacks.size() << "):\n";
                    for (const auto &up : s_bbRunUnpacks) {
                        std::snprintf(buf, sizeof(buf), "  src=0x%08X len=%u vuaddr=0x%03X fmt=%u num=%u\n",
                                      up.srcGuest, up.len, up.vuAddr, (unsigned)up.fmt, (unsigned)up.num);
                        txtOut << buf;
                    }

                    txtOut << "UNPACKs for previous MSCAL (count=" << s_bbPrevMscalUnpacks.size() << "):\n";
                    for (const auto &up : s_bbPrevMscalUnpacks) {
                        std::snprintf(buf, sizeof(buf), "  src=0x%08X len=%u vuaddr=0x%03X fmt=%u num=%u\n",
                                      up.srcGuest, up.len, up.vuAddr, (unsigned)up.fmt, (unsigned)up.num);
                        txtOut << buf;
                    }

                    txtOut << "GS Registers:\n";
                    auto printGs = [&](const char *name, uint64_t val) {
                        char gbuf[128];
                        std::snprintf(gbuf, sizeof(gbuf), "  %-12s: 0x%016llX\n", name, (unsigned long long)val);
                        txtOut << gbuf;
                    };
                    auto printGsPsm = [&](const char *name, uint64_t val) {
                        char gbuf[128];
                        uint32_t psm = static_cast<uint32_t>((val >> 24) & 0x3F);
                        std::snprintf(gbuf, sizeof(gbuf), "  %-12s: 0x%016llX (PSM=0x%02X)\n", name, (unsigned long long)val, psm);
                        txtOut << gbuf;
                    };

                    printGsPsm("FRAME_1", s_bbRunGsRegs[0x4C]);
                    printGsPsm("FRAME_2", s_bbRunGsRegs[0x4D]);
                    printGs("ZBUF_1", s_bbRunGsRegs[0x4E]);
                    printGs("ZBUF_2", s_bbRunGsRegs[0x4F]);
                    printGs("XYOFFSET_1", s_bbRunGsRegs[0x18]);
                    printGs("XYOFFSET_2", s_bbRunGsRegs[0x19]);
                    printGs("SCISSOR_1", s_bbRunGsRegs[0x40]);
                    printGs("SCISSOR_2", s_bbRunGsRegs[0x41]);
                    printGs("TEST_1", s_bbRunGsRegs[0x47]);
                    printGs("TEST_2", s_bbRunGsRegs[0x48]);
                    printGs("ALPHA_1", s_bbRunGsRegs[0x42]);
                    printGs("ALPHA_2", s_bbRunGsRegs[0x43]);
                    printGs("PRIM", s_bbRunGsRegs[0x00]);
                    printGs("PRMODECONT", s_bbRunGsRegs[0x1A]);
                    printGs("COLCLAMP", s_bbRunGsRegs[0x46]);
                    printGs("DTHE", s_bbRunGsRegs[0x45]);
                    printGs("PABE", s_bbRunGsRegs[0x49]);
                    printGs("FBA_1", s_bbRunGsRegs[0x4A]);
                    printGs("FBA_2", s_bbRunGsRegs[0x4B]);
                    printGs("TEX0_1", s_bbRunGsRegs[0x06]);
                    printGs("TEX0_2", s_bbRunGsRegs[0x07]);
                    printGs("TEXA", s_bbRunGsRegs[0x3B]);
                    printGs("FOGCOL", s_bbRunGsRegs[0x3D]);
                    printGs("DISPFB1", s_bbRunDispfb1);
                    printGs("DISPLAY1", s_bbRunDisplay1);
                    printGs("DISPFB2", s_bbRunDispfb2);
                    printGs("DISPLAY2", s_bbRunDisplay2);
                    txtOut.flush();
                    txtOut.close();
                }
            }

            std::printf("[probe] bb dumped bright%d bytes=%zu\n", k, binTotalBytes);
            std::fflush(stdout);
        }
    }

    void onGsWriteRegister(uint8_t regAddr, uint64_t value) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_bbMutex);
        if (regAddr < s_bbGsRegs.size()) {
            s_bbGsRegs[regAddr] = value;
        }
        if (regAddr == 0x59) s_bbDispfb1 = value;
        else if (regAddr == 0x5A) s_bbDisplay1 = value;
        else if (regAddr == 0x5B) s_bbDispfb2 = value;
        else if (regAddr == 0x5C) s_bbDisplay2 = value;
    }

    void onGsPrivRegister(uint32_t addr, uint64_t value) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_bbMutex);
        uint32_t off = addr & 0x1FFF;
        if (off == 0x70) s_bbDispfb1 = value;
        else if (off == 0x80) s_bbDisplay1 = value;
        else if (off == 0x90) s_bbDispfb2 = value;
        else if (off == 0xA0) s_bbDisplay2 = value;
    }

    void onBbCheckRaw8(bool validFrame, uint32_t numMbs, uint32_t dst, const uint8_t *rdram) {
        if (!isEnabled() && !isMovieHleActive() && !isMovieHleVerify()) return;
        if (rdram) s_cachedRdram = rdram;
        if (validFrame) {
            s_hasSeenFState9.store(true, std::memory_order_relaxed);
            s_lastRaw8ValidTime = std::chrono::steady_clock::now();
            s_hasSeenValidRaw8.store(true, std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> lock(s_bbMutex);
        s_bbLastRaw8Dst = dst;
        if (!isEnabled()) return;

        const uint32_t checkBytes = std::min(numMbs * 384u, 294912u);
        uint32_t dstOffset = dst & PS2_RAM_MASK;
        if (dstOffset + checkBytes > PS2_RAM_SIZE) return;

        const uint8_t *src = rdram + dstOffset;
        uint32_t nonBlackPixels = 0;
        for (uint32_t mb = 0; mb < numMbs && mb < 768; ++mb) {
            const uint8_t *mbPtr = src + mb * 384u;
            for (uint32_t i = 0; i < 256; ++i) {
                if (mbPtr[i] > 20 && mbPtr[i] != 128) {
                    nonBlackPixels++;
                }
            }
        }

        if (nonBlackPixels > 100) {
            std::error_code ec;
            std::filesystem::create_directories("logs/vu1dump", ec);
            const uint32_t dumpSize = 768u * 384u; // 294,912 B
            if (dstOffset + dumpSize <= PS2_RAM_SIZE) {
                std::ofstream out("logs/vu1dump/frame0_ycc.bin", std::ios::binary);
                if (out) {
                    out.write(reinterpret_cast<const char*>(src), dumpSize);
                    out.close();
                    s_bbDumpedFrame0Ycc = true;
                    std::printf("[probe] bb dumped frame0_ycc.bin size=%u nonBlack=%u dst=0x%08X\n",
                                dumpSize, nonBlackPixels, dst);
                    std::fflush(stdout);
                }
            }
        }
    }

    // Task BG: Movie YCbCr->RGB VU1 Native HLE with ordered GIF writes
    bool isMovieHleActive() {
        static int s_envHle = -1;
        if (s_envHle == -1) {
            const char *env = std::getenv("PS2X_MOVIE_HLE");
            s_envHle = (env && env[0] == '0') ? 0 : 1; // default ON (1)
        }
        return s_envHle == 1;
    }

    bool isMovieHleVerify() {
        static int s_envVerify = -1;
        if (s_envVerify == -1) {
            const char *env = std::getenv("PS2X_MOVIE_HLE_VERIFY");
            s_envVerify = (env && (env[0] == '1' || env[0] == 't')) ? 1 : 0; // default OFF (0)
        }
        return s_envVerify == 1;
    }

    static bool isMovieHleStrict() {
        static int s_envStrict = -1;
        if (s_envStrict == -1) {
            const char *env = std::getenv("PS2X_MOVIE_HLE_STRICT");
            s_envStrict = (env && (env[0] == '1' || env[0] == 't')) ? 1 : 0; // default OFF (0)
        }
        return s_envStrict == 1;
    }

    static std::atomic<bool> s_bhMovieActiveLogged{false};

    static bool isMovieActive(const uint8_t *rdram) {
        const uint8_t *r = rdram ? rdram : s_cachedRdram;
        const char *reason = nullptr;
        if (r) {
            uint32_t st = 0;
            std::memcpy(&st, r + (0x772188u & PS2_RAM_MASK), 4);
            if (st == 9) {
                reason = "f_state9";
                s_hasSeenFState9.store(true, std::memory_order_relaxed);
            }
        }
        if (!reason && s_hasSeenValidRaw8.load(std::memory_order_relaxed)) {
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - s_lastRaw8ValidTime).count();
            if (secs < 3.0) {
                reason = "raw8_have";
            }
        }

        if (reason != nullptr) {
            bool expected = false;
            if (s_bhMovieActiveLogged.compare_exchange_strong(expected, true)) {
                bhLog("[probe] bh movie_active reason=%s\n", reason);
            }
            return true;
        }
        return false;
    }

    static uint32_t s_secNativeKicks = 0;
    static uint32_t s_secInterpretedKicks = 0;
    static uint32_t s_secMixedKicks = 0;
    static double s_secNativeMsTotal = 0.0;
    static double s_secXferMsTotal = 0.0;
    static std::map<uint64_t, uint32_t> s_sigCountsThisSec;
    static auto s_bgProbeLastSec = std::chrono::steady_clock::now();
    static std::set<std::string> s_reportedInterpretReasons;

    static uint32_t s_movieWidth = 512;
    static uint32_t s_movieHeight = 384;
    static uint32_t s_movieMbCols = 32;
    static uint32_t s_movieMbRows = 24;
    static uint32_t s_movieTotalMbs = 768;
    static uint32_t s_movieInstance = 0;
    static uint32_t s_lastPrintedMovieInstance = 0;
    static uint32_t s_lastPrintedW = 0;
    static uint32_t s_lastPrintedH = 0;
    static std::atomic<uint32_t> s_kickMscalCount{0};

    void onMovieCreated() {
        s_movieInstance++;
    }

    void onMovieRaw8Geometry(uint32_t width, uint32_t height, uint32_t mbCols, uint32_t mbRows) {
        s_movieWidth = width;
        s_movieHeight = height;
        s_movieMbCols = mbCols;
        s_movieMbRows = mbRows;
        s_movieTotalMbs = mbCols * mbRows;

        if (s_movieInstance != s_lastPrintedMovieInstance || width != s_lastPrintedW || height != s_lastPrintedH) {
            s_lastPrintedMovieInstance = s_movieInstance;
            s_lastPrintedW = width;
            s_lastPrintedH = height;
            bhLog("[hle] movie geometry w=%u h=%u cols=%u rows=%u mbs=%u\n",
                  width, height, mbCols, mbRows, s_movieTotalMbs);
            ps2xSafeLog("[hle] movie geometry w=%u h=%u cols=%u rows=%u mbs=%u\n",
                        width, height, mbCols, mbRows, s_movieTotalMbs);
        }
    }

    void onMovieKickMscalCount(uint32_t count) {
        s_kickMscalCount.store(count, std::memory_order_relaxed);
    }

    static void doMovieFrameTransfer(GS &gs, PS2Memory *memory, uint32_t base, double &outNativeMs, double &outXferMs) {
        const uint8_t *rdram = memory ? memory->getRDRAM() : s_cachedRdram;
        if (!rdram) {
            outNativeMs = 0.0;
            outXferMs = 0.0;
            return;
        }

        auto t0 = std::chrono::steady_clock::now();

        GSDebugSnapshot snap = gs.getDebugSnapshot();
        const auto &ctx = snap.ctx[1]; // Context 2
        const uint32_t fbp = ctx.frame.fbp << 5u; // DBP = FBP*32
        const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        const uint32_t dpsm = ctx.frame.psm & 0x3Fu;

        uint32_t scissorW = (ctx.scissor.x1 >= ctx.scissor.x0) ? (ctx.scissor.x1 - ctx.scissor.x0 + 1u) : s_movieWidth;
        uint32_t scissorH = (ctx.scissor.y1 >= ctx.scissor.y0) ? (ctx.scissor.y1 - ctx.scissor.y0 + 1u) : (s_movieMbRows * 16u);

        uint32_t targetW = std::min(s_movieWidth, scissorW);
        uint32_t targetH = std::min(s_movieMbRows * 16u, scissorH);

        static std::vector<uint8_t> s_pkt1;
        static std::vector<uint8_t> s_pkt2;
        static std::vector<uint8_t> s_pktRestore(80, 0);

        auto writeSetupPacket = [](std::vector<uint8_t> &pkt, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir, uint32_t imageNloop) {
            uint64_t tagLo = 4ull | (0ull << 15u) | (0ull << 58u) | (1ull << 60u);
            uint64_t tagHi = 0x0Eull;
            std::memcpy(pkt.data(), &tagLo, 8);
            std::memcpy(pkt.data() + 8, &tagHi, 8);

            uint64_t regAddr = 0x50ull;
            std::memcpy(pkt.data() + 16, &bitbltbuf, 8);
            std::memcpy(pkt.data() + 24, &regAddr, 8);

            regAddr = 0x51ull;
            std::memcpy(pkt.data() + 32, &trxpos, 8);
            std::memcpy(pkt.data() + 40, &regAddr, 8);

            regAddr = 0x52ull;
            std::memcpy(pkt.data() + 48, &trxreg, 8);
            std::memcpy(pkt.data() + 56, &regAddr, 8);

            regAddr = 0x53ull;
            std::memcpy(pkt.data() + 64, &trxdir, 8);
            std::memcpy(pkt.data() + 72, &regAddr, 8);

            uint64_t imgLo = static_cast<uint64_t>(imageNloop) | (1ull << 15u /*EOP*/) | (2ull << 58u /*FLG=IMAGE*/);
            uint64_t imgHi = 0ull;
            std::memcpy(pkt.data() + 80, &imgLo, 8);
            std::memcpy(pkt.data() + 88, &imgHi, 8);
        };

        const uint64_t bitbltbuf_val = (static_cast<uint64_t>(fbp & 0x3FFFu) << 32u) |
                                       (static_cast<uint64_t>(fbw & 0x3Fu) << 48u) |
                                       (static_cast<uint64_t>(dpsm & 0x3Fu) << 56u);
        const uint64_t trxdir_val = 0ull;

        uint32_t pkt1_h = std::min(targetH, 192u);
        s_pkt1.resize(96 + targetW * pkt1_h * 4u);
        const uint64_t trxreg1_val = static_cast<uint64_t>(targetW) | (static_cast<uint64_t>(pkt1_h) << 32u);
        writeSetupPacket(s_pkt1, bitbltbuf_val, 0ull, trxreg1_val, trxdir_val, (targetW * pkt1_h) / 4u);

        uint32_t pkt2_h = (targetH > 192u) ? (targetH - 192u) : 0u;
        if (pkt2_h > 0) {
            s_pkt2.resize(96 + targetW * pkt2_h * 4u);
            const uint64_t trxpos2_val = (static_cast<uint64_t>(192 & 0x7FFu) << 48u);
            const uint64_t trxreg2_val = static_cast<uint64_t>(targetW) | (static_cast<uint64_t>(pkt2_h) << 32u);
            writeSetupPacket(s_pkt2, bitbltbuf_val, trxpos2_val, trxreg2_val, trxdir_val, (targetW * pkt2_h) / 4u);
        }

        uint32_t *pkt1_pixels = reinterpret_cast<uint32_t*>(s_pkt1.data() + 96);
        uint32_t *pkt2_pixels = (pkt2_h > 0) ? reinterpret_cast<uint32_t*>(s_pkt2.data() + 96) : nullptr;

        // Native YCbCr -> RGB conversion directly into packet payloads
        for (uint32_t k = 0; k < s_movieTotalMbs; ++k) {
            const uint32_t mbx = k / s_movieMbRows;
            const uint32_t mby = k % s_movieMbRows;
            const uint32_t mb_base = (base + k * 384u) & PS2_RAM_MASK;
            if (mb_base + 384u > PS2_RAM_SIZE) continue;

            const uint8_t *mbY = rdram + mb_base;
            const uint8_t *mbCb = rdram + mb_base + 256u;
            const uint8_t *mbCr = rdram + mb_base + 320u;

            const uint32_t startX = mbx * 16u;
            const uint32_t startY = mby * 16u;
            if (startX >= targetW) continue;

            for (uint32_t py = 0; py < 16u; ++py) {
                const uint32_t y = startY + py;
                if (y >= targetH) continue;
                const uint32_t py_chroma = (py >> 1u) * 8u;

                uint32_t *rowDst = (y < 192u) ? (pkt1_pixels + y * targetW + startX)
                                              : (pkt2_pixels ? (pkt2_pixels + (y - 192u) * targetW + startX) : nullptr);
                if (!rowDst) continue;

                for (uint32_t px = 0; px < 16u; ++px) {
                    if (startX + px >= targetW) continue;
                    const uint8_t y_val = mbY[py * 16u + px];
                    const uint32_t ch_idx = py_chroma + (px >> 1u);
                    const int cb_val = static_cast<int>(mbCb[ch_idx]) - 128;
                    const int cr_val = static_cast<int>(mbCr[ch_idx]) - 128;

                    const float y_f = static_cast<float>(y_val);
                    const float cb_f = static_cast<float>(cb_val);
                    const float cr_f = static_cast<float>(cr_val);

                    const float r_f = y_f + 1.402f * cr_f;
                    const float g_f = y_f - 0.344136f * cb_f - 0.714136f * cr_f;
                    const float b_f = y_f + 1.772f * cb_f;

                    const int r_i = static_cast<int>(std::round(r_f));
                    const int g_i = static_cast<int>(std::round(g_f));
                    const int b_i = static_cast<int>(std::round(b_f));

                    const uint32_t r = static_cast<uint32_t>(std::clamp(r_i, 0, 255));
                    const uint32_t g = static_cast<uint32_t>(std::clamp(g_i, 0, 255));
                    const uint32_t b = static_cast<uint32_t>(std::clamp(b_i, 0, 255));
                    const uint32_t a = 0u;

                    rowDst[px] = r | (g << 8u) | (b << 16u) | (a << 24u);
                }
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        outNativeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Packet 3: restore original GS registers
        {
            uint64_t tagLo = 4ull | (1ull << 15u /*EOP*/) | (0ull << 58u) | (1ull << 60u);
            uint64_t tagHi = 0x0Eull;
            std::memcpy(s_pktRestore.data(), &tagLo, 8);
            std::memcpy(s_pktRestore.data() + 8, &tagHi, 8);

            uint64_t orig_bitbltbuf = (static_cast<uint64_t>(snap.bitbltbuf.sbp & 0x3FFFu)) |
                                      (static_cast<uint64_t>(snap.bitbltbuf.sbw & 0x3Fu) << 16u) |
                                      (static_cast<uint64_t>(snap.bitbltbuf.spsm & 0x3Fu) << 24u) |
                                      (static_cast<uint64_t>(snap.bitbltbuf.dbp & 0x3FFFu) << 32u) |
                                      (static_cast<uint64_t>(snap.bitbltbuf.dbw & 0x3Fu) << 48u) |
                                      (static_cast<uint64_t>(snap.bitbltbuf.dpsm & 0x3Fu) << 56u);
            uint64_t orig_trxpos = (static_cast<uint64_t>(snap.trxpos.ssax & 0x7FFu)) |
                                   (static_cast<uint64_t>(snap.trxpos.ssay & 0x7FFu) << 16u) |
                                   (static_cast<uint64_t>(snap.trxpos.dsax & 0x7FFu) << 32u) |
                                   (static_cast<uint64_t>(snap.trxpos.dsay & 0x7FFu) << 48u) |
                                   (static_cast<uint64_t>(snap.trxpos.dir & 0x3u) << 59u);
            uint64_t orig_trxreg = (static_cast<uint64_t>(snap.trxreg.rrw & 0xFFFu)) |
                                   (static_cast<uint64_t>(snap.trxreg.rrh & 0xFFFu) << 32u);
            uint64_t orig_trxdir = static_cast<uint64_t>(snap.trxdir & 0x3u);

            uint64_t reg = 0x50ull;
            std::memcpy(s_pktRestore.data() + 16, &orig_bitbltbuf, 8);
            std::memcpy(s_pktRestore.data() + 24, &reg, 8);

            reg = 0x51ull;
            std::memcpy(s_pktRestore.data() + 32, &orig_trxpos, 8);
            std::memcpy(s_pktRestore.data() + 40, &reg, 8);

            reg = 0x52ull;
            std::memcpy(s_pktRestore.data() + 48, &orig_trxreg, 8);
            std::memcpy(s_pktRestore.data() + 56, &reg, 8);

            reg = 0x53ull;
            std::memcpy(s_pktRestore.data() + 64, &orig_trxdir, 8);
            std::memcpy(s_pktRestore.data() + 72, &reg, 8);
        }

        // Submit through Path1 in-order with surrounding draws
        if (memory) {
            memory->submitGifPacket(GifPathId::Path1, s_pkt1.data(), static_cast<uint32_t>(s_pkt1.size()));
            if (pkt2_h > 0) {
                memory->submitGifPacket(GifPathId::Path1, s_pkt2.data(), static_cast<uint32_t>(s_pkt2.size()));
            }
            memory->submitGifPacket(GifPathId::Path1, s_pktRestore.data(), static_cast<uint32_t>(s_pktRestore.size()));
        } else {
            gs.processGIFPacket(s_pkt1.data(), static_cast<uint32_t>(s_pkt1.size()));
            if (pkt2_h > 0) {
                gs.processGIFPacket(s_pkt2.data(), static_cast<uint32_t>(s_pkt2.size()));
            }
            gs.processGIFPacket(s_pktRestore.data(), static_cast<uint32_t>(s_pktRestore.size()));
        }

        auto t2 = std::chrono::steady_clock::now();
        outXferMs = std::chrono::duration<double, std::milli>(t2 - t1).count();

        static uint32_t s_frameCounter = 0;
        static uint32_t s_secFrameCount = 0;
        static double s_secTotalMs = 0.0;
        static auto s_lastSecLog = std::chrono::steady_clock::now();

        uint32_t frameNum = s_frameCounter++;
        s_secFrameCount++;
        s_secTotalMs += (outNativeMs + outXferMs);

        if (frameNum < 5u) {
            std::printf("[hle] movie conversion native base=0x%08X frame#=%u ms=%.2f\n",
                        base, frameNum, outNativeMs + outXferMs);
            std::fflush(stdout);
        } else {
            auto now = std::chrono::steady_clock::now();
            double secDiff = std::chrono::duration<double>(now - s_lastSecLog).count();
            if (secDiff >= 1.0) {
                double avgMs = s_secFrameCount > 0 ? (s_secTotalMs / s_secFrameCount) : 0.0;
                std::printf("[hle] movie conversion native frames=%u avg_ms=%.2f\n",
                            s_secFrameCount, avgMs);
                std::fflush(stdout);
                s_secFrameCount = 0;
                s_secTotalMs = 0.0;
                s_lastSecLog = now;
            }
        }
    }

    bool isMovieHleFrameMode() {
        static int s_envFrameMode = -1;
        if (s_envFrameMode == -1) {
            const char *env = std::getenv("PS2X_MOVIE_HLE_MODE");
            s_envFrameMode = (env && std::strcmp(env, "frame") == 0) ? 1 : 0;
        }
        return s_envFrameMode == 1;
    }

    struct MovieSnapshotCache {
        bool valid = false;
        uint32_t scisX0 = 0;
        uint32_t scisX1 = 0;
        uint32_t scisY0 = 0;
        uint32_t scisY1 = 0;
        uint32_t fbp = 0;
        uint32_t fbw = 0;
        uint32_t dpsm = 0;
    };
    static MovieSnapshotCache s_movieSnapCache;
    void invalidateMovieSnapCache() {
        s_movieSnapCache.valid = false;
    }

    static std::atomic<uint32_t> s_secBsMscals{0};
    static std::atomic<uint32_t> s_secBsBlocksClipped{0};
    static std::atomic<uint64_t> s_secBsTimeUs{0};
    static std::atomic<uint32_t> s_secDaFlushes{0};
    static std::atomic<uint32_t> s_secDaBlocks{0};
    static std::atomic<uint32_t> s_secDaMaxBlocks{0};
    static std::atomic<uint32_t> s_secDaSnapshotReads{0};
    static auto s_bsProbeLastSec = std::chrono::steady_clock::now();
    static uint32_t s_lastBsMovieInstance = 0xFFFFFFFFu;

    static std::vector<uint8_t> s_movieBatch;
    static uint32_t s_movieBatchBlocks = 0;
    static GS *s_batchGs = nullptr;
    static PS2Memory *s_batchMemory = nullptr;
    static uint32_t s_lastFrameFbp = 0;
    static uint32_t s_kickMscalsExecuted = 0;
    static uint32_t s_daVerifyFrameNum = 0;

    static bool isMovieBatchEnabled() {
        static int s_batchEnv = -1;
        if (s_batchEnv == -1) {
            const char *env = std::getenv("PS2X_MOVIE_BATCH");
            s_batchEnv = (env && (std::strcmp(env, "0") == 0 || std::strcmp(env, "false") == 0)) ? 0 : 1;
        }
        return s_batchEnv == 1;
    }

    static bool isMovieBatchVerifyEnabled() {
        static int s_verifyEnv = -1;
        if (s_verifyEnv == -1) {
            const char *env = std::getenv("PS2X_MOVIE_BATCH_VERIFY");
            s_verifyEnv = (env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0)) ? 1 : 0;
        }
        return s_verifyEnv == 1;
    }

    static void verifyDaFrameHash(PS2Memory *mem) {
        if (!mem) return;
        const uint8_t *vram = mem->getGSVRAM();
        if (!vram) return;
        const uint32_t fbp = s_lastFrameFbp;
        const uint32_t baseOffset = fbp * 2048u * 4u;
        uint32_t frameH = 384u;
        if (baseOffset + 512u * frameH * 4u > 4u * 1024u * 1024u) {
            frameH = 320u;
        }
        if (baseOffset + 512u * frameH * 4u <= 4u * 1024u * 1024u) {
            const uint32_t frameBytes = 512u * frameH * 4u;
            uint64_t h = 0xCBF29CE484222325ULL;
            for (uint32_t i = 0; i < frameBytes; ++i) {
                h ^= vram[baseOffset + i];
                h *= 0x100000001B3ULL;
            }
            uint32_t n = s_daVerifyFrameNum++;
            bhLog("[probe] da framehash n=%u h=0x%016llX\n", n, (unsigned long long)h);
            ps2xSafeLog("[probe] da framehash n=%u h=0x%016llX\n", n, (unsigned long long)h);
        }
    }

    static void checkDaProbeSecDump() {
        auto now = std::chrono::steady_clock::now();
        double secDiff = std::chrono::duration<double>(now - s_bsProbeLastSec).count();
        if (secDiff >= 1.0) {
            uint32_t mscals = s_secBsMscals.exchange(0, std::memory_order_relaxed);
            uint32_t clipped = s_secBsBlocksClipped.exchange(0, std::memory_order_relaxed);
            uint64_t totalUs = s_secBsTimeUs.exchange(0, std::memory_order_relaxed);
            uint64_t avgUs = mscals > 0 ? (totalUs / mscals) : 0;
            bhLog("[probe] bs mscals=%u blocks_clipped=%u avg_us=%llu\n",
                  mscals, clipped, (unsigned long long)avgUs);
            ps2xSafeLog("[probe] bs mscals=%u blocks_clipped=%u avg_us=%llu\n",
                        mscals, clipped, (unsigned long long)avgUs);

            uint32_t flushes = s_secDaFlushes.exchange(0, std::memory_order_relaxed);
            uint32_t blocks = s_secDaBlocks.exchange(0, std::memory_order_relaxed);
            uint32_t maxBlk = s_secDaMaxBlocks.exchange(0, std::memory_order_relaxed);
            uint32_t avgBlk = flushes > 0 ? (blocks / flushes) : 0;
            uint32_t snapReads = s_secDaSnapshotReads.exchange(0, std::memory_order_relaxed);

            bhLog("[probe] da batch flushes=%u blocks=%u avg_blocks=%u max_blocks=%u\n",
                  flushes, blocks, avgBlk, maxBlk);
            ps2xSafeLog("[probe] da batch flushes=%u blocks=%u avg_blocks=%u max_blocks=%u\n",
                        flushes, blocks, avgBlk, maxBlk);
            bhLog("[probe] da snapshot reads=%u\n", snapReads);
            ps2xSafeLog("[probe] da snapshot reads=%u\n", snapReads);

            s_bsProbeLastSec = now;
        }
    }

    void flushMovieBatch(GS *gs, PS2Memory *memory) {
        if (s_movieBatchBlocks == 0 || s_movieBatch.empty()) {
            if (isMovieBatchVerifyEnabled() && !isMovieBatchEnabled() && s_kickMscalsExecuted > 0) {
                PS2Memory *targetMem = memory ? memory : s_batchMemory;
                verifyDaFrameHash(targetMem);
                s_kickMscalsExecuted = 0;
            }
            checkDaProbeSecDump();
            return;
        }

        GS *targetGs = gs ? gs : s_batchGs;
        PS2Memory *targetMem = memory ? memory : s_batchMemory;

        // Set EOP=1 on the last block's IMAGE tag
        const size_t lastBlockOffset = (static_cast<size_t>(s_movieBatchBlocks) - 1u) * 1120u;
        if (lastBlockOffset + 88u <= s_movieBatch.size()) {
            uint64_t *imgLoPtr = reinterpret_cast<uint64_t*>(s_movieBatch.data() + lastBlockOffset + 80u);
            *imgLoPtr |= (1ull << 15u);
        }

        if (targetMem) {
            targetMem->submitGifPacket(GifPathId::Path1, s_movieBatch.data(), static_cast<uint32_t>(s_movieBatch.size()));
        } else if (targetGs) {
            targetGs->processGIFPacket(s_movieBatch.data(), static_cast<uint32_t>(s_movieBatch.size()));
        }

        const uint32_t blks = s_movieBatchBlocks;
        s_secDaFlushes.fetch_add(1, std::memory_order_relaxed);
        s_secDaBlocks.fetch_add(blks, std::memory_order_relaxed);
        uint32_t prevMax = s_secDaMaxBlocks.load(std::memory_order_relaxed);
        while (blks > prevMax && !s_secDaMaxBlocks.compare_exchange_weak(prevMax, blks, std::memory_order_relaxed));

        s_movieBatch.clear();
        s_movieBatchBlocks = 0;
        s_movieSnapCache.valid = false;

        if (isMovieBatchVerifyEnabled()) {
            verifyDaFrameHash(targetMem);
        }
        s_kickMscalsExecuted = 0;

        checkDaProbeSecDump();
    }

    bool handleMovieHleMscal(uint32_t startPC, const uint8_t *vuCode, uint32_t codeSize,
                             const uint8_t *vuData, uint32_t dataSize, uint32_t top,
                             GS &gs, PS2Memory *memory) {
        if (!vuCode || codeSize < 0xE00u) {
            return false;
        }

        static std::atomic<uint32_t> s_mscalHashCounter{0};
        static uint64_t s_cachedMscalHash = 0;
        static bool s_cachedMscalMatch = false;

        uint64_t hash;
        bool match;
        if ((s_mscalHashCounter.fetch_add(1u, std::memory_order_relaxed) & 1023u) == 0u || s_cachedMscalHash == 0) {
            hash = 0xCBF29CE484222325ULL;
            for (uint32_t i = 0; i < 0xE00u; ++i) {
                hash ^= vuCode[i];
                hash *= 0x100000001B3ULL;
            }
            match = (hash == 0xF72A6639E95D7AC8ULL);
            s_cachedMscalHash = hash;
            s_cachedMscalMatch = match;
        } else {
            hash = s_cachedMscalHash;
            match = s_cachedMscalMatch;
        }

        static std::mutex s_sigMutex;
        static std::vector<uint64_t> s_loggedSignatures;
        // PERF-JN: this ran a mutex + vector scan + std::map increment on EVERY VU microprogram call (~450k/s). The map count is never read;
        // the signature is logged once, so only take the lock when this thread has not yet seen the current hash.
        static thread_local uint64_t s_lastSeenHash = 0;
        if (hash != s_lastSeenHash) {
            std::lock_guard<std::mutex> lock(s_sigMutex);
            if (std::find(s_loggedSignatures.begin(), s_loggedSignatures.end(), hash) == s_loggedSignatures.end()) {
                s_loggedSignatures.push_back(hash);
                std::printf("[hle] sig=0x%016llX match=%d\n", (unsigned long long)hash, match ? 1 : 0);
                std::fflush(stdout);
            }
            s_lastSeenHash = hash;
        }

        probe_ae::recordCaMscal(startPC);

        probe_ae::checkCaDump(startPC, top, vuData, dataSize);

        if (startPC != 0x048 && startPC != 0x848) {
            s_movieKickMscalIndex.store(0, std::memory_order_relaxed);
            s_kickDecision.store(KickDecision::Undecided, std::memory_order_relaxed);
            return false;
        }

        if (isMovieHleVerify()) {
            return false;
        }

        if (!isMovieHleActive()) {
            return false;
        }

        const uint8_t *rdram = memory ? memory->getRDRAM() : s_cachedRdram;
        const bool movieActive = isMovieActive(rdram);
        if (!movieActive) {
            flushMovieBatch(&gs, memory);
            return false;
        }

        if (isMovieHleStrict() && !match) {
            return false;
        }

        // =====================================================================
        // Mode A: Legacy Frame Mode (PS2X_MOVIE_HLE_MODE=frame)
        // =====================================================================
        if (isMovieHleFrameMode()) {
            flushMovieBatch(&gs, memory);
            uint32_t curIdx = s_movieKickMscalIndex.fetch_add(1, std::memory_order_relaxed);
            if (curIdx == 0) {
                uint32_t base = s_movieKickFirstUnpackSrc.load(std::memory_order_relaxed);
                uint32_t mscalsInKick = s_kickMscalCount.load(std::memory_order_relaxed);

                const char *failReason = nullptr;
                if (base == 0) {
                    failReason = "no_y_unpack";
                } else if (base + s_movieTotalMbs * 384u > PS2_RAM_SIZE) {
                    failReason = "base_out_of_ram";
                } else if (mscalsInKick != s_movieTotalMbs) {
                    failReason = "movie_geometry_mismatch";
                    static bool s_mismatchLogged = false;
                    if (!s_mismatchLogged) {
                        s_mismatchLogged = true;
                        bhLog("[hle] movie geometry mismatch mscals=%u expected=%u\n", mscalsInKick, s_movieTotalMbs);
                        ps2xSafeLog("[hle] movie geometry mismatch mscals=%u expected=%u\n", mscalsInKick, s_movieTotalMbs);
                    }
                }

                if (failReason != nullptr) {
                    s_kickDecision.store(KickDecision::Interpret, std::memory_order_relaxed);
                    s_secInterpretedKicks++;
                    return false;
                } else {
                    s_kickDecision.store(KickDecision::Native, std::memory_order_relaxed);
                    s_secNativeKicks++;

                    double native_ms = 0.0, xfer_ms = 0.0;
                    doMovieFrameTransfer(gs, memory, base, native_ms, xfer_ms);
                    s_secNativeMsTotal += native_ms;
                    s_secXferMsTotal += xfer_ms;
                    onBhMovieHleTime(static_cast<uint64_t>((native_ms + xfer_ms) * 1000.0));
                    return true;
                }
            } else {
                return (s_kickDecision.load(std::memory_order_relaxed) == KickDecision::Native);
            }
        }

        // =====================================================================
        // Mode B: Task BS Native Movie Conversion PER MSCAL from VU1 data memory
        // =====================================================================
        if (!vuData || dataSize < 16384) {
            return false;
        }

        auto t0 = std::chrono::steady_clock::now();

        // 1. Read MB destination from qw[TOP+0] (12.4 fixed point)
        const uint32_t posByteOff = ((top + 0u) & 0x3FFu) * 16u;
        int32_t rawX = 0, rawY = 0;
        std::memcpy(&rawX, vuData + posByteOff + 0, 4);
        std::memcpy(&rawY, vuData + posByteOff + 4, 4);
        const int32_t mbX = rawX / 16;
        const int32_t mbY = rawY / 16;

        probe_ae::onBwMscalBlock(mbX, mbY);
        probe_ae::onBxNativeMscal(mbX, mbY, top & 0x3FFu);
        probe_ae::onBzNativeBlock(mbX, mbY);
        // Log once per movie for the first MSCAL
        if (s_lastBsMovieInstance != s_movieInstance) {
            s_lastBsMovieInstance = s_movieInstance;
            bhLog("[hle] movie mscal-native pc=0x%03X pos0=(%d,%d) top=0x%03X\n", startPC, mbX, mbY, top & 0x3FFu);
            ps2xSafeLog("[hle] movie mscal-native pc=0x%03X pos0=(%d,%d) top=0x%03X\n", startPC, mbX, mbY, top & 0x3FFu);
        }

        // 2. Clip to SCISSOR_2 (snapshot read once per kick)
        if (!s_movieSnapCache.valid) {
            GSDebugSnapshot snap = gs.getDebugSnapshot();
            const auto &ctx = snap.ctx[1]; // Context 2
            s_movieSnapCache.scisX0 = ctx.scissor.x0;
            s_movieSnapCache.scisX1 = ctx.scissor.x1;
            s_movieSnapCache.scisY0 = ctx.scissor.y0;
            s_movieSnapCache.scisY1 = ctx.scissor.y1;
            s_movieSnapCache.fbp = ctx.frame.fbp << 5u;
            s_movieSnapCache.fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
            s_movieSnapCache.dpsm = ctx.frame.psm & 0x3Fu;
            s_lastFrameFbp = ctx.frame.fbp;
            s_movieSnapCache.valid = true;
            s_secDaSnapshotReads.fetch_add(1, std::memory_order_relaxed);
        }

        const uint32_t scisX0 = s_movieSnapCache.scisX0;
        const uint32_t scisX1 = s_movieSnapCache.scisX1;
        const uint32_t scisY0 = s_movieSnapCache.scisY0;
        const uint32_t scisY1 = s_movieSnapCache.scisY1;
        bool isClipped = false;
        if (scisX1 >= scisX0 && scisY1 >= scisY0) {
            if (mbX >= static_cast<int32_t>(scisX1 + 1) || mbY >= static_cast<int32_t>(scisY1 + 1) ||
                (mbX + 16) <= static_cast<int32_t>(scisX0) || (mbY + 16) <= static_cast<int32_t>(scisY0)) {
                isClipped = true;
            }
        }

        s_secBsMscals.fetch_add(1, std::memory_order_relaxed);

        if (isClipped) {
            s_secBsBlocksClipped.fetch_add(1, std::memory_order_relaxed);
            auto t1 = std::chrono::steady_clock::now();
            s_secBsTimeUs.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(), std::memory_order_relaxed);
            return true;
        }

        const uint32_t fbp = s_movieSnapCache.fbp; // DBP = FBP*32
        const uint32_t fbw = s_movieSnapCache.fbw;
        const uint32_t dpsm = s_movieSnapCache.dpsm;

        const uint64_t bitbltbuf_val = (static_cast<uint64_t>(fbp & 0x3FFFu) << 32u) |
                                       (static_cast<uint64_t>(fbw & 0x3Fu) << 48u) |
                                       (static_cast<uint64_t>(dpsm & 0x3Fu) << 56u);
        const uint64_t trxpos_val = (static_cast<uint64_t>(static_cast<uint32_t>(mbX) & 0x7FFu) << 32u) |
                                    (static_cast<uint64_t>(static_cast<uint32_t>(mbY) & 0x7FFu) << 48u);
        const uint64_t trxreg_val = (16ull) | (16ull << 32u);
        const uint64_t trxdir_val = 0ull;

        uint32_t *dstPixels = nullptr;
        static std::vector<uint8_t> s_mbPkt(96 + 1024, 0);

        if (isMovieBatchEnabled()) {
            static bool s_batchReserved = false;
            if (!s_batchReserved) {
                s_movieBatch.reserve(800u * 1120u);
                s_batchReserved = true;
            }

            if (s_movieBatchBlocks >= 1024u || (s_movieBatch.size() + 1120u) > (64u * 1024u * 16u)) {
                flushMovieBatch(&gs, memory);
            }

            const size_t blockOffset = s_movieBatch.size();
            s_movieBatch.resize(blockOffset + 1120u);
            uint8_t *blockPtr = s_movieBatch.data() + blockOffset;

            // Tag 1: A+D setup
            uint64_t tagLo = 4ull | (0ull << 15u) | (0ull << 58u) | (1ull << 60u);
            uint64_t tagHi = 0x0Eull;
            std::memcpy(blockPtr, &tagLo, 8);
            std::memcpy(blockPtr + 8, &tagHi, 8);

            uint64_t regAddr = 0x50ull; // BITBLTBUF
            std::memcpy(blockPtr + 16, &bitbltbuf_val, 8);
            std::memcpy(blockPtr + 24, &regAddr, 8);

            regAddr = 0x51ull; // TRXPOS
            std::memcpy(blockPtr + 32, &trxpos_val, 8);
            std::memcpy(blockPtr + 40, &regAddr, 8);

            regAddr = 0x52ull; // TRXREG
            std::memcpy(blockPtr + 48, &trxreg_val, 8);
            std::memcpy(blockPtr + 56, &regAddr, 8);

            regAddr = 0x53ull; // TRXDIR
            std::memcpy(blockPtr + 64, &trxdir_val, 8);
            std::memcpy(blockPtr + 72, &regAddr, 8);

            // Tag 2: IMAGE transfer (256 pixels = 64 quadwords = 1024 bytes)
            uint64_t imgLo = 64ull | (0ull << 15u /*EOP=0*/) | (2ull << 58u /*FLG=IMAGE*/);
            uint64_t imgHi = 0ull;
            std::memcpy(blockPtr + 80, &imgLo, 8);
            std::memcpy(blockPtr + 88, &imgHi, 8);

            dstPixels = reinterpret_cast<uint32_t*>(blockPtr + 96);
        } else {
            // Tag 1: A+D setup
            uint64_t tagLo = 4ull | (0ull << 15u) | (0ull << 58u) | (1ull << 60u);
            uint64_t tagHi = 0x0Eull;
            std::memcpy(s_mbPkt.data(), &tagLo, 8);
            std::memcpy(s_mbPkt.data() + 8, &tagHi, 8);

            uint64_t regAddr = 0x50ull; // BITBLTBUF
            std::memcpy(s_mbPkt.data() + 16, &bitbltbuf_val, 8);
            std::memcpy(s_mbPkt.data() + 24, &regAddr, 8);

            regAddr = 0x51ull; // TRXPOS
            std::memcpy(s_mbPkt.data() + 32, &trxpos_val, 8);
            std::memcpy(s_mbPkt.data() + 40, &regAddr, 8);

            regAddr = 0x52ull; // TRXREG
            std::memcpy(s_mbPkt.data() + 48, &trxreg_val, 8);
            std::memcpy(s_mbPkt.data() + 56, &regAddr, 8);

            regAddr = 0x53ull; // TRXDIR
            std::memcpy(s_mbPkt.data() + 64, &trxdir_val, 8);
            std::memcpy(s_mbPkt.data() + 72, &regAddr, 8);

            // Tag 2: IMAGE transfer (256 pixels = 64 quadwords = 1024 bytes)
            uint64_t imgLo = 64ull | (1ull << 15u /*EOP*/) | (2ull << 58u /*FLG=IMAGE*/);
            uint64_t imgHi = 0ull;
            std::memcpy(s_mbPkt.data() + 80, &imgLo, 8);
            std::memcpy(s_mbPkt.data() + 88, &imgHi, 8);

            dstPixels = reinterpret_cast<uint32_t*>(s_mbPkt.data() + 96);
        }

        static int s_movie848Shift = -999;
        if (s_movie848Shift == -999) {
            const char *env = std::getenv("PS2X_MOVIE_848_SHIFT");
            s_movie848Shift = env ? std::atoi(env) : 0;
        }
        const uint32_t extraShift = (startPC == 0x848) ? static_cast<uint32_t>(s_movie848Shift) : 0u;

        // 4. Convert YCbCr to RGBA
        for (uint32_t py = 0; py < 16u; ++py) {
            const uint32_t py_chroma = (py >> 1u) * 8u;
            uint32_t *rowDst = dstPixels + py * 16u;
            for (uint32_t px = 0; px < 16u; ++px) {
                const uint32_t p = py * 16u + px;
                const uint32_t yByteOff = ((top + 0x20u + extraShift + p) & 0x3FFu) * 16u;
                const int32_t *yLane = reinterpret_cast<const int32_t*>(vuData + yByteOff);
                const int32_t Y = yLane[0];

                const uint32_t ch_idx = py_chroma + (px >> 1u);
                const uint32_t chByteOff = ((top + 0x20u + extraShift + ch_idx) & 0x3FFu) * 16u;
                const int32_t *chLane = reinterpret_cast<const int32_t*>(vuData + chByteOff);
                const int32_t Cb = chLane[1];
                const int32_t Cr = chLane[2];

                const float y_f = static_cast<float>(Y);
                const float cb_f = static_cast<float>(Cb - 128);
                const float cr_f = static_cast<float>(Cr - 128);

                const float r_f = y_f + 1.402f * cr_f;
                const float g_f = y_f - 0.344136f * cb_f - 0.714136f * cr_f;
                const float b_f = y_f + 1.772f * cb_f;

                const int r_i = static_cast<int>(std::round(r_f));
                const int g_i = static_cast<int>(std::round(g_f));
                const int b_i = static_cast<int>(std::round(b_f));

                const uint32_t r = static_cast<uint32_t>(std::clamp(r_i, 0, 255));
                const uint32_t g = static_cast<uint32_t>(std::clamp(g_i, 0, 255));
                const uint32_t b = static_cast<uint32_t>(std::clamp(b_i, 0, 255));
                const uint32_t a = 0u;

                rowDst[px] = r | (g << 8u) | (b << 16u) | (a << 24u);
            }
        }

        s_batchGs = &gs;
        s_batchMemory = memory;
        s_kickMscalsExecuted++;

        if (isMovieBatchEnabled()) {
            s_movieBatchBlocks++;
        } else {
            // 5. Submit GIF packet ordered through Path 1
            if (memory) {
                memory->submitGifPacket(GifPathId::Path1, s_mbPkt.data(), static_cast<uint32_t>(s_mbPkt.size()));
            } else {
                gs.processGIFPacket(s_mbPkt.data(), static_cast<uint32_t>(s_mbPkt.size()));
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        s_secBsTimeUs.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(), std::memory_order_relaxed);

        checkDaProbeSecDump();

        return true;
    }

    // =========================================================================
    // Task BH Probes
    // =========================================================================
    bool isMovieActivePublic() {
        return isMovieActive(s_cachedRdram);
    }

    struct SlowPacketInfo {
        double ms = 0.0;
        uint32_t primType = 0;
        uint32_t pixelCount = 0;
        bool texOn = false;
        bool blendOn = false;
        uint32_t tw = 0;
        uint32_t th = 0;
    };

    struct SleepSite {
        const char *desc;
        const char *durationConst;
        std::atomic<uint32_t> hits{0};
        std::atomic<uint64_t> totalUs{0};
    };

    static SleepSite s_sleepSites[] = {
        {"ps2_runtime.cpp:4612", "0"},
        {"Kernel/EeScheduler.cpp:2372", "inf"},
        {"Kernel/EeScheduler.cpp:2403", "timer_or_deadline"}
    };

    static std::atomic<bool> s_bhInitLogged{false};
    static std::atomic<bool> s_bhStartupPrinted{false};
    void bmStartSamplerThread();
    void cgStartSamplerIfEnabled();
    void bhInitStartup() {
        cgStartSamplerIfEnabled();
        bool expInit = false;
        if (s_bhInitLogged.compare_exchange_strong(expInit, true)) {
            const char *env = std::getenv("PS2X_PROBE_AE");
            bool en = isBhProbeEnabled();
            bhLog("[probe] bh init enabled=%d env=%s\n", en ? 1 : 0, env ? env : "null");
        }
        if (!isBhProbeEnabled()) return;
        bmStartSamplerThread();
        bool expected = false;
        if (s_bhStartupPrinted.compare_exchange_strong(expected, true)) {
            for (const auto &site : s_sleepSites) {
                bhLog("[probe] bh sleepsite %s ms=%s\n", site.desc, site.durationConst);
            }
        }
    }

    // Thread-local recursion tracking for VIF1 and GIF
    static thread_local int tl_vif1Depth = 0;
    static thread_local std::chrono::steady_clock::time_point tl_vif1Start;
    static thread_local uint64_t tl_vif1GifUs = 0;

    static thread_local int tl_submitGifDepth = 0;
    static thread_local std::chrono::steady_clock::time_point tl_submitGifStart;

    // Per-second accumulators
    static std::atomic<uint64_t> s_bhVif1Us{0};
    static std::atomic<uint64_t> s_bhGifUs{0};
    static std::atomic<uint64_t> s_bhGsBlockUs{0};
    static std::atomic<uint32_t> s_bhGsBlockCnt{0};

    static std::atomic<uint64_t> s_bhRaw8FeedUs{0};
    static std::atomic<uint64_t> s_bhRaw8DecUs{0};
    static std::atomic<uint64_t> s_bhRaw8YccUs{0};
    static std::atomic<uint32_t> s_bhRaw8Calls{0};

    static std::atomic<uint64_t> s_bhMovieHleUs{0};

    static std::atomic<uint64_t> s_bhSchedWaitUs{0};
    static std::atomic<uint32_t> s_bhSchedWaitCnt{0};

    static std::atomic<uint64_t> s_bhDmaPollUs{0};
    static std::atomic<uint64_t> s_bhCid1Us{0};

    static std::atomic<uint64_t> s_bhIntcUs{0};
    static std::atomic<uint32_t> s_bhIntcCnt{0};
    static std::atomic<uint64_t> s_bhIntcCauseUs[32]{};
    static std::atomic<uint32_t> s_bhIntcCauseCnt[32]{};

    static std::atomic<uint32_t> s_bhIters{0};

    // GS thread metrics
    static std::atomic<uint64_t> s_bhGsBusyUs{0};
    static std::atomic<uint32_t> s_bhQueueDepthMax{0};
    static std::atomic<uint64_t> s_bhQueueDepthSum{0};
    static std::atomic<uint32_t> s_bhQueueDepthSamples{0};

    static std::mutex s_slowPktsMutex;
    static std::vector<SlowPacketInfo> s_slowPkts;

    static auto s_bhLastSec = std::chrono::steady_clock::now();

    void onBhVif1Start() {
        if (!isBhProbeEnabled()) return;
        if (tl_vif1Depth == 0) {
            tl_vif1Start = std::chrono::steady_clock::now();
            tl_vif1GifUs = 0;
        }
        tl_vif1Depth++;
    }

    void onBhVif1End() {
        if (!isBhProbeEnabled()) return;
        tl_vif1Depth--;
        if (tl_vif1Depth == 0) {
            auto now = std::chrono::steady_clock::now();
            uint64_t totalUs = std::chrono::duration_cast<std::chrono::microseconds>(now - tl_vif1Start).count();
            uint64_t selfUs = (totalUs > tl_vif1GifUs) ? (totalUs - tl_vif1GifUs) : 0;
            s_bhVif1Us.fetch_add(selfUs, std::memory_order_relaxed);
        }
    }

    void onBhSubmitGifStart() {
        if (!isBhProbeEnabled()) return;
        if (tl_submitGifDepth == 0) {
            tl_submitGifStart = std::chrono::steady_clock::now();
        }
        tl_submitGifDepth++;
    }

    void onBhSubmitGifEnd() {
        if (!isBhProbeEnabled()) return;
        tl_submitGifDepth--;
        if (tl_submitGifDepth == 0) {
            auto now = std::chrono::steady_clock::now();
            uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now - tl_submitGifStart).count();
            s_bhGifUs.fetch_add(us, std::memory_order_relaxed);
            if (tl_vif1Depth > 0) {
                tl_vif1GifUs += us;
            }
        }
    }

    void onBhGsBlockWait(uint64_t waitUs) {
        if (!isBhProbeEnabled()) return;
        s_bhGsBlockUs.fetch_add(waitUs, std::memory_order_relaxed);
        s_bhGsBlockCnt.fetch_add(1, std::memory_order_relaxed);
    }

    void onBhRaw8FeedTime(uint64_t us) {
        if (!isBhProbeEnabled()) return;
        s_bhRaw8FeedUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhRaw8DecTime(uint64_t us) {
        if (!isBhProbeEnabled()) return;
        s_bhRaw8DecUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhRaw8YccTime(uint64_t us) {
        if (!isBhProbeEnabled()) return;
        s_bhRaw8YccUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhRaw8Call() {
        if (!isBhProbeEnabled()) return;
        s_bhRaw8Calls.fetch_add(1, std::memory_order_relaxed);
        onBhIteration();
    }

    void onBhMovieHleTime(uint64_t us) {
        if (!isBhProbeEnabled()) return;
        s_bhMovieHleUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhSchedWait(uint64_t waitUs) {
        if (!isBhProbeEnabled()) return;
        s_bhSchedWaitUs.fetch_add(waitUs, std::memory_order_relaxed);
        s_bhSchedWaitCnt.fetch_add(1, std::memory_order_relaxed);
    }

    void onBhDmaPollTime(uint64_t us) {
        if (!isBhProbeEnabled()) return;
        s_bhDmaPollUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhCid1Time(uint64_t us) {
        if (!isBhProbeEnabled()) return;
        s_bhCid1Us.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhIntcHandler(uint32_t cause, uint64_t execUs) {
        if (!isBhProbeEnabled()) return;
        s_bhIntcUs.fetch_add(execUs, std::memory_order_relaxed);
        s_bhIntcCnt.fetch_add(1, std::memory_order_relaxed);
        if (cause < 32) {
            s_bhIntcCauseUs[cause].fetch_add(execUs, std::memory_order_relaxed);
            s_bhIntcCauseCnt[cause].fetch_add(1, std::memory_order_relaxed);
        }
    }

    void onBhGsPacket(uint64_t packetUs, uint32_t primType, uint32_t pixelCount, bool texOn, bool blendOn, uint32_t tw, uint32_t th) {
        if (!isBhProbeEnabled()) return;
        SlowPacketInfo info;
        info.ms = static_cast<double>(packetUs) / 1000.0;
        info.primType = primType;
        info.pixelCount = pixelCount;
        info.texOn = texOn;
        info.blendOn = blendOn;
        info.tw = tw;
        info.th = th;

        std::lock_guard<std::mutex> lock(s_slowPktsMutex);
        s_slowPkts.push_back(info);
        std::sort(s_slowPkts.begin(), s_slowPkts.end(), [](const SlowPacketInfo &a, const SlowPacketInfo &b) {
            return a.ms > b.ms;
        });
        if (s_slowPkts.size() > 3) {
            s_slowPkts.resize(3);
        }
    }

    void onBhGsRasterTime(uint64_t us) {
        if (!isBhProbeEnabled()) return;
        s_bhGsBusyUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhRecordQueueDepth(uint32_t depth) {
        if (!isBhProbeEnabled()) return;
        uint32_t curMax = s_bhQueueDepthMax.load(std::memory_order_relaxed);
        while (depth > curMax && !s_bhQueueDepthMax.compare_exchange_weak(curMax, depth, std::memory_order_relaxed)) {}
        s_bhQueueDepthSum.fetch_add(depth, std::memory_order_relaxed);
        s_bhQueueDepthSamples.fetch_add(1, std::memory_order_relaxed);
    }

    void onBhSleepSiteHit(int siteId, uint64_t us) {
        if (!isBhProbeEnabled() || siteId < 0 || siteId >= 3) return;
        bhInitStartup();
        s_sleepSites[siteId].hits.fetch_add(1, std::memory_order_relaxed);
        s_sleepSites[siteId].totalUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhIteration() {
        if (!isBhProbeEnabled()) return;
        s_bhIters.fetch_add(1, std::memory_order_relaxed);
    }

    // Task BL counters & CID1 state
    std::atomic<uint32_t> cnt_bl_3cd320{0};
    std::atomic<uint32_t> cnt_bl_3ca818{0};
    std::atomic<uint32_t> cnt_bl_435a00{0};
    std::atomic<uint32_t> cnt_bl_3cbfd0{0};
    std::atomic<uint32_t> cnt_bl_3cd288{0};

    static std::atomic<uint32_t> s_blCid1LineCount{0};
    static std::atomic<uint32_t> s_blCid1TotalReq{0};
    static std::atomic<uint32_t> s_blCid1TotalReply{0};
    static std::atomic<uint32_t> s_blCid1SecReq{0};
    static std::atomic<uint32_t> s_blCid1SecReply{0};

    static auto s_bhStartTime = std::chrono::steady_clock::now();
    static void bmTickCheck(const uint8_t *rdram);

    void bhTickCheckPerSec(const uint8_t *rdram) {
        static std::atomic<uint32_t> s_bhTickCount{0};
        uint32_t n = s_bhTickCount.fetch_add(1, std::memory_order_relaxed) + 1;

        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - s_bhLastSec).count();
        bool en = isBhProbeEnabled();
        bool movie = isMovieActive(rdram ? rdram : s_cachedRdram);

        if (n <= 5 || (n % 100 == 0)) {
            bhLog("[probe] bh tick n=%u enabled=%d elapsed=%.3f movie=%d rdram=%s\n",
                  n, en ? 1 : 0, elapsedSec, movie ? 1 : 0, rdram ? "non-null" : "null");
        }

        if (!en) return;
        if (rdram) setRdram(rdram);
        bhInitStartup();

        if (elapsedSec < 1.0) return;

        uint32_t t = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now - s_bhStartTime).count());
        bhLog("[probe] bh alive movie=%d t=%u\n", movie ? 1 : 0, t);

        if (!movie) {
            // Keep resetting if movie is not active yet so timers don't accumulate
            s_bhVif1Us.store(0, std::memory_order_relaxed);
            s_bhGifUs.store(0, std::memory_order_relaxed);
            s_bhGsBlockUs.store(0, std::memory_order_relaxed);
            s_bhGsBlockCnt.store(0, std::memory_order_relaxed);
            s_bhRaw8FeedUs.store(0, std::memory_order_relaxed);
            s_bhRaw8DecUs.store(0, std::memory_order_relaxed);
            s_bhRaw8YccUs.store(0, std::memory_order_relaxed);
            s_bhRaw8Calls.store(0, std::memory_order_relaxed);
            s_bhMovieHleUs.store(0, std::memory_order_relaxed);
            s_bhSchedWaitUs.store(0, std::memory_order_relaxed);
            s_bhSchedWaitCnt.store(0, std::memory_order_relaxed);
            s_bhDmaPollUs.store(0, std::memory_order_relaxed);
            s_bhCid1Us.store(0, std::memory_order_relaxed);
            s_bhIntcUs.store(0, std::memory_order_relaxed);
            s_bhIntcCnt.store(0, std::memory_order_relaxed);
            for (int i = 0; i < 32; ++i) {
                s_bhIntcCauseUs[i].store(0, std::memory_order_relaxed);
                s_bhIntcCauseCnt[i].store(0, std::memory_order_relaxed);
            }
            s_bhIters.store(0, std::memory_order_relaxed);
            s_bhGsBusyUs.store(0, std::memory_order_relaxed);
            s_bhQueueDepthMax.store(0, std::memory_order_relaxed);
            s_bhQueueDepthSum.store(0, std::memory_order_relaxed);
            s_bhQueueDepthSamples.store(0, std::memory_order_relaxed);
            for (auto &site : s_sleepSites) {
                site.hits.store(0, std::memory_order_relaxed);
                site.totalUs.store(0, std::memory_order_relaxed);
            }
            {
                std::lock_guard<std::mutex> lock(s_slowPktsMutex);
                s_slowPkts.clear();
            }
            s_bhLastSec = now;
            return;
        }

        // Snapshots and exchanges
        double scale = 1.0 / (elapsedSec * 1000.0); // convert us to ms/s
        double vif1Ms = s_bhVif1Us.exchange(0, std::memory_order_relaxed) * scale;
        double gifMs = s_bhGifUs.exchange(0, std::memory_order_relaxed) * scale;
        double gsBlockMs = s_bhGsBlockUs.exchange(0, std::memory_order_relaxed) * scale;
        uint32_t gsBlockCnt = static_cast<uint32_t>(s_bhGsBlockCnt.exchange(0, std::memory_order_relaxed) / elapsedSec);

        double raw8FeedMs = s_bhRaw8FeedUs.exchange(0, std::memory_order_relaxed) * scale;
        double raw8DecMs = s_bhRaw8DecUs.exchange(0, std::memory_order_relaxed) * scale;
        double raw8YccMs = s_bhRaw8YccUs.exchange(0, std::memory_order_relaxed) * scale;
        double raw8TotalMs = raw8FeedMs + raw8DecMs + raw8YccMs;
        uint32_t raw8Calls = static_cast<uint32_t>(s_bhRaw8Calls.exchange(0, std::memory_order_relaxed) / elapsedSec);

        double hleMs = s_bhMovieHleUs.exchange(0, std::memory_order_relaxed) * scale;
        double schedWaitMs = s_bhSchedWaitUs.exchange(0, std::memory_order_relaxed) * scale;
        uint32_t schedWaitCnt = static_cast<uint32_t>(s_bhSchedWaitCnt.exchange(0, std::memory_order_relaxed) / elapsedSec);

        double dmaPollMs = s_bhDmaPollUs.exchange(0, std::memory_order_relaxed) * scale;
        double cid1Ms = s_bhCid1Us.exchange(0, std::memory_order_relaxed) * scale;

        double intcMs = s_bhIntcUs.exchange(0, std::memory_order_relaxed) * scale;
        uint32_t intcCnt = static_cast<uint32_t>(s_bhIntcCnt.exchange(0, std::memory_order_relaxed) / elapsedSec);

        std::string intcCausesStr;
        for (int i = 0; i < 32; ++i) {
            uint32_t cCnt = s_bhIntcCauseCnt[i].exchange(0, std::memory_order_relaxed);
            uint64_t cUs = s_bhIntcCauseUs[i].exchange(0, std::memory_order_relaxed);
            if (cCnt > 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "c%d:%.1fms(%u) ", i, cUs * scale, static_cast<uint32_t>(cCnt / elapsedSec));
                intcCausesStr += buf;
            }
        }
        if (intcCausesStr.empty()) intcCausesStr = "none";

        double accountedMs = vif1Ms + gifMs + gsBlockMs + raw8TotalMs + hleMs + schedWaitMs + dmaPollMs + cid1Ms + intcMs;
        double otherMs = (1000.0 > accountedMs) ? (1000.0 - accountedMs) : 0.0;

        // GS Metrics
        double gsBusyMs = s_bhGsBusyUs.exchange(0, std::memory_order_relaxed) * scale;
        double gsIdleMs = (1000.0 > gsBusyMs) ? (1000.0 - gsBusyMs) : 0.0;
        uint32_t qMax = s_bhQueueDepthMax.exchange(0, std::memory_order_relaxed);
        uint64_t qSum = s_bhQueueDepthSum.exchange(0, std::memory_order_relaxed);
        uint32_t qSamples = s_bhQueueDepthSamples.exchange(0, std::memory_order_relaxed);
        double qAvg = qSamples > 0 ? (static_cast<double>(qSum) / qSamples) : 0.0;

        uint64_t primsSec = static_cast<uint64_t>(::g_profPrims.exchange(0, std::memory_order_relaxed) / elapsedSec);
        uint64_t pixelsSec = static_cast<uint64_t>(::g_profPixels.exchange(0, std::memory_order_relaxed) / elapsedSec);

        std::vector<SlowPacketInfo> slowPktsCopy;
        {
            std::lock_guard<std::mutex> lock(s_slowPktsMutex);
            slowPktsCopy = s_slowPkts;
            s_slowPkts.clear();
        }
        std::string slowPktsStr;
        for (size_t i = 0; i < slowPktsCopy.size(); ++i) {
            const auto &sp = slowPktsCopy[i];
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[#%zu:%.1fms,prim=%u,px=%u,tex=%d,blend=%d,sz=%ux%u]",
                          i + 1, sp.ms, sp.primType, sp.pixelCount, sp.texOn ? 1 : 0, sp.blendOn ? 1 : 0, sp.tw, sp.th);
            if (!slowPktsStr.empty()) slowPktsStr += ",";
            slowPktsStr += buf;
        }
        if (slowPktsStr.empty()) slowPktsStr = "none";

        // Iteration metrics
        uint32_t iters = static_cast<uint32_t>(s_bhIters.exchange(0, std::memory_order_relaxed) / elapsedSec);
        double avgIterMs = iters > 0 ? (1000.0 / iters) : 0.0;

        uint32_t ringHead = 0, ringTail = 0;
        const uint8_t *activeRdram = rdram ? rdram : s_cachedRdram;
        if (activeRdram) {
            std::memcpy(&ringTail, activeRdram + (0x696E88u & PS2_RAM_MASK), 4);
            std::memcpy(&ringHead, activeRdram + (0x696E8Cu & PS2_RAM_MASK), 4);
            ringTail &= 31u;
            ringHead &= 31u;
        }

        // Sleep sites
        std::string sleepHitsStr;
        for (int i = 0; i < 3; ++i) {
            uint32_t hits = static_cast<uint32_t>(s_sleepSites[i].hits.exchange(0, std::memory_order_relaxed) / elapsedSec);
            double sleptMs = s_sleepSites[i].totalUs.exchange(0, std::memory_order_relaxed) * scale;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "site%d(%s)=%u(%.1fms) ", i + 1, s_sleepSites[i].desc, hits, sleptMs);
            sleepHitsStr += buf;
        }

        // Output formatting
        bhLog("[probe] bh ee vif1=%.1f gif=%.1f gs_block=%.1f(%u) raw8=%.1f(feed=%.1f,dec=%.1f,ycc=%.1f,calls=%u) hle=%.1f sched_wait=%.1f(%u) dma_poll=%.1f cid1=%.1f intc=%.1f(%u)[%s] other=%.1f\n",
              vif1Ms, gifMs, gsBlockMs, gsBlockCnt, raw8TotalMs, raw8FeedMs, raw8DecMs, raw8YccMs, raw8Calls,
              hleMs, schedWaitMs, schedWaitCnt, dmaPollMs, cid1Ms, intcMs, intcCnt, intcCausesStr.c_str(), otherMs);

        bhLog("[probe] bh gs busy=%.1f idle=%.1f q_max=%u q_avg=%.1f px/s=%llu prims/s=%llu slow_pkts=<%s>\n",
              gsBusyMs, gsIdleMs, qMax, qAvg, (unsigned long long)pixelsSec, (unsigned long long)primsSec, slowPktsStr.c_str());

        bhLog("[probe] bh iter iters/s=%u avg_ms=%.1f ring_head=%u ring_tail=%u\n",
              iters, avgIterMs, ringHead, ringTail);

        bhLog("[probe] bh sleep hits: %s\n", sleepHitsStr.c_str());

        // Task BL: per-second VIF1 ms and CID1 counters if line cap hit
        bhLog("[probe] bl vif1_ms=%.1f\n", vif1Ms);
        if (s_blCid1LineCount.load(std::memory_order_relaxed) >= 400) {
            uint32_t secReq = s_blCid1SecReq.exchange(0, std::memory_order_relaxed);
            uint32_t secRep = s_blCid1SecReply.exchange(0, std::memory_order_relaxed);
            int32_t pending = static_cast<int32_t>(s_blCid1TotalReq.load(std::memory_order_relaxed) - s_blCid1TotalReply.load(std::memory_order_relaxed));
            bhLog("[probe] bl cid1 sec req=%u reply=%u pending=%d\n", secReq, secRep, pending);
        }

        bmTickCheck(rdram);

        s_bhLastSec = now;
    }

    // =========================================================================
    // Task BL Probes
    // =========================================================================
    static std::atomic<bool> s_bnClose1sPending{false};
    static std::chrono::steady_clock::time_point s_bnClose1sDueTime;

    void onBlCid1Req(uint32_t opcode, uint32_t slotPtr, uint32_t seq, uint32_t fd, uint32_t dst, uint32_t size, uint32_t flags, const char *path) {
        if (!isBhProbeEnabled()) return;
        if (opcode == 0x04) { // Close
            auto read32 = [&](uint32_t addr) -> uint32_t {
                if (!s_cachedRdram || (addr & PS2_RAM_MASK) + 4 > PS2_RAM_SIZE) return 0;
                uint32_t val = 0;
                std::memcpy(&val, s_cachedRdram + (addr & PS2_RAM_MASK), 4);
                return val;
            };
            uint32_t dev30 = read32(0x00771BB8u);
            bhLog("[probe] bn close_req seq=%u dev30=%u\n", seq, dev30);
            const uint32_t kDevBase = 0x00771B88u;
            for (int i = 0; i < 16; ++i) {
                uint32_t slotAddr = kDevBase + 0x34u + 16u * i;
                bhLog("[probe] bn close_slot i=%02d addr=0x%08X: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                      i, slotAddr, read32(slotAddr), read32(slotAddr + 4), read32(slotAddr + 8), read32(slotAddr + 12));
            }
            s_bnClose1sDueTime = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            s_bnClose1sPending.store(true, std::memory_order_relaxed);
        }
        onBoCid1Req(opcode, fd, slotPtr, seq);
        s_blCid1TotalReq.fetch_add(1, std::memory_order_relaxed);
        s_blCid1SecReq.fetch_add(1, std::memory_order_relaxed);
        uint32_t line = s_blCid1LineCount.fetch_add(1, std::memory_order_relaxed);
        if (line < 400) {
            if (path && path[0]) {
                bhLog("[probe] bl cid1 req op=%u fd=%u slot=0x%08X seq=%u dst=0x%08X size=0x%X flags=0x%X path='%s'\n",
                      opcode, fd, slotPtr, seq, dst, size, flags, path);
            } else {
                bhLog("[probe] bl cid1 req op=%u fd=%u slot=0x%08X seq=%u dst=0x%08X size=0x%X flags=0x%X\n",
                      opcode, fd, slotPtr, seq, dst, size, flags);
            }
        }
    }

    void onBlCid1Reply(uint32_t slotPtr, uint32_t seq, uint32_t result, uint32_t written) {
        if (!isBhProbeEnabled()) return;
        s_blCid1TotalReply.fetch_add(1, std::memory_order_relaxed);
        s_blCid1SecReply.fetch_add(1, std::memory_order_relaxed);
        uint32_t line = s_blCid1LineCount.fetch_add(1, std::memory_order_relaxed);
        if (line < 400) {
            bhLog("[probe] bl cid1 reply slot=0x%08X seq=%u status=0x%X written=%u\n",
                  slotPtr, seq, result, written);
        }
    }

    static std::atomic<bool> s_blStallDumped{false};

    void onBlStallDiag(const uint8_t *rdram, const EeScheduler *scheduler, uint32_t sec, uint32_t curScene) {
        if (!isBhProbeEnabled() || !rdram) return;
        if (scheduler) setScheduler(scheduler);
        if (rdram && !s_cachedRdram) s_cachedRdram = rdram;
        return; // Task BM: Stall dump is managed by bmTickCheck when movie scene stalls

        const GuestThread *th = scheduler ? scheduler->thread(1) : nullptr;
        if (th) {
            const R5900Context &ctx = th->activeContext();
            uint32_t sp = getRegU32(&ctx, 29);
            uint32_t pc = ctx.pc;
            uint32_t ra = getRegU32(&ctx, 31);
            uint32_t gp = getRegU32(&ctx, 28);
            uint32_t fp = getRegU32(&ctx, 30);
            uint32_t v0 = getRegU32(&ctx, 2), v1 = getRegU32(&ctx, 3);
            uint32_t a0 = getRegU32(&ctx, 4), a1 = getRegU32(&ctx, 5), a2 = getRegU32(&ctx, 6), a3 = getRegU32(&ctx, 7);
            uint32_t t0 = getRegU32(&ctx, 8), t1 = getRegU32(&ctx, 9), t2 = getRegU32(&ctx, 10), t3 = getRegU32(&ctx, 11);
            uint32_t t4 = getRegU32(&ctx, 12), t5 = getRegU32(&ctx, 13), t6 = getRegU32(&ctx, 14), t7 = getRegU32(&ctx, 15);
            uint32_t s0 = getRegU32(&ctx, 16), s1 = getRegU32(&ctx, 17), s2 = getRegU32(&ctx, 18), s3 = getRegU32(&ctx, 19);
            uint32_t s4 = getRegU32(&ctx, 20), s5 = getRegU32(&ctx, 21), s6 = getRegU32(&ctx, 22), s7 = getRegU32(&ctx, 23);

            bhLog("[probe] bl trigger t=%u stuck_main pc=0x%08X sp=0x%08X ra=0x%08X\n", sec, pc, sp, ra);

            // 1. Stack walk (scan 0x1800 bytes upward, cap 40 lines)
            uint32_t retLines = 0;
            for (uint32_t off = 0; off <= 0x1800 && retLines < 40; off += 4) {
                uint32_t currAddr = sp + off;
                if ((currAddr & PS2_RAM_MASK) + 4 > PS2_RAM_SIZE) continue;
                uint32_t w = 0;
                std::memcpy(&w, rdram + (currAddr & PS2_RAM_MASK), 4);
                if (w >= 0x00100000u && w < 0x00600000u) {
                    uint32_t insnAddr = w - 8;
                    if (insnAddr >= 0x00100000u && (insnAddr & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                        uint32_t insn = 0;
                        std::memcpy(&insn, rdram + (insnAddr & PS2_RAM_MASK), 4);
                        if ((insn & 0xFC000000u) == 0x0C000000u) {
                            uint32_t callee = ((w - 4) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
                            bhLog("[probe] bl ret w=0x%08X at sp+0x%X callee=0x%08X\n", w, off, callee);
                            retLines++;
                        } else if ((insn & 0xFC00003Fu) == 0x00000009u) {
                            bhLog("[probe] bl ret w=0x%08X at sp+0x%X callee=jalr\n", w, off);
                            retLines++;
                        }
                    }
                }
            }

            // Regs
            bhLog("[probe] bl regs pc=0x%08X ra=0x%08X sp=0x%08X gp=0x%08X fp=0x%08X "
                  "v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X "
                  "t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X t4=0x%08X t5=0x%08X t6=0x%08X t7=0x%08X "
                  "s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X s5=0x%08X s6=0x%08X s7=0x%08X\n",
                  pc, ra, sp, gp, fp,
                  v0, v1, a0, a1, a2, a3,
                  t0, t1, t2, t3, t4, t5, t6, t7,
                  s0, s1, s2, s3, s4, s5, s6, s7);
        }

        auto read32 = [&](uint32_t addr) -> uint32_t {
            if ((addr & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                uint32_t val = 0;
                std::memcpy(&val, rdram + (addr & PS2_RAM_MASK), 4);
                return val;
            }
            return 0;
        };

        // 2. File handle table (64 entries)
        constexpr uint32_t kHandleTableBase = 0x00746218u;
        for (int idx = 0; idx < 64; ++idx) {
            uint32_t ptr = kHandleTableBase + idx * 0x2Cu;
            uint32_t w0 = read32(ptr + 0), w1 = read32(ptr + 4), w2 = read32(ptr + 8), w3 = read32(ptr + 12);
            uint32_t w4 = read32(ptr + 16), w5 = read32(ptr + 20), w6 = read32(ptr + 24), w7 = read32(ptr + 28);
            bhLog("[probe] bl handle idx=%02d ptr=0x%08X w=[0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X]\n",
                  idx, ptr, w0, w1, w2, w3, w4, w5, w6, w7);
        }

        // 3. Device 0x771B88 slots and header
        constexpr uint32_t kDevBase = 0x00771B88u;
        char devHdrBuf[256];
        int devHdrLen = std::snprintf(devHdrBuf, sizeof(devHdrBuf), "[probe] bl dev 0x771B88 hdr:");
        for (uint32_t o = 0; o < 0x34; o += 4) {
            devHdrLen += std::snprintf(devHdrBuf + devHdrLen, sizeof(devHdrBuf) - devHdrLen, " +0x%02X:0x%08X", o, read32(kDevBase + o));
        }
        bhLog("%s\n", devHdrBuf);

        for (int h = 0; h < 8; ++h) {
            uint32_t slotAddr = kDevBase + 0x34u + 16u * h;
            bhLog("[probe] bl dev slot h=%d addr=0x%08X: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                  h, slotAddr, read32(slotAddr), read32(slotAddr + 4), read32(slotAddr + 8), read32(slotAddr + 12));
        }

        // 4. Exit condition words and context
        bhLog("[probe] bl wait 0x649DE0: 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
              read32(0x00649DE0u), read32(0x00649DE4u), read32(0x00649DE8u), read32(0x00649DECu), read32(0x00649DF0u));

        char ctxBuf[512];
        int ctxLen = std::snprintf(ctxBuf, sizeof(ctxBuf), "[probe] bl ctx 0x772180:");
        for (int i = 0; i < 16; ++i) {
            ctxLen += std::snprintf(ctxBuf + ctxLen, sizeof(ctxBuf) - ctxLen, " 0x%08X", read32(0x00772180u + i * 4));
        }
        bhLog("%s\n", ctxBuf);

        uint32_t sceneAddr = (curScene != 0) ? curScene : 0x008566a0u;
        for (uint32_t off = 0; off <= 0x120; off += 32) {
            bhLog("[probe] bl s3 +0x%03X: 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
                  off,
                  read32(sceneAddr + off + 0), read32(sceneAddr + off + 4), read32(sceneAddr + off + 8), read32(sceneAddr + off + 12),
                  read32(sceneAddr + off + 16), read32(sceneAddr + off + 20), read32(sceneAddr + off + 24), read32(sceneAddr + off + 28));
        }
    }

    void onBlStuckSec(uint32_t sec, uint32_t mainPc, uint32_t mainRa) {
        if (!isBhProbeEnabled()) return;
        uint32_t c3cd320 = cnt_bl_3cd320.exchange(0, std::memory_order_relaxed);
        uint32_t c3ca818 = cnt_bl_3ca818.exchange(0, std::memory_order_relaxed);
        uint32_t c435a00 = cnt_bl_435a00.exchange(0, std::memory_order_relaxed);
        uint32_t c3cbfd0 = cnt_bl_3cbfd0.exchange(0, std::memory_order_relaxed);
        uint32_t c3cd288 = cnt_bl_3cd288.exchange(0, std::memory_order_relaxed);
        bhLog("[probe] bl stuck t=%u main_pc=0x%08X main_ra=0x%08X 3CD320=%u 3CA818=%u 435A00=%u 3CBFD0=%u 3CD288=%u\n",
              sec, mainPc, mainRa, c3cd320, c3ca818, c435a00, c3cbfd0, c3cd288);
    }

    // =========================================================================
    // Task BM Probes
    // =========================================================================
    static const EeScheduler *s_bmScheduler = nullptr;
    void setScheduler(const EeScheduler *sched) {
        if (sched && !s_bmScheduler) s_bmScheduler = sched;
    }

    static std::atomic<uint32_t> s_bmLastCid1Op{0};
    static std::atomic<uint32_t> s_bmLastCid1Seq{0};

    void onBmCid1Req(uint32_t opcode, uint32_t seq) {
        s_bmLastCid1Op.store(opcode, std::memory_order_relaxed);
        s_bmLastCid1Seq.store(seq, std::memory_order_relaxed);
    }

    void onBmCid1(uint32_t op, uint32_t seq, uint32_t dev30_before, uint32_t dev30_after) {
        if (!isBhProbeEnabled()) return;
        static std::atomic<uint32_t> s_bmCid1Lines{0};
        uint32_t l = s_bmCid1Lines.fetch_add(1, std::memory_order_relaxed);
        if (l < 400) {
            bhLog("[probe] bm cid1 op=%u seq=%u dev30_before=%u dev30_after=%u\n",
                  op, seq, dev30_before, dev30_after);
        }
    }

    static std::atomic<bool> s_bmSamplerStarted{false};
    void bmStartSamplerThread() {
        if (!isBhProbeEnabled()) return;
        bool exp = false;
        if (s_bmSamplerStarted.compare_exchange_strong(exp, true)) {
            std::thread([]() {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    const uint8_t *rdram = s_cachedRdram;
                    if (!rdram) continue;

                    auto read32 = [&](uint32_t addr) -> uint32_t {
                        return *reinterpret_cast<const uint32_t *>(rdram + (addr & PS2_RAM_MASK));
                    };

                    // Read required addresses
                    uint32_t curDev30 = read32(0x00771BB8u);
                    uint32_t dev2c = read32(0x00771BB4u);
                    uint32_t dev34 = read32(0x00771BBCu);
                    uint32_t dev38 = read32(0x00771BC0u);
                    uint32_t dev3c = read32(0x00771BC4u);
                    uint32_t dev40 = read32(0x00771BC8u);
                    uint32_t dev44 = read32(0x00771BCCu);
                    uint32_t dev48 = read32(0x00771BD0u);
                    uint32_t dev4c = read32(0x00771BD4u);
                    uint32_t dev50 = read32(0x00771BD8u);
                    uint32_t dev54 = read32(0x00771BDCu);
                    uint32_t wait4 = read32(0x00649DE4u);
                    (void)dev2c; (void)dev34; (void)dev38; (void)dev3c; (void)dev40;
                    (void)dev44; (void)dev48; (void)dev4c; (void)dev50; (void)dev54; (void)wait4;

                    static uint32_t s_lastDev30 = 0xFFFFFFFFu;
                    if (s_lastDev30 == 0xFFFFFFFFu) {
                        s_lastDev30 = curDev30;
                    } else if (curDev30 != s_lastDev30) {
                        static std::atomic<uint32_t> s_dev30Lines{0};
                        uint32_t l = s_dev30Lines.fetch_add(1, std::memory_order_relaxed);
                        if (l < 300) {
                            uint64_t t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - s_bhStartTime).count();
                            uint32_t mainPc = 0, mainRa = 0;
                            if (s_bmScheduler) {
                                const GuestThread *th = s_bmScheduler->thread(1);
                                if (th) {
                                    const R5900Context &ctx = th->activeContext();
                                    mainPc = ctx.pc;
                                    mainRa = getRegU32(&ctx, 31);
                                }
                            }
                            uint32_t lastOp = s_bmLastCid1Op.load(std::memory_order_relaxed);
                            uint32_t lastSeq = s_bmLastCid1Seq.load(std::memory_order_relaxed);
                            bhLog("[probe] bm dev30 old=%u new=%u t_ms=%llu main_pc=0x%08X main_ra=0x%08X last_cid1_op=%u seq=%u\n",
                                  s_lastDev30, curDev30, (unsigned long long)t_ms, mainPc, mainRa, lastOp, lastSeq);
                        }
                        s_lastDev30 = curDev30;
                    }

                    // Check [scene+0xF4]
                    uint32_t smScene = read32(0x00635530u);
                    if (smScene >= 0x10000 && (smScene + 0xF8) <= PS2_RAM_SIZE) {
                        uint32_t vtable = read32(smScene);
                        if (vtable == 0x006adfa8u) {
                            uint32_t curSceneF4 = read32(smScene + 0xF4);
                            static uint32_t s_lastSceneF4 = 0xFFFFFFFFu;
                            static uint32_t s_lastSceneF4Ptr = 0;
                            if (smScene != s_lastSceneF4Ptr) {
                                s_lastSceneF4Ptr = smScene;
                                s_lastSceneF4 = curSceneF4;
                            } else if (curSceneF4 != s_lastSceneF4) {
                                static std::atomic<uint32_t> s_sceneF4Lines{0};
                                uint32_t l = s_sceneF4Lines.fetch_add(1, std::memory_order_relaxed);
                                if (l < 300) {
                                    uint64_t t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - s_bhStartTime).count();
                                    bhLog("[probe] bm scene_f4 old=%u new=%u t_ms=%llu scene=0x%08X\n",
                                          s_lastSceneF4, curSceneF4, (unsigned long long)t_ms, smScene);
                                }
                                s_lastSceneF4 = curSceneF4;
                            }
                        }
                    }

                    if (s_bnClose1sPending.load(std::memory_order_relaxed)) {
                        if (std::chrono::steady_clock::now() >= s_bnClose1sDueTime) {
                            s_bnClose1sPending.store(false, std::memory_order_relaxed);
                            uint32_t dev30_1s = read32(0x00771BB8u);
                            bhLog("[probe] bn close_post1s dev30=%u\n", dev30_1s);
                            const uint32_t kDevBase = 0x00771B88u;
                            for (int i = 0; i < 16; ++i) {
                                uint32_t slotAddr = kDevBase + 0x34u + 16u * i;
                                bhLog("[probe] bn close_post1s_slot i=%02d addr=0x%08X: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                                      i, slotAddr, read32(slotAddr), read32(slotAddr + 4), read32(slotAddr + 8), read32(slotAddr + 12));
                            }
                        }
                    }
                }
            }).detach();
        }
    }

    void boFlushTail();
    void boCheckStateChange(const uint8_t *rdram);
    bool isBoActive();
    void setBoClosed();

    static void doBmStallDump(const uint8_t *rdram, uint32_t curScene) {
        if (!rdram) return;
        auto read32 = [&](uint32_t addr) -> uint32_t {
            if ((addr & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                uint32_t val = 0;
                std::memcpy(&val, rdram + (addr & PS2_RAM_MASK), 4);
                return val;
            }
            return 0;
        };

        bhLog("[probe] bm stall dump fired: curScene=0x%08X\n", curScene);
        boFlushTail();

        // 1. Stack walk and regs like Task AT
        if (s_bmScheduler) {
            const GuestThread *th = s_bmScheduler->thread(1);
            if (th) {
                const R5900Context &ctx = th->activeContext();
                uint32_t pc = ctx.pc;
                uint32_t sp = getRegU32(&ctx, 29);
                uint32_t ra = getRegU32(&ctx, 31);
                uint32_t gp = getRegU32(&ctx, 28);
                uint32_t fp = getRegU32(&ctx, 30);
                uint32_t v0 = getRegU32(&ctx, 2), v1 = getRegU32(&ctx, 3);
                uint32_t a0 = getRegU32(&ctx, 4), a1 = getRegU32(&ctx, 5), a2 = getRegU32(&ctx, 6), a3 = getRegU32(&ctx, 7);
                uint32_t t0 = getRegU32(&ctx, 8), t1 = getRegU32(&ctx, 9), t2 = getRegU32(&ctx, 10), t3 = getRegU32(&ctx, 11);
                uint32_t t4 = getRegU32(&ctx, 12), t5 = getRegU32(&ctx, 13), t6 = getRegU32(&ctx, 14), t7 = getRegU32(&ctx, 15);
                uint32_t s0 = getRegU32(&ctx, 16), s1 = getRegU32(&ctx, 17), s2 = getRegU32(&ctx, 18), s3 = getRegU32(&ctx, 19);
                uint32_t s4 = getRegU32(&ctx, 20), s5 = getRegU32(&ctx, 21), s6 = getRegU32(&ctx, 22), s7 = getRegU32(&ctx, 23);

                bhLog("[probe] bm dump trigger: stuck_main pc=0x%08X sp=0x%08X ra=0x%08X\n", pc, sp, ra);

                uint32_t retLines = 0;
                for (uint32_t off = 0; off <= 0x1800 && retLines < 40; off += 4) {
                    uint32_t currAddr = sp + off;
                    if ((currAddr & PS2_RAM_MASK) + 4 > PS2_RAM_SIZE) continue;
                    uint32_t w = 0;
                    std::memcpy(&w, rdram + (currAddr & PS2_RAM_MASK), 4);
                    if (w >= 0x00100000u && w < 0x00600000u) {
                        uint32_t insnAddr = w - 8;
                        if (insnAddr >= 0x00100000u && (insnAddr & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                            uint32_t insn = 0;
                            std::memcpy(&insn, rdram + (insnAddr & PS2_RAM_MASK), 4);
                            if ((insn & 0xFC000000u) == 0x0C000000u) {
                                uint32_t callee = ((w - 4) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
                                bhLog("[probe] bm ret w=0x%08X at sp+0x%X callee=0x%08X\n", w, off, callee);
                                retLines++;
                            } else if ((insn & 0xFC00003Fu) == 0x00000009u) {
                                bhLog("[probe] bm ret w=0x%08X at sp+0x%X callee=jalr\n", w, off);
                                retLines++;
                            }
                        }
                    }
                }

                bhLog("[probe] bm regs pc=0x%08X ra=0x%08X sp=0x%08X gp=0x%08X fp=0x%08X "
                      "v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X "
                      "t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X t4=0x%08X t5=0x%08X t6=0x%08X t7=0x%08X "
                      "s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X s5=0x%08X s6=0x%08X s7=0x%08X\n",
                      pc, ra, sp, gp, fp,
                      v0, v1, a0, a1, a2, a3,
                      t0, t1, t2, t3, t4, t5, t6, t7,
                      s0, s1, s2, s3, s4, s5, s6, s7);
            }
        }

        // 2. Scene object 0x200 bytes
        uint32_t sceneAddr = (curScene != 0) ? curScene : 0x008566a0u;
        for (uint32_t off = 0; off < 0x200; off += 32) {
            bhLog("[probe] bm scene +0x%03X: 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
                  off,
                  read32(sceneAddr + off + 0), read32(sceneAddr + off + 4), read32(sceneAddr + off + 8), read32(sceneAddr + off + 12),
                  read32(sceneAddr + off + 16), read32(sceneAddr + off + 20), read32(sceneAddr + off + 24), read32(sceneAddr + off + 28));
        }

        // 3. Device 0x771B88..+0x100
        const uint32_t kDevBase = 0x00771B88u;
        for (uint32_t off = 0; off < 0x100; off += 32) {
            bhLog("[probe] bm dev +0x%03X: 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
                  off,
                  read32(kDevBase + off + 0), read32(kDevBase + off + 4), read32(kDevBase + off + 8), read32(kDevBase + off + 12),
                  read32(kDevBase + off + 16), read32(kDevBase + off + 20), read32(kDevBase + off + 24), read32(kDevBase + off + 28));
        }

        // 4. [0x649DE0..0x649DF0]
        bhLog("[probe] bm wait 0x649DE0: 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
              read32(0x00649DE0u), read32(0x00649DE4u), read32(0x00649DE8u), read32(0x00649DECu), read32(0x00649DF0u));

        // 5. 4E2730 early-exit inputs
        uint32_t scene_f4 = (curScene != 0) ? read32(curScene + 0xF4) : 0;
        uint32_t dev30 = read32(0x00771BB8u);
        uint8_t byte_72d390 = *reinterpret_cast<const uint8_t *>(rdram + (0x0072D390u & 0x01FFFFFFu));
        bhLog("[probe] bm 4e2730 inputs: [scene+0xF4]=0x%08X [0x771BB8]=0x%08X byte[0x72D390]=0x%02X\n",
              scene_f4, dev30, byte_72d390);
    }

    static void bmTickCheck(const uint8_t *rdram) {
        if (!isBhProbeEnabled() || !rdram) return;
        if (!s_cachedRdram) s_cachedRdram = rdram;
        bmStartSamplerThread();

        auto read32 = [&](uint32_t addr) -> uint32_t {
            if ((addr & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                uint32_t val = 0;
                std::memcpy(&val, rdram + (addr & PS2_RAM_MASK), 4);
                return val;
            }
            return 0;
        };

        uint32_t sm = 0x00635530u;
        uint32_t curScene = read32(sm);
        uint32_t curVtable = (curScene >= 0x10000 && (curScene + 4) <= PS2_RAM_SIZE) ? read32(curScene) : 0;

        uint32_t f_val = g_probe_F.load(std::memory_order_relaxed);
        uint32_t g_val = g_probe_G.load(std::memory_order_relaxed);
        uint32_t c4e = g_probe_4e2730.load(std::memory_order_relaxed);
        uint32_t c31a = g_probe_31a514.load(std::memory_order_relaxed);
        uint32_t dev30 = read32(0x00771BB8u);
        uint32_t scene_f4 = (curScene >= 0x10000 && (curScene + 0xF8) <= PS2_RAM_SIZE) ? read32(curScene + 0xF4) : 0;

        bhLog("[probe] bm sec dev30=%u F=%u G=%u 4e2730=%u 31a514=%u scene=0x%08X scene_f4=%u\n",
              dev30, f_val, g_val, c4e, c31a, curScene, scene_f4);

        static bool s_hasSeenFPositive = false;
        static uint32_t s_consecFZeroSec = 0;
        static uint32_t s_lastMovieScenePtr = 0;
        static uint32_t s_newMovieSceneFZeroSec = 0;
        static bool s_sceneChangedToNewMovie = false;
        static std::atomic<bool> s_bmStallDumped{false};

        if (f_val > 0) {
            s_hasSeenFPositive = true;
            s_consecFZeroSec = 0;
        } else {
            s_consecFZeroSec++;
        }

        if (curVtable == 0x006adfa8u) {
            if (s_lastMovieScenePtr != 0 && curScene != s_lastMovieScenePtr) {
                s_sceneChangedToNewMovie = true;
                s_newMovieSceneFZeroSec = 0;
            }
            s_lastMovieScenePtr = curScene;
            if (s_sceneChangedToNewMovie) {
                if (f_val == 0) {
                    s_newMovieSceneFZeroSec++;
                } else {
                    s_newMovieSceneFZeroSec = 0;
                }
            }
        }

        bool isMovieVtable = (curVtable == 0x006adfa8u);
        bool cond1 = (s_hasSeenFPositive && s_consecFZeroSec >= 5);
        bool cond2 = (s_sceneChangedToNewMovie && s_newMovieSceneFZeroSec >= 5);

        if (isMovieVtable && (cond1 || cond2)) {
            bool exp = false;
            if (s_bmStallDumped.compare_exchange_strong(exp, true)) {
                doBmStallDump(rdram, curScene);
            }
        }

        if (isBoActive()) {
            boCheckStateChange(rdram);
            if (curVtable != 0x006adfa8u) {
                boFlushTail();
                setBoClosed();
            }
        }
    }

    // =========================================================================
    // Task BN Probes
    // =========================================================================
    static std::mutex s_bnMutex;
    static std::deque<std::string> s_bnPreRing; // max 60 lines
    static std::atomic<bool> s_bnActive{false};
    static std::atomic<bool> s_bnSceneEnded{false};
    static std::atomic<uint32_t> s_bnTotalLines{0};

    static void bnLogLine(const uint8_t *rdram, const char *fmt, ...) {
        if (!isBhProbeEnabled()) return;
        if (s_bnSceneEnded.load(std::memory_order_relaxed)) return;
        if (s_bnTotalLines.load(std::memory_order_relaxed) >= 300) return;

        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        const uint8_t *r = rdram ? rdram : s_cachedRdram;
        uint32_t f_state = 0;
        if (r && (0x00772188u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
            std::memcpy(&f_state, r + (0x00772188u & PS2_RAM_MASK), sizeof(f_state));
        }

        uint32_t curVtable = g_trackedCurVtable.load(std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(s_bnMutex);
        if (s_bnSceneEnded.load(std::memory_order_relaxed)) return;

        if (!s_bnActive.load(std::memory_order_relaxed)) {
            if (f_state >= 10 && curVtable == 0x006adfa8u) {
                s_bnActive.store(true, std::memory_order_relaxed);
                for (const auto &line : s_bnPreRing) {
                    if (s_bnTotalLines.load(std::memory_order_relaxed) < 300) {
                        s_bnTotalLines.fetch_add(1, std::memory_order_relaxed);
                        bhLog("%s", line.c_str());
                    }
                }
                s_bnPreRing.clear();
            } else {
                if (s_bnPreRing.size() >= 60) {
                    s_bnPreRing.pop_front();
                }
                s_bnPreRing.push_back(std::string(buf));
                return;
            }
        } else {
            if (curVtable != 0x006adfa8u) {
                s_bnSceneEnded.store(true, std::memory_order_relaxed);
                return;
            }
        }

        if (s_bnTotalLines.load(std::memory_order_relaxed) < 300) {
            s_bnTotalLines.fetch_add(1, std::memory_order_relaxed);
            bhLog("%s", buf);
        }
    }

    // 0x24BFF8: Shutdown fn
    BnHook_24BFF8::BnHook_24BFF8(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        auto read32 = [&](uint32_t addr) -> uint32_t {
            if (!rdram || (addr & PS2_RAM_MASK) + 4 > PS2_RAM_SIZE) return 0;
            uint32_t val = 0;
            std::memcpy(&val, rdram + (addr & PS2_RAM_MASK), 4);
            return val;
        };
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t dev30 = read32(0x00771BB8u);
        uint32_t w08 = read32(a0 + 0x08);
        int32_t w0C = static_cast<int32_t>(read32(a0 + 0x0C));
        int32_t w10 = static_cast<int32_t>(read32(a0 + 0x10));
        uint32_t w14 = read32(a0 + 0x14);
        uint32_t w18 = read32(a0 + 0x18);

        bnLogLine(rdram, "[probe] bn 24BFF8 entry a0=0x%08X dev30=%u +08=0x%08X +0C=%d(0x%X) +10=%d(0x%X) +14=0x%08X +18=0x%08X\n",
                  a0, dev30, w08, w0C, (uint32_t)w0C, w10, (uint32_t)w10, w14, w18);

        if (w08 == 0) {
            bnLogLine(rdram, "[probe] bn 24BFF8 SKIP ALL: [a0+0x08]==0\n");
        }
        if (w10 < 0) {
            bnLogLine(rdram, "[probe] bn 24BFF8 SKIP slot10: [s0+0x10]=%d is negative\n", w10);
        }
        if (w0C < 0) {
            bnLogLine(rdram, "[probe] bn 24BFF8 SKIP slot0C: [s0+0x0C]=%d is negative\n", w0C);
        }

        const uint32_t kDevBase = 0x00771B88u;
        for (int i = 0; i < 16; ++i) {
            uint32_t slotAddr = kDevBase + 0x34u + 16u * i;
            uint32_t s0 = read32(slotAddr);
            uint32_t s1 = read32(slotAddr + 4);
            uint32_t s2 = read32(slotAddr + 8);
            uint32_t s3 = read32(slotAddr + 12);
            bnLogLine(rdram, "[probe] bn 24BFF8 slot i=%02d addr=0x%08X: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                      i, slotAddr, s0, s1, s2, s3);
        }
    }

    BnHook_24BFF8::~BnHook_24BFF8() {
        if (!isEnabled()) return;
        auto read32 = [&](uint32_t addr) -> uint32_t {
            if (!rdram || (addr & PS2_RAM_MASK) + 4 > PS2_RAM_SIZE) return 0;
            uint32_t val = 0;
            std::memcpy(&val, rdram + (addr & PS2_RAM_MASK), 4);
            return val;
        };
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t dev30 = read32(0x00771BB8u);
        bnLogLine(rdram, "[probe] bn 24BFF8 exit v0=0x%08X dev30=%u\n", v0, dev30);
    }

    // 0x1DA880
    BnHook_1DA880::BnHook_1DA880(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        a1 = getRegU32(ctx, 5);
        a2 = getRegU32(ctx, 6);
        a3 = getRegU32(ctx, 7);
        dev30_in = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
    }
    BnHook_1DA880::~BnHook_1DA880() {
        if (!isEnabled()) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t dev30_out = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
        bnLogLine(rdram, "[probe] bn 1DA880 a0=0x%08X a1=%d(0x%X) a2=%u a3=0x%08X -> v0=0x%08X dev30=%u->%u\n",
                  a0, (int32_t)a1, a1, a2, a3, v0, dev30_in, dev30_out);
    }

    // 0x1DB798
    BnHook_1DB798::BnHook_1DB798(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        a1 = getRegU32(ctx, 5);
        dev30_in = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
    }
    BnHook_1DB798::~BnHook_1DB798() {
        if (!isEnabled()) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t dev30_out = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
        bnLogLine(rdram, "[probe] bn 1DB798 a0=0x%08X a1=%d(0x%X) -> v0=0x%08X dev30=%u->%u\n",
                  a0, (int32_t)a1, a1, v0, dev30_in, dev30_out);
    }

    // =========================================================================
    // Task BO Probes
    // =========================================================================
    static std::mutex s_boMutex;
    static std::atomic<bool> s_boActive{false};
    static std::atomic<bool> s_boClosed{false};
    static std::atomic<bool> s_boFlushed{false};
    static std::atomic<uint32_t> s_boTotalLines{0};
    static std::deque<std::string> s_boTailRing;
    static std::atomic<uint32_t> s_boLastState{0xFFFFFFFFu};

    struct BoCtxFields {
        uint32_t st, c, h10, h14, h18;
    };

    static BoCtxFields getBoCtxFields(const uint8_t *rdram) {
        BoCtxFields cf = {0, 0, 0, 0, 0};
        const uint8_t *r = rdram ? rdram : s_cachedRdram;
        if (r && (0x00772180u & PS2_RAM_MASK) + 0x20 <= PS2_RAM_SIZE) {
            std::memcpy(&cf.st,  r + (0x00772188u & PS2_RAM_MASK), 4);
            std::memcpy(&cf.c,   r + (0x0077218Cu & PS2_RAM_MASK), 4);
            std::memcpy(&cf.h10, r + (0x00772190u & PS2_RAM_MASK), 4);
            std::memcpy(&cf.h14, r + (0x00772194u & PS2_RAM_MASK), 4);
            std::memcpy(&cf.h18, r + (0x00772198u & PS2_RAM_MASK), 4);
        }
        return cf;
    }

    static uint32_t getDev30(const uint8_t *rdram) {
        const uint8_t *r = rdram ? rdram : s_cachedRdram;
        if (r && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
            return *reinterpret_cast<const uint32_t *>(r + (0x00771BB8u & PS2_RAM_MASK));
        }
        return 0;
    }

    static bool isJalValidated(const uint8_t *rdram, uint32_t addr) {
        const uint8_t *r = rdram ? rdram : s_cachedRdram;
        if (!r) return false;
        if ((addr & 3) != 0) return false;
        if (addr < 0x00100008u || addr >= 0x00600000u) return false;
        uint32_t callAddr = addr - 8;
        if ((callAddr & PS2_RAM_MASK) + 4 > PS2_RAM_SIZE) return false;
        uint32_t instr = *reinterpret_cast<const uint32_t *>(r + (callAddr & PS2_RAM_MASK));
        uint32_t op = instr >> 26;
        if (op == 3) return true; // jal target
        if (op == 0 && (instr & 0x3Fu) == 9) return true; // jalr rs, rd
        if (op == 1) { // bal / bgezal / bltzal
            uint32_t rt = (instr >> 16) & 0x1Fu;
            if (rt == 0x10 || rt == 0x11) return true;
        }
        return false;
    }

    static void getRaChain(const uint8_t *rdram, R5900Context *ctx, uint32_t ra_in, uint32_t sp_in, uint32_t &ra1, uint32_t &ra2) {
        ra1 = ra_in ? ra_in : (ctx ? getRegU32(ctx, 31) : 0);
        ra2 = 0;
        uint32_t sp = sp_in ? sp_in : (ctx ? getRegU32(ctx, 29) : 0);
        const uint8_t *r = rdram ? rdram : s_cachedRdram;
        if (!r || sp < 0x1000) return;
        uint32_t spMasked = sp & PS2_RAM_MASK;
        for (uint32_t off = 0; off <= 0x200 - 4; off += 4) {
            if (spMasked + off + 4 > PS2_RAM_SIZE) break;
            uint32_t candidate = *reinterpret_cast<const uint32_t *>(r + spMasked + off);
            if (candidate != ra1 && isJalValidated(r, candidate)) {
                ra2 = candidate;
                break;
            }
        }
    }

    static void boLogLine(const char *fmt, ...) {
        if (!isEnabled()) return;
        if (!s_boActive.load(std::memory_order_relaxed) || s_boClosed.load(std::memory_order_relaxed)) return;
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(s_boMutex);
        uint32_t idx = s_boTotalLines.fetch_add(1, std::memory_order_relaxed);
        if (idx < 150) {
            bhLog("%s", buf);
            ps2xSafeLog("%s", buf);
        } else {
            if (s_boTailRing.size() >= 100) {
                s_boTailRing.pop_front();
            }
            s_boTailRing.push_back(std::string(buf));
        }
    }

    bool isBoActive() {
        return s_boActive.load(std::memory_order_relaxed) && !s_boClosed.load(std::memory_order_relaxed);
    }

    void setBoClosed() {
        s_boClosed.store(true, std::memory_order_relaxed);
    }

    void boFlushTail() {
        std::lock_guard<std::mutex> lock(s_boMutex);
        if (s_boFlushed.exchange(true)) return;
        uint32_t total = s_boTotalLines.load(std::memory_order_relaxed);
        if (total > 150 && !s_boTailRing.empty()) {
            uint32_t skipped = (total > 150 + s_boTailRing.size()) ? (total - 150 - (uint32_t)s_boTailRing.size()) : 0;
            if (skipped > 0) {
                bhLog("[probe] bo ... (skipped %u lines) ...\n", skipped);
                ps2xSafeLog("[probe] bo ... (skipped %u lines) ...\n", skipped);
            }
            for (const auto &line : s_boTailRing) {
                bhLog("%s", line.c_str());
                ps2xSafeLog("%s", line.c_str());
            }
        }
        s_boTailRing.clear();
    }

    void boCheckStateChange(const uint8_t *rdram) {
        if (!s_boActive.load(std::memory_order_relaxed) || s_boClosed.load(std::memory_order_relaxed)) return;
        const uint8_t *r = rdram ? rdram : s_cachedRdram;
        BoCtxFields cf = getBoCtxFields(r);
        uint32_t lastSt = s_boLastState.load(std::memory_order_relaxed);
        if (lastSt == 0xFFFFFFFFu) {
            s_boLastState.store(cf.st, std::memory_order_relaxed);
        } else if (cf.st != lastSt) {
            s_boLastState.store(cf.st, std::memory_order_relaxed);
            uint32_t dev30 = getDev30(r);
            boLogLine("[probe] bo state %u->%u dev30=%u ctx_C=0x%08X ctx_10=0x%08X ctx_14=0x%08X\n",
                      lastSt, cf.st, dev30, cf.c, cf.h10, cf.h14);
        }
    }

    void onBoFEntry() {
        if (!isEnabled()) return;
        if (!s_boActive.load(std::memory_order_relaxed)) {
            uint32_t curVtable = g_trackedCurVtable.load(std::memory_order_relaxed);
            if (curVtable != 0x006adfa8u && s_cachedRdram) {
                uint32_t sc = *reinterpret_cast<const uint32_t *>(s_cachedRdram + (0x00635530u & PS2_RAM_MASK));
                if (sc >= 0x10000 && (sc & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                    curVtable = *reinterpret_cast<const uint32_t *>(s_cachedRdram + (sc & PS2_RAM_MASK));
                }
            }
            if (curVtable == 0x006adfa8u) {
                s_boActive.store(true, std::memory_order_relaxed);
                boLogLine("[probe] bo activated on first F call in movie scene\n");
            }
        }
        if (s_cachedRdram) {
            boCheckStateChange(s_cachedRdram);
        }
    }

    void onBoCid1Req(uint32_t opcode, uint32_t fd, uint32_t slotPtr, uint32_t seq) {
        if (!isEnabled()) return;
        if (!s_boActive.load(std::memory_order_relaxed) || s_boClosed.load(std::memory_order_relaxed)) return;
        const uint8_t *rdram = s_cachedRdram;
        boCheckStateChange(rdram);
        uint32_t dev30 = getDev30(rdram);
        boLogLine("[probe] bo cid1 op=%u fd=%u slot=0x%08X seq=%u dev30=%u\n",
                  opcode, fd, slotPtr, seq, dev30);
        if (opcode == 0x04) {
            boFlushTail();
            s_boClosed.store(true, std::memory_order_relaxed);
        }
    }

    void onBoLabel(uint32_t labelAddr) {
        if (!isEnabled()) return;
        if (!s_boActive.load(std::memory_order_relaxed) || s_boClosed.load(std::memory_order_relaxed)) return;
        const uint8_t *rdram = s_cachedRdram;
        boCheckStateChange(rdram);
        uint32_t dev30 = getDev30(rdram);
        BoCtxFields cf = getBoCtxFields(rdram);
        boLogLine("[probe] bo label 0x%08X dev30=%u st=%u ctx_C=0x%08X ctx_10=0x%08X ctx_14=0x%08X ctx_18=0x%08X\n",
                  labelAddr, dev30, cf.st, cf.c, cf.h10, cf.h14, cf.h18);
    }

    void on24CD58(R5900Context *ctx) {
        (void)ctx;
        onBoLabel(0x24CD58u);
    }

    void on24CDAC(R5900Context *ctx) {
        (void)ctx;
        onBoLabel(0x24CDACu);
    }

    // 0x1DB164
    BnHook_1DB164::BnHook_1DB164(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        a1 = getRegU32(ctx, 5);
        a2 = getRegU32(ctx, 6);
        ra_in = getRegU32(ctx, 31);
        sp_in = getRegU32(ctx, 29);
        dev30_in = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
    }
    BnHook_1DB164::~BnHook_1DB164() {
        if (!isEnabled()) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t dev30_out = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
        bnLogLine(rdram, "[probe] bn 1DB164 a0=0x%08X a1=%d(0x%X) a2=%u -> v0=0x%08X dev30=%u->%u\n",
                  a0, (int32_t)a1, a1, a2, v0, dev30_in, dev30_out);
        if (isBoActive()) {
            boCheckStateChange(rdram);
            uint32_t ra1 = 0, ra2 = 0;
            getRaChain(rdram, ctx, ra_in, sp_in, ra1, ra2);
            BoCtxFields cf = getBoCtxFields(rdram);
            boLogLine("[probe] bo 1DB164 a0=0x%08X a1=%d(0x%X) a2=%u -> v0=0x%08X dev30=%u->%u ra=0x%08X ra2=0x%08X st=%u ctx_C=0x%08X ctx_10=0x%08X ctx_14=0x%08X ctx_18=0x%08X\n",
                      a0, (int32_t)a1, a1, a2, v0, dev30_in, dev30_out, ra1, ra2, cf.st, cf.c, cf.h10, cf.h14, cf.h18);
        }
    }

    // 0x1DBA64
    BnHook_1DBA64::BnHook_1DBA64(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        ra_in = getRegU32(ctx, 31);
        sp_in = getRegU32(ctx, 29);
        dev30_in = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
    }
    BnHook_1DBA64::~BnHook_1DBA64() {
        if (!isEnabled()) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t dev30_out = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
        bnLogLine(rdram, "[probe] bn 1DBA64 alloc a0=0x%08X -> slot=%d(0x%X) dev30=%u->%u\n",
                  a0, (int32_t)v0, v0, dev30_in, dev30_out);
        if (isBoActive()) {
            boCheckStateChange(rdram);
            uint32_t ra1 = 0, ra2 = 0;
            getRaChain(rdram, ctx, ra_in, sp_in, ra1, ra2);
            BoCtxFields cf = getBoCtxFields(rdram);
            boLogLine("[probe] bo 1DBA64 alloc a0=0x%08X -> slot=%d(0x%X) dev30=%u->%u ra=0x%08X ra2=0x%08X st=%u ctx_C=0x%08X ctx_10=0x%08X ctx_14=0x%08X ctx_18=0x%08X\n",
                      a0, (int32_t)v0, v0, dev30_in, dev30_out, ra1, ra2, cf.st, cf.c, cf.h10, cf.h14, cf.h18);
        }
    }

    // 0x1DBAA0
    BnHook_1DBAA0::BnHook_1DBAA0(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        a1 = getRegU32(ctx, 5);
        ra_in = getRegU32(ctx, 31);
        sp_in = getRegU32(ctx, 29);
        dev30_in = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
    }
    BnHook_1DBAA0::~BnHook_1DBAA0() {
        if (!isEnabled()) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t dev30_out = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
        bnLogLine(rdram, "[probe] bn 1DBAA0 release a0=0x%08X slot=%d(0x%X) -> v0=0x%08X dev30=%u->%u\n",
                  a0, (int32_t)a1, a1, v0, dev30_in, dev30_out);
        if (isBoActive()) {
            boCheckStateChange(rdram);
            uint32_t ra1 = 0, ra2 = 0;
            getRaChain(rdram, ctx, ra_in, sp_in, ra1, ra2);
            BoCtxFields cf = getBoCtxFields(rdram);
            boLogLine("[probe] bo 1DBAA0 release a0=0x%08X slot=%d(0x%X) -> v0=0x%08X dev30=%u->%u ra=0x%08X ra2=0x%08X st=%u ctx_C=0x%08X ctx_10=0x%08X ctx_14=0x%08X ctx_18=0x%08X\n",
                      a0, (int32_t)a1, a1, v0, dev30_in, dev30_out, ra1, ra2, cf.st, cf.c, cf.h10, cf.h14, cf.h18);
        }
    }

    // 0x24CBFC
    BnHook_24CBFC::BnHook_24CBFC(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        dev30_in = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
    }
    BnHook_24CBFC::~BnHook_24CBFC() {
        if (!isEnabled()) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t dev30_out = (rdram && (0x00771BB8u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + 0x00771BB8u) : 0;
        bnLogLine(rdram, "[probe] bn 24CBFC poll a0=0x%08X -> v0=0x%08X dev30=%u->%u\n",
                  a0, v0, dev30_in, dev30_out);
    }

    // 0x1DC67C
    BoHook_1DC67C::BoHook_1DC67C(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        a1 = getRegU32(ctx, 5);
        a2 = getRegU32(ctx, 6);
        a3 = getRegU32(ctx, 7);
        t0 = getRegU32(ctx, 8);
        ra_in = getRegU32(ctx, 31);
        sp_in = getRegU32(ctx, 29);
        dev30_in = getDev30(rdram);
        a1_in[0] = a1_in[1] = a1_in[2] = a1_in[3] = 0;
        const uint8_t *m = rdram ? rdram : s_cachedRdram;
        if (m && a1 >= 0x1000 && (a1 & PS2_RAM_MASK) + 16 <= PS2_RAM_SIZE) {
            std::memcpy(a1_in, m + (a1 & PS2_RAM_MASK), 16);
        }
    }
    BoHook_1DC67C::~BoHook_1DC67C() {
        if (!isEnabled()) return;
        if (isBoActive()) {
            boCheckStateChange(rdram);
            uint32_t v0 = getRegU32(ctx, 2);
            uint32_t dev30_out = getDev30(rdram);
            uint32_t a1_out[4] = {0, 0, 0, 0};
            const uint8_t *m = rdram ? rdram : s_cachedRdram;
            if (m && a1 >= 0x1000 && (a1 & PS2_RAM_MASK) + 16 <= PS2_RAM_SIZE) {
                std::memcpy(a1_out, m + (a1 & PS2_RAM_MASK), 16);
            }
            uint32_t ra1 = 0, ra2 = 0;
            getRaChain(rdram, ctx, ra_in, sp_in, ra1, ra2);
            BoCtxFields cf = getBoCtxFields(rdram);
            boLogLine("[probe] bo 1DC67C a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X t0=0x%08X in=[0x%08X,0x%08X,0x%08X,0x%08X] -> v0=0x%08X out=[0x%08X,0x%08X,0x%08X,0x%08X] dev30=%u->%u ra=0x%08X ra2=0x%08X st=%u ctx_C=0x%08X ctx_10=0x%08X ctx_14=0x%08X ctx_18=0x%08X\n",
                      a0, a1, a2, a3, t0,
                      a1_in[0], a1_in[1], a1_in[2], a1_in[3],
                      v0, a1_out[0], a1_out[1], a1_out[2], a1_out[3],
                      dev30_in, dev30_out, ra1, ra2, cf.st, cf.c, cf.h10, cf.h14, cf.h18);
        }
    }

    // 0x1DC578
    BoHook_1DC578::BoHook_1DC578(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        a1 = getRegU32(ctx, 5);
        ra_in = getRegU32(ctx, 31);
        sp_in = getRegU32(ctx, 29);
        dev30_in = getDev30(rdram);
    }
    BoHook_1DC578::~BoHook_1DC578() {
        if (!isEnabled()) return;
        if (isBoActive()) {
            boCheckStateChange(rdram);
            uint32_t v0 = getRegU32(ctx, 2);
            uint32_t dev30_out = getDev30(rdram);
            uint32_t ra1 = 0, ra2 = 0;
            getRaChain(rdram, ctx, ra_in, sp_in, ra1, ra2);
            BoCtxFields cf = getBoCtxFields(rdram);
            boLogLine("[probe] bo 1DC578 poll a0=0x%08X a1=0x%08X -> v0=0x%08X dev30=%u->%u ra=0x%08X ra2=0x%08X st=%u ctx_C=0x%08X ctx_10=0x%08X ctx_14=0x%08X ctx_18=0x%08X\n",
                      a0, a1, v0, dev30_in, dev30_out, ra1, ra2, cf.st, cf.c, cf.h10, cf.h14, cf.h18);
        }
    }

    // 0x1DD010
    BoHook_1DD010::BoHook_1DD010(const uint8_t *r, R5900Context *c) : rdram(r), ctx(c) {
        if (!isEnabled()) return;
        a0 = getRegU32(ctx, 4);
        a1 = getRegU32(ctx, 5);
        ra_in = getRegU32(ctx, 31);
        sp_in = getRegU32(ctx, 29);
        dev30_in = getDev30(rdram);
    }
    BoHook_1DD010::~BoHook_1DD010() {
        if (!isEnabled()) return;
        if (isBoActive()) {
            boCheckStateChange(rdram);
            uint32_t v0 = getRegU32(ctx, 2);
            uint32_t dev30_out = getDev30(rdram);
            uint32_t ra1 = 0, ra2 = 0;
            getRaChain(rdram, ctx, ra_in, sp_in, ra1, ra2);
            BoCtxFields cf = getBoCtxFields(rdram);
            boLogLine("[probe] bo 1DD010 pump a0=0x%08X a1=0x%08X -> v0=0x%08X dev30=%u->%u ra=0x%08X ra2=0x%08X st=%u ctx_C=0x%08X ctx_10=0x%08X ctx_14=0x%08X ctx_18=0x%08X\n",
                      a0, a1, v0, dev30_in, dev30_out, ra1, ra2, cf.st, cf.c, cf.h10, cf.h14, cf.h18);
        }
    }

    // Task BP
    void onBpCloseLog(uint32_t fd, uint32_t freed_slots, uint32_t dev30_before, uint32_t dev30_after) {
        bhLog("[hle] cid1 close fd=%u freed_slots=%u dev30_before=%u dev30_after=%u\n",
              fd, freed_slots, dev30_before, dev30_after);
        ps2xSafeLog("[hle] cid1 close fd=%u freed_slots=%u dev30_before=%u dev30_after=%u\n",
                    fd, freed_slots, dev30_before, dev30_after);
        std::printf("[hle] cid1 close fd=%u freed_slots=%u dev30_before=%u dev30_after=%u\n",
                    fd, freed_slots, dev30_before, dev30_after);
        std::fflush(stdout);
    }

    void onBpDev30Log(uint32_t dev30) {
        bhLog("[probe] bp dev30=%u\n", dev30);
        ps2xSafeLog("[probe] bp dev30=%u\n", dev30);
        std::printf("[probe] bp dev30=%u\n", dev30);
        std::fflush(stdout);
    }

    // Task BQ
    struct SemaRecordBQ {
        uint32_t lastSignalRa = 0;
        int lastSignalThread = 0;
        uint32_t lastWaitRa = 0;
        int lastWaitThread = 0;
    };
    static std::mutex s_semaBqMutex;
    static std::unordered_map<int, SemaRecordBQ> s_semaBqRecords;

    void recordSemaSignalBQ(int semaId, uint32_t ra, int threadId) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_semaBqMutex);
        auto &r = s_semaBqRecords[semaId];
        r.lastSignalRa = ra;
        r.lastSignalThread = threadId;
    }

    void recordSemaWaitBQ(int semaId, uint32_t ra, int threadId) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_semaBqMutex);
        auto &r = s_semaBqRecords[semaId];
        r.lastWaitRa = ra;
        r.lastWaitThread = threadId;
    }

    static std::atomic<uint32_t> s_idleSecCount{0};
    static std::atomic<uint32_t> s_lastDumpSec{0};
    static uint32_t s_bqTotalSec = 0;

    static const char *getThreadStatusStr(EeThreadStatus st) {
        switch (st) {
        case EeThreadStatus::Dormant: return "DORMANT";
        case EeThreadStatus::Ready: return "READY";
        case EeThreadStatus::Running: return "RUNNING";
        case EeThreadStatus::Waiting: return "WAIT";
        case EeThreadStatus::Suspended: return "SUSPEND";
        case EeThreadStatus::WaitingSuspended: return "WAIT_SUSPEND";
        default: return "UNKNOWN";
        }
    }

    static const char *getWaitReasonStr(EeWaitReason r) {
        switch (r) {
        case EeWaitReason::None: return "NONE";
        case EeWaitReason::Sleep: return "SLEEP";
        case EeWaitReason::Semaphore: return "SEMA";
        case EeWaitReason::EventFlag: return "EVENTFLAG";
        case EeWaitReason::VSync: return "VSYNC";
        case EeWaitReason::External: return "EXTERNAL";
        case EeWaitReason::Mpeg: return "MPEG";
        default: return "OTHER";
        }
    }

    static void doBqStallDump(const uint8_t *rdram) {
        bhLog("================ [probe] BQ EE IDLE STALL DUMP ================\n");
        ps2xSafeLog("================ [probe] BQ EE IDLE STALL DUMP ================\n");

        if (!s_bmScheduler) {
            bhLog("[probe] bq scheduler not bound\n");
            ps2xSafeLog("[probe] bq scheduler not bound\n");
            return;
        }

        // a. Guest threads
        bhLog("--- [probe] bq threads ---\n");
        ps2xSafeLog("--- [probe] bq threads ---\n");
        std::vector<int> blockedThreads;
        for (int id = 1; id < 256; ++id) {
            const GuestThread *th = s_bmScheduler->thread(id);
            if (!th || th->status == EeThreadStatus::Dormant) continue;
            const R5900Context &ctx = th->activeContext();
            uint32_t pc = ctx.pc;
            uint32_t ra = getRegU32(&ctx, 31);
            uint32_t sp = getRegU32(&ctx, 29);
            uint32_t a0 = getRegU32(&ctx, 4);
            uint32_t a1 = getRegU32(&ctx, 5);
            uint32_t a2 = getRegU32(&ctx, 6);
            uint32_t a3 = getRegU32(&ctx, 7);

            int waitPayloadId = 0;
            if (std::holds_alternative<EeSemaphoreWait>(th->wait.payload)) {
                waitPayloadId = std::get<EeSemaphoreWait>(th->wait.payload).id;
            } else if (std::holds_alternative<EeEventFlagWait>(th->wait.payload)) {
                waitPayloadId = std::get<EeEventFlagWait>(th->wait.payload).id;
            }

            bhLog("[probe] bq th id=%d entry=0x%08X prio=%d state=%s wait=%s(payload=%d) pc=0x%08X ra=0x%08X sp=0x%08X a0..a3=0x%08X,0x%08X,0x%08X,0x%08X\n",
                  id, th->entry, th->currentPriority, getThreadStatusStr(th->status),
                  getWaitReasonStr(th->wait.reason), waitPayloadId,
                  pc, ra, sp, a0, a1, a2, a3);
            ps2xSafeLog("[probe] bq th id=%d entry=0x%08X prio=%d state=%s wait=%s(payload=%d) pc=0x%08X ra=0x%08X sp=0x%08X a0..a3=0x%08X,0x%08X,0x%08X,0x%08X\n",
                        id, th->entry, th->currentPriority, getThreadStatusStr(th->status),
                        getWaitReasonStr(th->wait.reason), waitPayloadId,
                        pc, ra, sp, a0, a1, a2, a3);

            if (th->status == EeThreadStatus::Waiting || th->status == EeThreadStatus::WaitingSuspended) {
                blockedThreads.push_back(id);
            }
        }

        // b. Semaphores with waiters
        bhLog("--- [probe] bq semaphores with waiters ---\n");
        ps2xSafeLog("--- [probe] bq semaphores with waiters ---\n");
        uint32_t semaWaitersCount = 0;
        for (int id = 1; id < 256; ++id) {
            const EeSemaphore *sem = s_bmScheduler->semaphore(id);
            if (!sem || sem->waiters.empty()) continue;
            semaWaitersCount++;
            std::string waiterStr;
            for (int w : sem->waiters) {
                if (!waiterStr.empty()) waiterStr += ",";
                waiterStr += std::to_string(w);
            }
            uint32_t sigRa = 0, waitRa = 0;
            int sigTh = 0, waitTh = 0;
            {
                std::lock_guard<std::mutex> lock(s_semaBqMutex);
                auto it = s_semaBqRecords.find(id);
                if (it != s_semaBqRecords.end()) {
                    sigRa = it->second.lastSignalRa;
                    sigTh = it->second.lastSignalThread;
                    waitRa = it->second.lastWaitRa;
                    waitTh = it->second.lastWaitThread;
                }
            }
            bhLog("[probe] bq sema id=%d count=%d max=%d waiters=[%s] last_sig=ra:0x%08X/th:%d last_wait=ra:0x%08X/th:%d\n",
                  id, sem->count, sem->maxCount, waiterStr.c_str(), sigRa, sigTh, waitRa, waitTh);
            ps2xSafeLog("[probe] bq sema id=%d count=%d max=%d waiters=[%s] last_sig=ra:0x%08X/th:%d last_wait=ra:0x%08X/th:%d\n",
                        id, sem->count, sem->maxCount, waiterStr.c_str(), sigRa, sigTh, waitRa, waitTh);
        }
        if (semaWaitersCount == 0) {
            bhLog("[probe] bq sema with waiters: none\n");
            ps2xSafeLog("[probe] bq sema with waiters: none\n");
        }

        // c & d. Pending SIF RPC calls + SG2 dump
        bhLog("--- [probe] bq rpc & sg2 ---\n");
        ps2xSafeLog("--- [probe] bq rpc & sg2 ---\n");
        onBqDumpRpcAndSg2(rdram);

        // e. Stack walks of blocked threads (cap 20 lines each)
        bhLog("--- [probe] bq stack walks ---\n");
        ps2xSafeLog("--- [probe] bq stack walks ---\n");
        if (rdram) {
            for (int id : blockedThreads) {
                const GuestThread *th = s_bmScheduler->thread(id);
                if (!th) continue;
                const R5900Context &ctx = th->activeContext();
                uint32_t sp = getRegU32(&ctx, 29);
                bhLog("[probe] bq stack walk th=%d sp=0x%08X:\n", id, sp);
                ps2xSafeLog("[probe] bq stack walk th=%d sp=0x%08X:\n", id, sp);

                uint32_t retLines = 0;
                for (uint32_t off = 0; off <= 0x1800; off += 4) {
                    if (retLines >= 20) break;
                    uint32_t currAddr = sp + off;
                    if ((currAddr & PS2_RAM_MASK) + 4 > PS2_RAM_SIZE) continue;
                    uint32_t w = 0;
                    std::memcpy(&w, rdram + (currAddr & PS2_RAM_MASK), 4);

                    if (w >= 0x00100000u && w < 0x00600000u) {
                        uint32_t insnAddr = w - 8;
                        if (insnAddr >= 0x00100000u && (insnAddr & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                            uint32_t insn = 0;
                            std::memcpy(&insn, rdram + (insnAddr & PS2_RAM_MASK), 4);
                            if ((insn & 0xFC000000u) == 0x0C000000u) {
                                uint32_t callee = ((w - 4) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
                                bhLog("  [th %d walk %u] w=0x%08X at sp+0x%X callee=0x%08X\n", id, retLines, w, off, callee);
                                ps2xSafeLog("  [th %d walk %u] w=0x%08X at sp+0x%X callee=0x%08X\n", id, retLines, w, off, callee);
                                retLines++;
                            } else if ((insn & 0xFC00003Fu) == 0x00000009u) {
                                bhLog("  [th %d walk %u] w=0x%08X at sp+0x%X callee=jalr\n", id, retLines, w, off);
                                ps2xSafeLog("  [th %d walk %u] w=0x%08X at sp+0x%X callee=jalr\n", id, retLines, w, off);
                                retLines++;
                            }
                        }
                    }
                }
            }
        }
        bhLog("================================================================\n");
        ps2xSafeLog("================================================================\n");
    }

    static const uint32_t s_brVtableAddrs[25] = {
        0x004F131Cu, 0x004F14B0u, 0x004F1534u, 0x004F17B4u, 0x004F1818u,
        0x00497E64u, 0x00497E6Cu, 0x004F12B0u, 0x004F12D4u, 0x00497E74u,
        0x004F1300u, 0x004F12A0u, 0x004F1280u, 0x004F1200u, 0x004F1208u,
        0x004F126Cu, 0x004F11F0u, 0x004F11D0u, 0x004F10B4u, 0x004F11B4u,
        0x004F1100u, 0x004F1144u, 0x004F1178u, 0x004F10A4u, 0x004F1084u
    };

    static std::atomic<uint32_t> s_brFnSecCounts[25];
    static std::atomic<bool> s_brSceneActive{false};
    static uint32_t s_brSceneSec = 0;
    static std::atomic<uint32_t> s_brRetCount_131c{0};
    static std::atomic<uint32_t> s_brRetCount_14b0{0};
    static std::atomic<uint32_t> s_brRetCount_1534{0};

    static uint32_t s_prevSceneDump[96] = {0};
    static bool s_hasPrevSceneDump = false;

    void onBrFunctionEntry(uint32_t addr, const R5900Context *ctx) {
        s_brSceneActive.store(true, std::memory_order_relaxed);
        for (size_t i = 0; i < 25; ++i) {
            if (s_brVtableAddrs[i] == addr) {
                s_brFnSecCounts[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }

    void onBrFunctionExit(uint32_t addr, const R5900Context *ctx) {
        if (!ctx) return;
        uint32_t n = 0;
        if (addr == 0x004F131Cu) n = s_brRetCount_131c.fetch_add(1, std::memory_order_relaxed) + 1;
        else if (addr == 0x004F14B0u) n = s_brRetCount_14b0.fetch_add(1, std::memory_order_relaxed) + 1;
        else if (addr == 0x004F1534u) n = s_brRetCount_1534.fetch_add(1, std::memory_order_relaxed) + 1;

        if (n > 0 && n <= 20) {
            uint32_t v0 = getRegU32(ctx, 2);
            uint32_t ra = getRegU32(ctx, 31);
            bhLog("[probe] br ret addr=0x%08X n=%u v0=0x%08X ra=0x%08X\n", addr, n, v0, ra);
            ps2xSafeLog("[probe] br ret addr=0x%08X n=%u v0=0x%08X ra=0x%08X\n", addr, n, v0, ra);
        }
    }

    void onBrTickSec(const uint8_t *rdram) {
        if (!isEnabled()) return;
        if (!s_brSceneActive.load(std::memory_order_relaxed)) {
            if (g_trackedCurScene.load(std::memory_order_relaxed) == 0x00852F20u ||
                g_trackedCurVtable.load(std::memory_order_relaxed) == 0x006AE6E0u) {
                s_brSceneActive.store(true, std::memory_order_relaxed);
            }
        }
        if (!s_brSceneActive.load(std::memory_order_relaxed)) return;

        s_brSceneSec++;

        // 1. Per-second entry counters
        std::string countsStr;
        uint32_t distinct = 0;
        for (size_t i = 0; i < 25; ++i) {
            uint32_t c = s_brFnSecCounts[i].exchange(0, std::memory_order_relaxed);
            if (c > 0) {
                distinct++;
                char buf[64];
                std::snprintf(buf, sizeof(buf), " n(0x%08X)=%u", s_brVtableAddrs[i], c);
                countsStr += buf;
            }
        }
        bhLog("[probe] br fn sec=%u total_distinct=%u%s\n", s_brSceneSec, distinct, countsStr.c_str());
        ps2xSafeLog("[probe] br fn sec=%u total_distinct=%u%s\n", s_brSceneSec, distinct, countsStr.c_str());

        // 2. Every 5s dump scene object at 0x852F20 (0x180 bytes = 96 words)
        if (s_brSceneSec % 5 == 0 && rdram) {
            uint32_t currDump[96];
            const uint32_t sceneAddr = 0x00852F20u & PS2_RAM_MASK;
            if (sceneAddr + 0x180u <= PS2_RAM_SIZE) {
                std::memcpy(currDump, rdram + sceneAddr, 0x180);

                for (uint32_t line = 0; line < 12; ++line) {
                    uint32_t off = line * 32;
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                                  "[probe] br scene +0x%03X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                                  off,
                                  currDump[line * 8 + 0], currDump[line * 8 + 1],
                                  currDump[line * 8 + 2], currDump[line * 8 + 3],
                                  currDump[line * 8 + 4], currDump[line * 8 + 5],
                                  currDump[line * 8 + 6], currDump[line * 8 + 7]);
                    bhLog("%s", buf);
                    ps2xSafeLog("%s", buf);
                }

                if (s_hasPrevSceneDump) {
                    for (uint32_t i = 0; i < 96; ++i) {
                        if (currDump[i] != s_prevSceneDump[i]) {
                            bhLog("[probe] br scene delta +0x%03X: 0x%08X -> 0x%08X\n",
                                  i * 4u, s_prevSceneDump[i], currDump[i]);
                            ps2xSafeLog("[probe] br scene delta +0x%03X: 0x%08X -> 0x%08X\n",
                                        i * 4u, s_prevSceneDump[i], currDump[i]);
                        }
                    }
                }
                std::memcpy(s_prevSceneDump, currDump, 0x180);
                s_hasPrevSceneDump = true;
            }
        }

        // 3. Ring at 10s and 30s
        if (s_brSceneSec == 10 || s_brSceneSec == 30) {
            std::lock_guard<std::mutex> lock(s_brRingMutex);
            std::string ringStr;
            uint32_t count = std::min(s_brMainRingIdx, 24u);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t idx = (s_brMainRingIdx >= 24) ? (s_brMainRingIdx - 24 + i) : i;
                uint32_t pc = s_brMainRing[idx % 24];
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%s0x%08X", (i > 0 ? " " : ""), pc);
                ringStr += buf;
            }
            bhLog("[probe] br main_ring sec=%u: %s\n", s_brSceneSec, ringStr.c_str());
            ps2xSafeLog("[probe] br main_ring sec=%u: %s\n", s_brSceneSec, ringStr.c_str());
        }
    }

    void onBqTickSec(const uint8_t *rdram, double ee_ms) {
        if (!isEnabled()) return;
        onBrTickSec(rdram);
        onBtTickSec(rdram);
        onBuTickSec(rdram);
        onBvTickSec();
        onBwTickSec();
        onBxTickSec(rdram);
        onBzTickSec();
        onCaTickSec();
        onCcTickSec();
        onCeTickSec();
        s_bqTotalSec++;

        uint32_t running = 0, blocked = 0;
        if (s_bmScheduler) {
            for (int id = 1; id < 256; ++id) {
                const GuestThread *th = s_bmScheduler->thread(id);
                if (!th || th->status == EeThreadStatus::Dormant) continue;
                if (th->status == EeThreadStatus::Running || th->status == EeThreadStatus::Ready) {
                    running++;
                } else if (th->status == EeThreadStatus::Waiting || th->status == EeThreadStatus::WaitingSuspended) {
                    blocked++;
                }
            }
        }

        uint32_t sif_pending = 0, sg2_polls = 0, cid1_req = 0;
        onBqGetStats(sif_pending, sg2_polls, cid1_req);

        bhLog("[probe] bq sec threads_running=%u blocked=%u sif_pending=%u sg2_polls=%u cid1_req=%u\n",
              running, blocked, sif_pending, sg2_polls, cid1_req);
        ps2xSafeLog("[probe] bq sec threads_running=%u blocked=%u sif_pending=%u sg2_polls=%u cid1_req=%u\n",
                    running, blocked, sif_pending, sg2_polls, cid1_req);

        // EE idle detector: ee_ms <= 0.05 consecutive seconds >= 3
        if (ee_ms <= 0.05) {
            uint32_t idle = s_idleSecCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (idle >= 3) {
                uint32_t lastDump = s_lastDumpSec.load(std::memory_order_relaxed);
                if (lastDump == 0 || (s_bqTotalSec - lastDump >= 10)) {
                    s_lastDumpSec.store(s_bqTotalSec, std::memory_order_relaxed);
                    doBqStallDump(rdram);
                }
            }
        } else {
            s_idleSecCount.store(0, std::memory_order_relaxed);
        }
    }

    // =========================================================================
    // Task BT Probes
    // =========================================================================
    static std::atomic<uint32_t> s_btFCalls{0};
    static std::atomic<uint32_t> s_btGCalls{0};
    static std::map<uint32_t, std::atomic<uint32_t>> s_btHookCounts;
    static std::atomic<uint32_t> s_lastMovieStateBT{0};

    static std::string getBtMainRing() {
        std::lock_guard<std::mutex> lock(s_brRingMutex);
        std::string s;
        uint32_t count = std::min(s_brMainRingIdx, 24u);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = (s_brMainRingIdx >= 24) ? (s_brMainRingIdx - 24 + i) : i;
            uint32_t pc = s_brMainRing[idx % 24];
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s0x%08X", (i > 0 ? " " : ""), pc);
            s += buf;
        }
        return s;
    }

    static void checkBtState10(const uint8_t *rdram) {
        if (!isEnabled() || !rdram) return;
        uint32_t st = 0;
        std::memcpy(&st, rdram + (0x772188u & PS2_RAM_MASK), 4);
        uint32_t oldSt = s_lastMovieStateBT.exchange(st, std::memory_order_relaxed);

        if (st == 10 && oldSt != 10) {
            bhLog("================ [probe] BT MOVIE STATE 9->10 DUMP ================\n");
            ps2xSafeLog("================ [probe] BT MOVIE STATE 9->10 DUMP ================\n");

            // 1. Context words
            static const uint32_t ctxOffsets[] = {
                0x08, 0x0C, 0x10, 0x14, 0x18, 0x48, 0x4C, 0x50, 0x58, 0x5C,
                0x60, 0x64, 0x68, 0x6C, 0x70, 0x7C, 0x84, 0x88, 0xC8, 0xCC,
                0xD8, 0xE0, 0xE4, 0xE8
            };
            std::string ctxStr;
            for (uint32_t off : ctxOffsets) {
                uint32_t val = 0;
                std::memcpy(&val, rdram + ((0x772180u + off) & PS2_RAM_MASK), 4);
                char buf[32];
                std::snprintf(buf, sizeof(buf), " +%02X=0x%08X", off, val);
                ctxStr += buf;
            }
            bhLog("[probe] bt change ctx:%s\n", ctxStr.c_str());
            ps2xSafeLog("[probe] bt change ctx:%s\n", ctxStr.c_str());

            // 2. Command ring: head [+0x228], tail [+0x22C], entries from +0xA0
            uint32_t head = 0, tail = 0;
            std::memcpy(&head, rdram + ((0x1900E00u + 0x228u) & PS2_RAM_MASK), 4);
            std::memcpy(&tail, rdram + ((0x1900E00u + 0x22Cu) & PS2_RAM_MASK), 4);
            bhLog("[probe] bt change cmdq head=%u tail=%u\n", head, tail);
            ps2xSafeLog("[probe] bt change cmdq head=%u tail=%u\n", head, tail);

            for (uint32_t i = 0; i < 32; ++i) {
                uint32_t eAddr = (0x1900E00u + 0x0A0u + i * 12u) & PS2_RAM_MASK;
                uint32_t w0 = 0, w1 = 0, w2 = 0;
                std::memcpy(&w0, rdram + eAddr + 0, 4);
                std::memcpy(&w1, rdram + eAddr + 4, 4);
                std::memcpy(&w2, rdram + eAddr + 8, 4);
                if (w0 != 0 || w1 != 0 || w2 != 0 || i == head || i == tail) {
                    bhLog("[probe] bt change cmdq[%u]: w0=0x%08X w1=0x%08X w2=0x%08X\n", i, w0, w1, w2);
                    ps2xSafeLog("[probe] bt change cmdq[%u]: w0=0x%08X w1=0x%08X w2=0x%08X\n", i, w0, w1, w2);
                }
            }

            // 3. Last 24 main thread function entries
            std::string ring = getBtMainRing();
            bhLog("[probe] bt change main_ring: %s\n", ring.c_str());
            ps2xSafeLog("[probe] bt change main_ring: %s\n", ring.c_str());
        }
    }

    
    // =========================================================================
    // Task DO: Producer 0x3F7BA8 enqueue amounts probe
    // =========================================================================
    static std::atomic<uint32_t> s_dc10LogCount{0};

    static bool checkDoGate(const uint8_t *rdram) {
        if (!isDcProbeEnabled() || !rdram) return false;
        uint64_t gtick = 0;
        std::memcpy(&gtick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);
        if (gtick < 5010 || gtick > 5045) return false; // window around the g241 burst (tick 5035, deterministic)
        return (s_dc10LogCount.fetch_add(1, std::memory_order_relaxed) < 600);
    }

    void onDoProdEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!checkDoGate(rdram) || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint64_t tick = 0;
        std::memcpy(&tick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);
        uint32_t state = 0, last2D4 = 0, r230 = 0, r234 = 0, r244 = 0, r248 = 0, head = 0, tail = 0;
        uint64_t mask = 0;
        if ((a0 & 0x1FFFFFFFu) + 0x2E0 <= 32 * 1024 * 1024) {
            std::memcpy(&state, rdram + ((a0 + 0x00u) & 0x1FFFFFFFu), 4);
            std::memcpy(&last2D4, rdram + ((a0 + 0x2D4u) & 0x1FFFFFFFu), 4);
            std::memcpy(&r230, rdram + ((a0 + 0x230u) & 0x1FFFFFFFu), 4);
            std::memcpy(&r234, rdram + ((a0 + 0x234u) & 0x1FFFFFFFu), 4);
            std::memcpy(&r244, rdram + ((a0 + 0x244u) & 0x1FFFFFFFu), 4);
            std::memcpy(&r248, rdram + ((a0 + 0x248u) & 0x1FFFFFFFu), 4);
            std::memcpy(&mask, rdram + ((a0 + 0xA0u) & 0x1FFFFFFFu), 8);
            std::memcpy(&head, rdram + ((a0 + 0x228u) & 0x1FFFFFFFu), 4);
            std::memcpy(&tail, rdram + ((a0 + 0x22Cu) & 0x1FFFFFFFu), 4);
        }
        std::printf("[probe] dc10 prod tick=%llu obj=0x%08X state=%u last2D4=%u r230=%u r234=%u r244=%u r248=%u mask=0x%llX head=%u tail=%u\n",
                    (unsigned long long)tick, a0, state, last2D4, r230, r234, r244, r248, (unsigned long long)mask, head, tail);
        std::fflush(stdout);
    }

    void onDoEnqEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!checkDoGate(rdram) || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        uint64_t tick = 0;
        std::memcpy(&tick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);
        uint32_t last2D4 = 0, head = 0, tail = 0;
        if ((a0 & 0x1FFFFFFFu) + 0x2E0 <= 32 * 1024 * 1024) {
            std::memcpy(&last2D4, rdram + ((a0 + 0x2D4u) & 0x1FFFFFFFu), 4);
            std::memcpy(&head, rdram + ((a0 + 0x228u) & 0x1FFFFFFFu), 4);
            std::memcpy(&tail, rdram + ((a0 + 0x22Cu) & 0x1FFFFFFFu), 4);
        }
        std::printf("[probe] dc10 enq tick=%llu a1=%u a2=0x%08X a3=%u last2D4=%u head=%u tail=%u\n",
                    (unsigned long long)tick, a1, a2, a3, last2D4, head, tail);
        std::fflush(stdout);
    }

    void onDoPosEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!checkDoGate(rdram) || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint64_t tick = 0;
        std::memcpy(&tick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);
        std::printf("[probe] dc10 pos tick=%llu a0=%u a1=%u\n",
                    (unsigned long long)tick, a0, a1);
        std::fflush(stdout);
    }

    void onDoRunEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!checkDoGate(rdram) || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint64_t tick = 0;
        std::memcpy(&tick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);
        uint32_t head = 0, tail = 0;
        if ((a0 & 0x1FFFFFFFu) + 0x2E0 <= 32 * 1024 * 1024) {
            std::memcpy(&head, rdram + ((a0 + 0x228u) & 0x1FFFFFFFu), 4);
            std::memcpy(&tail, rdram + ((a0 + 0x22Cu) & 0x1FFFFFFFu), 4);
        }
        std::printf("[probe] dc10 run tick=%llu head=%u tail=%u\n",
                    (unsigned long long)tick, head, tail);
        std::fflush(stdout);
    }

    
    // =========================================================================
    // Task DQ: Movie dispatcher blocking diagnostics during g203 stall
    // =========================================================================
    static std::atomic<uint32_t> s_dqTotalLogCount{0};
    static std::atomic<uint32_t> s_dqDa880Count{0};
    static std::atomic<uint32_t> s_dqDd010Count{0};

    static bool checkDqGate(const uint8_t *rdram, uint64_t *outTick) {
        if (!isDcProbeEnabled() || !rdram) return false;
        uint64_t tick = 0;
        std::memcpy(&tick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);
        if (outTick) *outTick = tick;
        if (tick < 4255 || tick > 4385) return false;
        return (s_dqTotalLogCount.fetch_add(1, std::memory_order_relaxed) < 400);
    }

    void onDqCbfcEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        uint64_t tick = 0;
        if (!checkDqGate(rdram, &tick) || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        int32_t c0C = 0, c10 = 0;
        uint32_t st = 0, dev30 = 0;
        std::memcpy(&c0C, rdram + ((a0 + 0x0Cu) & 0x1FFFFFFFu), 4);
        std::memcpy(&c10, rdram + ((a0 + 0x10u) & 0x1FFFFFFFu), 4);
        std::memcpy(&st, rdram + ((a0 + 0x08u) & 0x1FFFFFFFu), 4);
        std::memcpy(&dev30, rdram + (0x771BB8u & 0x1FFFFFFFu), 4);

        auto read4W = [&](uint32_t addr) -> std::string {
            char buf[64];
            uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
            std::memcpy(&w0, rdram + ((addr + 0) & 0x1FFFFFFFu), 4);
            std::memcpy(&w1, rdram + ((addr + 4) & 0x1FFFFFFFu), 4);
            std::memcpy(&w2, rdram + ((addr + 8) & 0x1FFFFFFFu), 4);
            std::memcpy(&w3, rdram + ((addr + 12) & 0x1FFFFFFFu), 4);
            std::snprintf(buf, sizeof(buf), "[0x%08X,0x%08X,0x%08X,0x%08X]", w0, w1, w2, w3);
            return std::string(buf);
        };
        std::string s0w = read4W(0x771BBCu);
        std::string s1w = read4W(0x771BCCu);
        std::string s2w = read4W(0x771BDCu);
        std::string s3w = read4W(0x771BECu);

        std::printf("[probe] dc11 cbfc tick=%llu c0C=%d c10=%d st=%u dev30=0x%08X s0w=%s s1w=%s s2w=%s s3w=%s\n",
                    (unsigned long long)tick, c0C, c10, st, dev30, s0w.c_str(), s1w.c_str(), s2w.c_str(), s3w.c_str());
        std::fflush(stdout);
    }

    void onDqDa880Entry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (s_dqDa880Count.fetch_add(1, std::memory_order_relaxed) >= 150) return;
        uint64_t tick = 0;
        if (!checkDqGate(rdram, &tick) || !ctx) return;
        int32_t h = static_cast<int32_t>(getRegU32(ctx, 5));
        uint32_t wait = getRegU32(ctx, 6);
        uint32_t ra = getRegU32(ctx, 31);
        std::printf("[probe] dc11 da880 tick=%llu h=%d wait=%u ra=0x%08X\n",
                    (unsigned long long)tick, h, wait, ra);
        std::fflush(stdout);
    }

    void onDqDd010Entry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (s_dqDd010Count.fetch_add(1, std::memory_order_relaxed) >= 100) return;
        uint64_t tick = 0;
        if (!checkDqGate(rdram, &tick) || !ctx) return;
        uint32_t ra = getRegU32(ctx, 31);
        std::printf("[probe] dc11 dd010 tick=%llu ra=0x%08X\n",
                    (unsigned long long)tick, ra);
        std::fflush(stdout);
    }

    void onDqPumpEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        uint64_t tick = 0;
        if (!checkDqGate(rdram, &tick)) return;
        uint32_t st = 0, b64 = 0, b60 = 0, b68 = 0, b6c = 0, n58 = 0, f84 = 0;
        int32_t c0C = 0, c10 = 0;
        std::memcpy(&st, rdram + ((0x772180u + 0x08u) & 0x1FFFFFFFu), 4);
        std::memcpy(&b64, rdram + ((0x772180u + 0x64u) & 0x1FFFFFFFu), 4);
        std::memcpy(&b60, rdram + ((0x772180u + 0x60u) & 0x1FFFFFFFu), 4);
        std::memcpy(&b68, rdram + ((0x772180u + 0x68u) & 0x1FFFFFFFu), 4);
        std::memcpy(&b6c, rdram + ((0x772180u + 0x6Cu) & 0x1FFFFFFFu), 4);
        std::memcpy(&n58, rdram + ((0x772180u + 0x58u) & 0x1FFFFFFFu), 4);
        std::memcpy(&c0C, rdram + ((0x772180u + 0x0Cu) & 0x1FFFFFFFu), 4);
        std::memcpy(&c10, rdram + ((0x772180u + 0x10u) & 0x1FFFFFFFu), 4);
        std::memcpy(&f84, rdram + ((0x772180u + 0x84u) & 0x1FFFFFFFu), 4);
        std::printf("[probe] dc11 pump tick=%llu st=%u b64=%u b60=%u b68=%u b6c=%u n58=%u c0C=%d c10=%d f84=%u\n",
                    (unsigned long long)tick, st, b64, b60, b68, b6c, n58, c0C, c10, f84);
        std::fflush(stdout);
    }

    void onBtFunctionEntry(uint32_t addr, const R5900Context *ctx, const uint8_t *rdram) {
        if (addr == 0x0024CBFCu) { onDqCbfcEntry(ctx, rdram); }
        if (addr == 0x001DA880u) { onDqDa880Entry(ctx, rdram); }
        if (addr == 0x001DD010u) { onDqDd010Entry(ctx, rdram); }
        if (addr == 0x003F7BA8u) { onDoProdEntry(ctx, rdram); }
        if (addr == 0x00402888u) { onDoPosEntry(ctx, rdram); }
        if (addr == 0x003F7A18u) { onDoRunEntry(ctx, rdram); }
        if (addr == 0x0024CB10u) { onDhVideoReadEntry(ctx, rdram); }
        if (addr == 0x0024CE28u) {
            onDc4GEntry(ctx, rdram);
        } else if (addr == 0x0024C164u) {
            onDc4FEntry(ctx, rdram);
        }
        if (!isEnabled()) return;
        if (addr == 0x0024C164u) {
            s_btFCalls.fetch_add(1, std::memory_order_relaxed);
            if (rdram) {
                checkBtState10(rdram);
            }
        } else if (addr == 0x0024CE28u) {
            s_btGCalls.fetch_add(1, std::memory_order_relaxed);
        }

        uint32_t n = s_btHookCounts[addr].fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10) {
            uint32_t a0 = ctx ? getRegU32(ctx, 4) : 0;
            uint32_t a1 = ctx ? getRegU32(ctx, 5) : 0;
            uint32_t a2 = ctx ? getRegU32(ctx, 6) : 0;
            uint32_t a3 = ctx ? getRegU32(ctx, 7) : 0;
            uint32_t ra = ctx ? getRegU32(ctx, 31) : 0;
            bhLog("[probe] bt hook 0x%08X n=%u a0=0x%08X a1=%u(0x%X) a2=0x%08X a3=0x%08X ra=0x%08X\n",
                  addr, n, a0, a1, a1, a2, a3, ra);
            ps2xSafeLog("[probe] bt hook 0x%08X n=%u a0=0x%08X a1=%u(0x%X) a2=0x%08X a3=0x%08X ra=0x%08X\n",
                        addr, n, a0, a1, a1, a2, a3, ra);
        }
    }

    void onBtTickSec(const uint8_t *rdram) {
        if (!isEnabled() || !rdram) return;
        checkBtState10(rdram);

        uint32_t st = 0;
        std::memcpy(&st, rdram + (0x772188u & PS2_RAM_MASK), 4);
        if (st == 9) {
            uint32_t s60 = 0, s64 = 0, s5C = 0, s70 = 0;
            std::memcpy(&s60, rdram + ((0x772180u + 0x60u) & PS2_RAM_MASK), 4);
            std::memcpy(&s64, rdram + ((0x772180u + 0x64u) & PS2_RAM_MASK), 4);
            std::memcpy(&s5C, rdram + ((0x772180u + 0x5Cu) & PS2_RAM_MASK), 4);
            std::memcpy(&s70, rdram + ((0x772180u + 0x70u) & PS2_RAM_MASK), 4);

            uint32_t head = 0, tail = 0;
            std::memcpy(&head, rdram + ((0x1900E00u + 0x228u) & PS2_RAM_MASK), 4);
            std::memcpy(&tail, rdram + ((0x1900E00u + 0x22Cu) & PS2_RAM_MASK), 4);

            uint32_t g_calls = s_btGCalls.exchange(0, std::memory_order_relaxed);
            uint32_t f_calls = s_btFCalls.exchange(0, std::memory_order_relaxed);

            bhLog("[probe] bt sec s60=0x%X s64=0x%X s5C=%u s70=0x%X cmdq_head=%u cmdq_tail=%u g_calls=%u f_calls=%u\n",
                  s60, s64, s5C, s70, head, tail, g_calls, f_calls);
            ps2xSafeLog("[probe] bt sec s60=0x%X s64=0x%X s5C=%u s70=0x%X cmdq_head=%u cmdq_tail=%u g_calls=%u f_calls=%u\n",
                        s60, s64, s5C, s70, head, tail, g_calls, f_calls);

            if (head != tail) {
                for (uint32_t idx = tail; idx != head; idx = (idx + 1) % 32) {
                    uint32_t eAddr = (0x1900E00u + 0x0A0u + idx * 12u) & PS2_RAM_MASK;
                    uint32_t w0 = 0, w1 = 0, w2 = 0;
                    std::memcpy(&w0, rdram + eAddr + 0, 4);
                    std::memcpy(&w1, rdram + eAddr + 4, 4);
                    std::memcpy(&w2, rdram + eAddr + 8, 4);
                    bhLog("[probe] bt sec cmdq[%u]: w0=0x%08X w1=0x%08X w2=0x%08X\n", idx, w0, w1, w2);
                    ps2xSafeLog("[probe] bt sec cmdq[%u]: w0=0x%08X w1=0x%08X w2=0x%08X\n", idx, w0, w1, w2);
                }
            }
        }
    }

    // =========================================================================
    // Task BU: Movie group pump 0x24C7DC wrapper and EE hot-spot sampler
    // =========================================================================
    extern "C" uint32_t probe_getNewestDistinctFn();

    static std::atomic<uint32_t> s_buPumpCalls{0};
    static std::atomic<uint32_t> s_buLastCtx{0x772180u};
    static std::atomic<bool> s_buEnd0Logged{false};

    static std::atomic<bool> s_buSamplerRunning{false};
    static std::thread s_buSamplerThread;
    static std::mutex s_buHotMutex;
    static std::unordered_map<uint32_t, uint32_t> s_buHotCounts;
    static uint32_t s_buHotTotalSamples = 0;

    static void buSamplerWorker() {
        while (s_buSamplerRunning.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            uint32_t fn = probe_getNewestDistinctFn();
            if (fn != 0) {
                std::lock_guard<std::mutex> lock(s_buHotMutex);
                s_buHotCounts[fn]++;
                s_buHotTotalSamples++;
            }
        }
    }

    static void ensureBuSamplerStarted() {
        if (!isEnabled()) return;
        static std::once_flag flag;
        std::call_once(flag, []() {
            s_buSamplerRunning.store(true, std::memory_order_relaxed);
            s_buSamplerThread = std::thread(buSamplerWorker);
        });
    }

    void onBuPumpEntry(const struct R5900Context *ctx, const uint8_t *rdram, BuPreState *pre) {
        // Task DS: dc12 pump race test hook (cap 400)
        if (isDcProbeEnabled() && rdram) {
            static std::atomic<uint32_t> s_dc12PumpCount{0};
            if (s_dc12PumpCount.fetch_add(1, std::memory_order_relaxed) < 400) {
                uint64_t tick = 0;
                std::memcpy(&tick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);
                int curTh = -1;
                const char *th1Status = "none";
                uint32_t th1Pc = 0, th1Ra = 0;
                ps2_getDkThread1Info(&curTh, &th1Status, &th1Pc, &th1Ra);
                int inWindow = (th1Pc >= 0x0024C6D4u && th1Pc <= 0x0024C720u) ? 1 : 0;
                uint32_t g74 = 0, p4c = 0, p70 = 0;
                std::memcpy(&g74, rdram + ((0x772180u + 0x74u) & 0x1FFFFFFFu), 4);
                std::memcpy(&p4c, rdram + ((0x772180u + 0x4Cu) & 0x1FFFFFFFu), 4);
                std::memcpy(&p70, rdram + ((0x772180u + 0x70u) & 0x1FFFFFFFu), 4);
                std::printf("[probe] dc12 pump tick=%llu cur_th=%d th1_status=%s th1_pc=0x%08X th1_ra=0x%08X in_window=%d g74=%u p4c=0x%08X p70=%u\n",
                            (unsigned long long)tick, curTh, th1Status, th1Pc, th1Ra, inWindow, g74, p4c, p70);
                std::fflush(stdout);
            }
        }

        onDqPumpEntry(ctx, rdram);
        onDhPumpEntry(ctx, rdram);
        if (!isEnabled() || !ctx || !rdram || !pre) return;
        ensureBuSamplerStarted();
        s_buPumpCalls.fetch_add(1, std::memory_order_relaxed);

        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t ra = getRegU32(ctx, 31);
        pre->a0 = a0;
        pre->a1 = a1;
        pre->ra = ra;
        s_buLastCtx.store(a0, std::memory_order_relaxed);

        uint32_t preState = 0;
        std::memcpy(&preState, rdram + ((a0 + 0x08u) & PS2_RAM_MASK), 4);
        pre->preState = preState;

        int32_t idC = -1, id10 = -1;
        std::memcpy(&idC, rdram + ((a0 + 0x0Cu) & PS2_RAM_MASK), 4);
        std::memcpy(&id10, rdram + ((a0 + 0x10u) & PS2_RAM_MASK), 4);
        pre->slotC_id = idC;
        pre->slot10_id = id10;

        if (idC >= 0) {
            uint32_t slotAddr = (0x771BBCu + 16u * (uint32_t)idC) & PS2_RAM_MASK;
            std::memcpy(pre->slotC_w, rdram + slotAddr, 16);
        } else {
            std::memset(pre->slotC_w, 0, sizeof(pre->slotC_w));
        }

        if (id10 >= 0) {
            uint32_t slotAddr = (0x771BBCu + 16u * (uint32_t)id10) & PS2_RAM_MASK;
            std::memcpy(pre->slot10_w, rdram + slotAddr, 16);
        } else {
            std::memset(pre->slot10_w, 0, sizeof(pre->slot10_w));
        }
    }

    
    // =========================================================================
    // Task DM: Group descriptor fields per pump
    // =========================================================================
    static std::atomic<uint32_t> s_dmPumpLogCount{0};

    static void dumpDmState(const uint8_t *rdram, const char *tag) {
        if (!isDcProbeEnabled()) return;
        auto readU32 = [&](uint32_t addr) -> uint32_t {
            if (!rdram || (addr & 0x1FFFFFFFu) + 4 > 32 * 1024 * 1024) return 0;
            uint32_t v = 0;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFFu), 4);
            return v;
        };
        uint64_t tick = 0;
        if (rdram) std::memcpy(&tick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);

        uint32_t ctx = 0x772180u;
        uint32_t g74 = readU32(ctx + 0x74);
        uint32_t p48 = readU32(ctx + 0x48);
        uint32_t w0 = p48 ? readU32(p48 + 0x00) : 0;
        uint32_t w1 = p48 ? readU32(p48 + 0x04) : 0;
        uint32_t w2 = p48 ? readU32(p48 + 0x08) : 0;
        uint32_t w3 = p48 ? readU32(p48 + 0x0C) : 0;
        uint32_t p4c = readU32(ctx + 0x4C);
        uint32_t r4c_10 = p4c ? readU32(p4c + 0x10) : 0;
        uint32_t r4c_14 = p4c ? readU32(p4c + 0x14) : 0;
        uint32_t f50 = readU32(ctx + 0x50);
        uint32_t f54 = readU32(ctx + 0x54);
        uint32_t n58 = readU32(ctx + 0x58);
        uint32_t f5c = readU32(ctx + 0x5C);
        uint32_t f60 = readU32(ctx + 0x60);
        uint32_t f64 = readU32(ctx + 0x64);
        uint32_t f68 = readU32(ctx + 0x68);
        uint32_t f6c = readU32(ctx + 0x6C);
        uint32_t p70 = readU32(ctx + 0x70);

        if (std::strcmp(tag, "pump") == 0) {
            std::printf("[probe] dc8 pump g74=%u p48=0x%08X w0=%u w1=%u w2=%u w3=%u p4c=0x%08X r4c_10=%u r4c_14=%u f50=%u f54=%u n58=%u f5c=%u f60=%u f64=%u f68=%u f6c=%u p70=%u tick=%llu\n",
                        g74, p48, w0, w1, w2, w3, p4c, r4c_10, r4c_14,
                        f50, f54, n58, f5c, f60, f64, f68, f6c, p70, (unsigned long long)tick);
        } else {
            std::printf("[probe] dc8 stall g74=%u p48=0x%08X w0=%u w1=%u w2=%u w3=%u p4c=0x%08X r4c_10=%u r4c_14=%u f50=%u f54=%u n58=%u f5c=%u f60=%u f64=%u f68=%u f6c=%u p70=%u tick=%llu\n",
                        g74, p48, w0, w1, w2, w3, p4c, r4c_10, r4c_14,
                        f50, f54, n58, f5c, f60, f64, f68, f6c, p70, (unsigned long long)tick);
        }

        if (std::strcmp(tag, "pump") == 0) {
            char hex48[512] = "";
            for (int i = 0; i < 16; ++i) {
                char b[32];
                std::snprintf(b, sizeof(b), "%s0x%08X", (i > 0 ? " " : ""), p48 ? readU32(p48 + i * 4) : 0);
                std::strcat(hex48, b);
            }
            std::printf("[probe] dc8 hdr g74=%u a=0x%08X w=%s\n", g74, p48, hex48);

            char hex4c[512] = "";
            for (int i = 0; i < 16; ++i) {
                char b[32];
                std::snprintf(b, sizeof(b), "%s0x%08X", (i > 0 ? " " : ""), p4c ? readU32(p4c + i * 4) : 0);
                std::strcat(hex4c, b);
            }
            std::printf("[probe] dc8 hdr4c g74=%u a=0x%08X w=%s\n", g74, p4c, hex4c);
        } else if (std::strcmp(tag, "stall") == 0) {
            for (int row = 0; row < 2; ++row) {
                char buf[512] = "";
                for (int i = 0; i < 16; ++i) {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%s0x%08X", (i > 0 ? " " : ""), p48 ? readU32(p48 + (row * 16 + i) * 4) : 0);
                    std::strcat(buf, b);
                }
                std::printf("[probe] dc8 stall_p48 row=%d a=0x%08X w=%s\n", row, p48 + row * 64, buf);
            }
            for (int row = 0; row < 2; ++row) {
                char buf[512] = "";
                for (int i = 0; i < 16; ++i) {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%s0x%08X", (i > 0 ? " " : ""), p4c ? readU32(p4c + (row * 16 + i) * 4) : 0);
                    std::strcat(buf, b);
                }
                std::printf("[probe] dc8 stall_p4c row=%d a=0x%08X w=%s\n", row, p4c + row * 64, buf);
            }
        }
        std::fflush(stdout);
    }

    void onBuPumpExit(const struct R5900Context *ctx, const uint8_t *rdram, const BuPreState *pre) {
        if (isDcProbeEnabled()) {
            if (s_dmPumpLogCount.fetch_add(1, std::memory_order_relaxed) < 400) {
                dumpDmState(rdram, "pump");
            }
        }

        if (!isEnabled() || !ctx || !rdram || !pre) return;

        uint32_t a0 = pre->a0;
        uint32_t postState = 0;
        std::memcpy(&postState, rdram + ((a0 + 0x08u) & PS2_RAM_MASK), 4);

        if (pre->preState != 10 && postState == 10) {
            bool expected = false;
            if (s_buEnd0Logged.compare_exchange_strong(expected, true)) {
                // Read context words: [+0x08,0x0C,0x10,0x14,0x18,0x1C,0x48,0x4C,0x50,0x58,0x5C,0x60,0x64,0x68,0x6C,0x70,0x74,0x78,0x7C,0x84]
                static const uint32_t offsets[] = {
                    0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x48, 0x4C, 0x50, 0x58,
                    0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x84
                };
                char ctxWordsBuf[512];
                int cpos = 0;
                for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
                    uint32_t val = 0;
                    std::memcpy(&val, rdram + ((a0 + offsets[i]) & PS2_RAM_MASK), 4);
                    cpos += std::snprintf(ctxWordsBuf + cpos, sizeof(ctxWordsBuf) - cpos,
                                         "+0x%02X=0x%08X%s", offsets[i], val,
                                         (i + 1 < sizeof(offsets)/sizeof(offsets[0]) ? " " : ""));
                }

                bhLog("[probe] bu end0 pre=%u post=10 ra=0x%08X a0=0x%08X a1=0x%08X %s\n",
                      pre->preState, pre->ra, a0, pre->a1, ctxWordsBuf);
                ps2xSafeLog("[probe] bu end0 pre=%u post=10 ra=0x%08X a0=0x%08X a1=0x%08X %s\n",
                            pre->preState, pre->ra, a0, pre->a1, ctxWordsBuf);

                // Slot words
                bhLog("[probe] bu slotw idC=%d w=[0x%08X 0x%08X 0x%08X 0x%08X] id10=%d w=[0x%08X 0x%08X 0x%08X 0x%08X]\n",
                      pre->slotC_id, pre->slotC_w[0], pre->slotC_w[1], pre->slotC_w[2], pre->slotC_w[3],
                      pre->slot10_id, pre->slot10_w[0], pre->slot10_w[1], pre->slot10_w[2], pre->slot10_w[3]);
                ps2xSafeLog("[probe] bu slotw idC=%d w=[0x%08X 0x%08X 0x%08X 0x%08X] id10=%d w=[0x%08X 0x%08X 0x%08X 0x%08X]\n",
                            pre->slotC_id, pre->slotC_w[0], pre->slotC_w[1], pre->slotC_w[2], pre->slotC_w[3],
                            pre->slot10_id, pre->slot10_w[0], pre->slot10_w[1], pre->slot10_w[2], pre->slot10_w[3]);

                // Header info
                uint32_t idx = 0;
                std::memcpy(&idx, rdram + ((a0 + 0x50u) & PS2_RAM_MASK), 4);

                char bufHex[4][65];
                for (int i = 0; i < 4; ++i) {
                    uint32_t bufPtr = 0;
                    std::memcpy(&bufPtr, rdram + ((a0 + 0x34u + 4u * i) & PS2_RAM_MASK), 4);
                    int bpos = 0;
                    if (bufPtr != 0) {
                        for (int b = 0; b < 32; ++b) {
                            uint8_t byteVal = rdram[(bufPtr + b) & PS2_RAM_MASK];
                            bpos += std::snprintf(bufHex[i] + bpos, sizeof(bufHex[i]) - bpos, "%02x", byteVal);
                        }
                    } else {
                        std::snprintf(bufHex[i], sizeof(bufHex[i]), "null");
                    }
                }

                char magicHex[33];
                int mpos = 0;
                for (int b = 0; b < 16; ++b) {
                    uint8_t byteVal = rdram[(0x6EAFF0u + b) & PS2_RAM_MASK];
                    mpos += std::snprintf(magicHex + mpos, sizeof(magicHex) - mpos, "%02x", byteVal);
                }

                uint32_t p1C = 0, deref10 = 0;
                std::memcpy(&p1C, rdram + ((a0 + 0x1Cu) & PS2_RAM_MASK), 4);
                if (p1C != 0) {
                    std::memcpy(&deref10, rdram + ((p1C + 0x10u) & PS2_RAM_MASK), 4);
                }

                bhLog("[probe] bu hdr idx=%u b0=%s b1=%s b2=%s b3=%s magic=%s p1C_10=0x%08X\n",
                      idx, bufHex[0], bufHex[1], bufHex[2], bufHex[3], magicHex, deref10);
                ps2xSafeLog("[probe] bu hdr idx=%u b0=%s b1=%s b2=%s b3=%s magic=%s p1C_10=0x%08X\n",
                            idx, bufHex[0], bufHex[1], bufHex[2], bufHex[3], magicHex, deref10);
            }
        }
    }

    void onBuTickSec(const uint8_t *rdram) {
        if (!isEnabled() || !rdram) return;
        ensureBuSamplerStarted();

        uint32_t calls = s_buPumpCalls.exchange(0, std::memory_order_relaxed);
        uint32_t ctxAddr = s_buLastCtx.load(std::memory_order_relaxed);
        uint32_t state = 0, f74 = 0;
        std::memcpy(&state, rdram + ((ctxAddr + 0x08u) & PS2_RAM_MASK), 4);
        std::memcpy(&f74, rdram + ((ctxAddr + 0x74u) & PS2_RAM_MASK), 4);

        bhLog("[probe] bu sec pump_calls=%u state=%u f74=0x%X\n", calls, state, f74);
        ps2xSafeLog("[probe] bu sec pump_calls=%u state=%u f74=0x%X\n", calls, state, f74);

        // Sampler print
        std::vector<std::pair<uint32_t, uint32_t>> hot;
        uint32_t totalSamples = 0;
        {
            std::lock_guard<std::mutex> lock(s_buHotMutex);
            hot.assign(s_buHotCounts.begin(), s_buHotCounts.end());
            totalSamples = s_buHotTotalSamples;
            s_buHotCounts.clear();
            s_buHotTotalSamples = 0;
        }

        std::sort(hot.begin(), hot.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });

        std::string hotStr;
        size_t count = std::min<size_t>(hot.size(), 12);
        for (size_t i = 0; i < count; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s0x%08X:%u", (i > 0 ? " " : ""), hot[i].first, hot[i].second);
            hotStr += buf;
        }

        bhLog("[probe] bu hot top12=<%s> samples=%u\n", hotStr.c_str(), totalSamples);
        ps2xSafeLog("[probe] bu hot top12=<%s> samples=%u\n", hotStr.c_str(), totalSamples);
    }

    // =========================================================================
    // Task BV: Stale video-read size probe (0x24CA3C) + Main thread wait tracker
    // =========================================================================
    static std::atomic<uint32_t> s_bvReadCalls{0};

    struct BvWaitKey {
        int kind;
        int id;
        uint32_t ra;
        bool operator==(const BvWaitKey &o) const {
            return kind == o.kind && id == o.id && ra == o.ra;
        }
    };
    struct BvWaitKeyHash {
        std::size_t operator()(const BvWaitKey &k) const {
            return std::hash<int>()(k.kind) ^ (std::hash<int>()(k.id) << 1) ^ (std::hash<uint32_t>()(k.ra) << 2);
        }
    };

    static std::mutex s_bvWaitMutex;
    static double s_bvWaitTotalMs = 0.0;
    static std::unordered_map<BvWaitKey, double, BvWaitKeyHash> s_bvWaitStats;
    static std::atomic<uint32_t> s_bvIntcCause3{0};
    static std::atomic<uint32_t> s_bvIntcCause5{0};

    void recordBvMainWait(int kind, int id, uint32_t ra, double duration_ms) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_bvWaitMutex);
        s_bvWaitTotalMs += duration_ms;
        s_bvWaitStats[{kind, id, ra}] += duration_ms;
    }

    void recordBvIntcDelivery(uint32_t cause) {
        if (cause == 3u) {
            s_bvIntcCause3.fetch_add(1, std::memory_order_relaxed);
        } else if (cause == 5u) {
            s_bvIntcCause5.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void onBvReadSubmit(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled() || !ctx || !rdram) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        uint32_t ra = getRegU32(ctx, 31);

        uint32_t callCount = s_bvReadCalls.fetch_add(1, std::memory_order_relaxed);

        uint32_t c48 = 0, c4C = 0, c50 = 0, c60 = 0, c64 = 0, c70 = 0, c74 = 0, c78 = 0;
        std::memcpy(&c48, rdram + ((a0 + 0x48u) & PS2_RAM_MASK), 4);
        std::memcpy(&c4C, rdram + ((a0 + 0x4Cu) & PS2_RAM_MASK), 4);
        std::memcpy(&c50, rdram + ((a0 + 0x50u) & PS2_RAM_MASK), 4);
        std::memcpy(&c60, rdram + ((a0 + 0x60u) & PS2_RAM_MASK), 4);
        std::memcpy(&c64, rdram + ((a0 + 0x64u) & PS2_RAM_MASK), 4);
        std::memcpy(&c70, rdram + ((a0 + 0x70u) & PS2_RAM_MASK), 4);
        std::memcpy(&c74, rdram + ((a0 + 0x74u) & PS2_RAM_MASK), 4);
        std::memcpy(&c78, rdram + ((a0 + 0x78u) & PS2_RAM_MASK), 4);

        uint32_t expectedVsize = 0xFFFFFFFFu;
        if (c4C >= 0x20u) {
            uint32_t hdrBase = (c4C - 0x20u) & PS2_RAM_MASK;
            std::memcpy(&expectedVsize, rdram + ((hdrBase + 0x28u) & PS2_RAM_MASK), 4);
        }

        bool shouldLog = (callCount < 40) || (a1 == 0) || (a1 != expectedVsize);
        if (!shouldLog) return;

        bhLog("[probe] bv rd a1=%u a2=0x%X a3=0x%X ra=0x%08X ctx[+0x48]=0x%X [+0x4C]=0x%X [+0x50]=%u [+0x60]=0x%X [+0x64]=0x%X [+0x70]=0x%X [+0x74]=0x%X [+0x78]=0x%X\n",
              a1, a2, a3, ra, c48, c4C, c50, c60, c64, c70, c74, c78);
        ps2xSafeLog("[probe] bv rd a1=%u a2=0x%X a3=0x%X ra=0x%08X ctx[+0x48]=0x%X [+0x4C]=0x%X [+0x50]=%u [+0x60]=0x%X [+0x64]=0x%X [+0x70]=0x%X [+0x74]=0x%X [+0x78]=0x%X\n",
                    a1, a2, a3, ra, c48, c4C, c50, c60, c64, c70, c74, c78);

        static const uint32_t bufs[3] = {0x187B600u, 0x18A7E00u, 0x18D4600u};
        char bStr[3][128];
        for (int i = 0; i < 3; ++i) {
            uint32_t bAddr = bufs[i] & PS2_RAM_MASK;
            uint32_t cnt = 0, frames = 0, n = 0, vsize = 0;
            std::memcpy(&cnt, rdram + bAddr + 0x10u, 4);
            std::memcpy(&frames, rdram + bAddr + 0x20u, 4);
            std::memcpy(&n, rdram + bAddr + 0x24u, 4);
            std::memcpy(&vsize, rdram + bAddr + 0x28u, 4);
            std::snprintf(bStr[i], sizeof(bStr[i]), "0x%08X:cnt=%u frames=%u n=%u vsize=%u",
                          bufs[i], cnt, frames, n, vsize);
        }

        uint32_t g742[8] = {0};
        for (int i = 0; i < 8; ++i) {
            std::memcpy(&g742[i], rdram + ((0x742C48u + 4u * i) & PS2_RAM_MASK), 4);
        }

        uint64_t cid1Off = probe_getCid1Fd3Offset();

        bhLog("[probe] bv hdrs %s | %s | %s | g742=[0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X] newOff=%llu\n",
              bStr[0], bStr[1], bStr[2],
              g742[0], g742[1], g742[2], g742[3], g742[4], g742[5], g742[6], g742[7],
              (unsigned long long)cid1Off);
        ps2xSafeLog("[probe] bv hdrs %s | %s | %s | g742=[0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X] newOff=%llu\n",
                    bStr[0], bStr[1], bStr[2],
                    g742[0], g742[1], g742[2], g742[3], g742[4], g742[5], g742[6], g742[7],
                    (unsigned long long)cid1Off);
    }

    static std::atomic<uint32_t> s_ceLastGuestTicks{0};

    void onBvTickSec() {
        if (!isEnabled()) return;
        double totalMs = 0.0;
        std::vector<std::pair<BvWaitKey, double>> stats;
        {
            std::lock_guard<std::mutex> lock(s_bvWaitMutex);
            totalMs = s_bvWaitTotalMs;
            s_bvWaitTotalMs = 0.0;
            stats.assign(s_bvWaitStats.begin(), s_bvWaitStats.end());
            s_bvWaitStats.clear();
        }

        std::sort(stats.begin(), stats.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });

        const char *kindNames[] = {"None", "Sleep", "Sema", "EvFlag", "Unknown"};
        std::string topStr;
        size_t count = std::min<size_t>(stats.size(), 5);
        for (size_t i = 0; i < count; ++i) {
            const auto &k = stats[i].first;
            int kIdx = (k.kind >= 0 && k.kind <= 3) ? k.kind : 4;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s%s:%d@0x%08X=%.1fms",
                          (i > 0 ? " " : ""), kindNames[kIdx], k.id, k.ra, stats[i].second);
            topStr += buf;
        }

        uint32_t c3 = s_bvIntcCause3.exchange(0, std::memory_order_relaxed);
        uint32_t c5 = s_bvIntcCause5.exchange(0, std::memory_order_relaxed);
        s_ceLastGuestTicks.store(c5, std::memory_order_relaxed);

        bhLog("[probe] bv wait total_ms=%.1f top5=<%s> cause3=%u cause5=%u\n",
              totalMs, topStr.c_str(), c3, c5);
        ps2xSafeLog("[probe] bv wait total_ms=%.1f top5=<%s> cause3=%u cause5=%u\n",
                    totalMs, topStr.c_str(), c3, c5);
    }

    // =========================================================================
    // Task BW: Display state, decode health, and screenshots for intro movie
    // =========================================================================

    static std::atomic<uint32_t> s_bwPresentedFrames{0};
    static uint64_t s_bwLastXyoffset2 = 0xFFFFFFFFFFFFFFFFULL;
    static uint64_t s_bwLastFrame2 = 0xFFFFFFFFFFFFFFFFULL;
    static uint64_t s_bwLastScissor2 = 0xFFFFFFFFFFFFFFFFULL;
    static uint64_t s_bwLastDispfb2 = 0xFFFFFFFFFFFFFFFFULL;
    static uint64_t s_bwLastDisplay2 = 0xFFFFFFFFFFFFFFFFULL;
    static uint64_t s_bwLastPmode = 0xFFFFFFFFFFFFFFFFULL;
    static std::pair<int32_t, int32_t> s_bwLastFirstMscal{-9999, -9999};

    // MSCAL block tracking per frame / per second
    static std::mutex s_bwMscalMutex;
    static bool s_bwFrameHasFirstMscal = false;
    static std::pair<int32_t, int32_t> s_bwCurrentFrameFirstMscal{-1, -1};
    static std::pair<int32_t, int32_t> s_bwSecFirstMscal{-1, -1};
    static int32_t s_bwSecMinX = 999999, s_bwSecMinY = 999999;
    static int32_t s_bwSecMaxX = -999999, s_bwSecMaxY = -999999;
    static uint32_t s_bwSecBlocks = 0;

    void onBwMscalBlock(int32_t mbX, int32_t mbY) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_bwMscalMutex);
        if (!s_bwFrameHasFirstMscal) {
            s_bwFrameHasFirstMscal = true;
            s_bwCurrentFrameFirstMscal = {mbX, mbY};
        }
        if (s_bwSecBlocks == 0) {
            s_bwSecFirstMscal = {mbX, mbY};
        }
        s_bwSecBlocks++;
        if (mbX < s_bwSecMinX) s_bwSecMinX = mbX;
        if (mbY < s_bwSecMinY) s_bwSecMinY = mbY;
        if (mbX > s_bwSecMaxX) s_bwSecMaxX = mbX;
        if (mbY > s_bwSecMaxY) s_bwSecMaxY = mbY;
    }

    // Screenshots
    static auto s_bwLastShotTime = std::chrono::steady_clock::time_point::min();
    static uint32_t s_bwShotIndex = 0;

    void onBwFramePresented(uint32_t width, uint32_t height, const uint8_t *rgba, const uint8_t *rdram, const class PS2Runtime *rt) {
        if (!isEnabled() || !rdram) return;
        uint32_t st = 0;
        std::memcpy(&st, rdram + (0x772188u & PS2_RAM_MASK), 4);
        if (st != 9) return;

        uint32_t frameNum = s_bwPresentedFrames.fetch_add(1, std::memory_order_relaxed) + 1;

        // Fetch GS registers
        uint64_t xyoffset2 = 0, frame2 = 0, scissor2 = 0;
        uint64_t dispfb2 = 0, display2 = 0, pmode = 0;
        {
            std::lock_guard<std::mutex> lock(s_bbMutex);
            xyoffset2 = s_bbGsRegs[0x19];
            frame2 = s_bbGsRegs[0x4D];
            scissor2 = s_bbGsRegs[0x41];
            dispfb2 = s_bbDispfb2;
            display2 = s_bbDisplay2;
        }
        if (rt) {
            auto &mem = const_cast<PS2Runtime*>(rt)->memory();
            pmode = mem.read64(0x12001000u + 0x0000u);
            if (dispfb2 == 0) dispfb2 = mem.read64(0x12001000u + 0x0090u);
            if (display2 == 0) display2 = mem.read64(0x12001000u + 0x00A0u);
        }

        std::pair<int32_t, int32_t> frameFirstMscal{-1, -1};
        {
            std::lock_guard<std::mutex> lock(s_bwMscalMutex);
            frameFirstMscal = s_bwCurrentFrameFirstMscal;
            s_bwFrameHasFirstMscal = false;
            s_bwCurrentFrameFirstMscal = {-1, -1};
        }

        bool mscalDiff = (frameFirstMscal.first != -1 && frameFirstMscal != s_bwLastFirstMscal);
        if (xyoffset2 != s_bwLastXyoffset2 || frame2 != s_bwLastFrame2 ||
            scissor2 != s_bwLastScissor2 || dispfb2 != s_bwLastDispfb2 ||
            display2 != s_bwLastDisplay2 || pmode != s_bwLastPmode || mscalDiff)
        {
            s_bwLastXyoffset2 = xyoffset2;
            s_bwLastFrame2 = frame2;
            s_bwLastScissor2 = scissor2;
            s_bwLastDispfb2 = dispfb2;
            s_bwLastDisplay2 = display2;
            s_bwLastPmode = pmode;
            if (frameFirstMscal.first != -1) s_bwLastFirstMscal = frameFirstMscal;

            uint32_t dbp = static_cast<uint32_t>((dispfb2 >> 0) & 0x1FFu);
            uint32_t dbw = static_cast<uint32_t>((dispfb2 >> 9) & 0x3Fu);
            uint32_t psm = static_cast<uint32_t>((dispfb2 >> 15) & 0x1Fu);
            uint32_t dbx = static_cast<uint32_t>((dispfb2 >> 32) & 0x7FFu);
            uint32_t dby = static_cast<uint32_t>((dispfb2 >> 43) & 0x7FFu);

            uint32_t dx = static_cast<uint32_t>((display2 >> 0) & 0xFFFu);
            uint32_t dy = static_cast<uint32_t>((display2 >> 12) & 0x7FFu);
            uint32_t magh = static_cast<uint32_t>((display2 >> 23) & 0x0Fu);
            uint32_t magv = static_cast<uint32_t>((display2 >> 27) & 0x03u);
            uint32_t dw = static_cast<uint32_t>((display2 >> 32) & 0x0FFFu);
            uint32_t dh = static_cast<uint32_t>((display2 >> 44) & 0x07FFu);

            bhLog("[probe] bw gs frame=%u XYOFF2=0x%016llX FRAME2=0x%016llX SCIS2=0x%016llX DISPFB2=(dbp=%u,dbw=%u,psm=%u,dbx=%u,dby=%u) DISPLAY2=(dx=%u,dy=%u,dw=%u,dh=%u,magh=%u,magv=%u) PMODE=0x%04llX mscal=(%d,%d)\n",
                  frameNum, (unsigned long long)xyoffset2, (unsigned long long)frame2, (unsigned long long)scissor2,
                  dbp, dbw, psm, dbx, dby, dx, dy, dw, dh, magh, magv, (unsigned long long)pmode,
                  frameFirstMscal.first, frameFirstMscal.second);
            ps2xSafeLog("[probe] bw gs frame=%u XYOFF2=0x%016llX FRAME2=0x%016llX SCIS2=0x%016llX DISPFB2=(dbp=%u,dbw=%u,psm=%u,dbx=%u,dby=%u) DISPLAY2=(dx=%u,dy=%u,dw=%u,dh=%u,magh=%u,magv=%u) PMODE=0x%04llX mscal=(%d,%d)\n",
                        frameNum, (unsigned long long)xyoffset2, (unsigned long long)frame2, (unsigned long long)scissor2,
                        dbp, dbw, psm, dbx, dby, dx, dy, dw, dh, magh, magv, (unsigned long long)pmode,
                        frameFirstMscal.first, frameFirstMscal.second);
        }

        // Part C: screenshot every 3 seconds, max 30 files
        auto now = std::chrono::steady_clock::now();
        if (rgba && width > 0 && height > 0 && s_bwShotIndex < 30) {
            double elapsedSec = std::chrono::duration<double>(now - s_bwLastShotTime).count();
            if (elapsedSec >= 3.0) {
                s_bwLastShotTime = now;
                char filename[64];
                std::snprintf(filename, sizeof(filename), "frames/bw_%02u.png", s_bwShotIndex++);
                std::filesystem::create_directories("frames");
                ::writeUncompressedPng(width, height, rgba, filename);
                double t_ms = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
                bhLog("[probe] bw shot %s t_ms=%.1f\n", filename, t_ms);
                ps2xSafeLog("[probe] bw shot %s t_ms=%.1f\n", filename, t_ms);
            }
        }
    }

    // Part B: Decode health
    static std::atomic<uint32_t> s_bwDecCalls{0};
    static std::atomic<uint32_t> s_bwDecOk{0};
    static std::atomic<uint32_t> s_bwDecFail{0};
    static std::atomic<uint32_t> s_bwRingPops{0};
    static std::atomic<uint32_t> s_bwPicI{0};
    static std::atomic<uint32_t> s_bwPicP{0};
    static std::atomic<uint32_t> s_bwPicB{0};
    static std::atomic<uint32_t> s_bwBytesIn{0};
    static std::atomic<uint32_t> s_bwRingUnderflow{0};
    static std::atomic<uint32_t> s_bwLastW{0};
    static std::atomic<uint32_t> s_bwLastH{0};

    static std::mutex s_bwErrMutex;
    static uint32_t s_bwLoggedErrors = 0;
    static std::atomic<uint32_t> s_bwChunksFed{0};

    void onBwDecPicture(int pict_type, int width, int height) {
        if (!isEnabled()) return;
        if (pict_type == 1) s_bwPicI.fetch_add(1, std::memory_order_relaxed);
        else if (pict_type == 2) s_bwPicP.fetch_add(1, std::memory_order_relaxed);
        else if (pict_type == 3) s_bwPicB.fetch_add(1, std::memory_order_relaxed);
        s_bwLastW.store(static_cast<uint32_t>(width), std::memory_order_relaxed);
        s_bwLastH.store(static_cast<uint32_t>(height), std::memory_order_relaxed);
    }

    void onBwDecError(const char *err) {
        if (!isEnabled() || !err) return;
        std::lock_guard<std::mutex> lock(s_bwErrMutex);
        if (s_bwLoggedErrors < 5) {
            s_bwLoggedErrors++;
            bhLog("[probe] bw decerr %s\n", err);
            ps2xSafeLog("[probe] bw decerr %s\n", err);
        }
    }

    void onBwFeedChunk(const uint8_t *data, size_t size) {
        if (!isEnabled() || !data) return;
        uint32_t chunkIdx = s_bwChunksFed.fetch_add(1, std::memory_order_relaxed);
        if (chunkIdx < 6) {
            char hexBuf[32];
            int pos = 0;
            size_t n = std::min<size_t>(size, 8);
            for (size_t i = 0; i < n; ++i) {
                pos += std::snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02x ", data[i]);
            }
            bhLog("[probe] bw chunk%u bytes=%zu: %s\n", chunkIdx, size, hexBuf);
            ps2xSafeLog("[probe] bw chunk%u bytes=%zu: %s\n", chunkIdx, size, hexBuf);
        }
    }

    void onBwRaw8End(bool ok, int pops, uint32_t bytesIn, uint32_t underflow) {
        if (!isEnabled()) return;
        s_bwDecCalls.fetch_add(1, std::memory_order_relaxed);
        if (ok) s_bwDecOk.fetch_add(1, std::memory_order_relaxed);
        else s_bwDecFail.fetch_add(1, std::memory_order_relaxed);
        s_bwRingPops.fetch_add(static_cast<uint32_t>(pops), std::memory_order_relaxed);
        s_bwBytesIn.fetch_add(bytesIn, std::memory_order_relaxed);
        s_bwRingUnderflow.fetch_add(underflow, std::memory_order_relaxed);
    }

    void onBwTickSec() {
        if (!isEnabled()) return;

        // 1. [probe] bw pos
        std::pair<int32_t, int32_t> firstPos{-1, -1};
        int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
        uint32_t blocks = 0;
        {
            std::lock_guard<std::mutex> lock(s_bwMscalMutex);
            firstPos = s_bwSecFirstMscal;
            minX = (s_bwSecBlocks > 0) ? s_bwSecMinX : 0;
            minY = (s_bwSecBlocks > 0) ? s_bwSecMinY : 0;
            maxX = (s_bwSecBlocks > 0) ? s_bwSecMaxX : 0;
            maxY = (s_bwSecBlocks > 0) ? s_bwSecMaxY : 0;
            blocks = s_bwSecBlocks;

            s_bwSecFirstMscal = {-1, -1};
            s_bwSecMinX = 999999; s_bwSecMinY = 999999;
            s_bwSecMaxX = -999999; s_bwSecMaxY = -999999;
            s_bwSecBlocks = 0;
        }

        bhLog("[probe] bw pos first=(%d,%d) min=(%d,%d) max=(%d,%d) blocks=%u\n",
              firstPos.first, firstPos.second, minX, minY, maxX, maxY, blocks);
        ps2xSafeLog("[probe] bw pos first=(%d,%d) min=(%d,%d) max=(%d,%d) blocks=%u\n",
                    firstPos.first, firstPos.second, minX, minY, maxX, maxY, blocks);

        // 2. [probe] bw dec
        uint32_t calls = s_bwDecCalls.exchange(0, std::memory_order_relaxed);
        uint32_t ok = s_bwDecOk.exchange(0, std::memory_order_relaxed);
        uint32_t fail = s_bwDecFail.exchange(0, std::memory_order_relaxed);
        uint32_t pops = s_bwRingPops.exchange(0, std::memory_order_relaxed);
        uint32_t picI = s_bwPicI.exchange(0, std::memory_order_relaxed);
        uint32_t picP = s_bwPicP.exchange(0, std::memory_order_relaxed);
        uint32_t picB = s_bwPicB.exchange(0, std::memory_order_relaxed);
        uint32_t bytesIn = s_bwBytesIn.exchange(0, std::memory_order_relaxed);
        uint32_t underflow = s_bwRingUnderflow.exchange(0, std::memory_order_relaxed);
        uint32_t w = s_bwLastW.load(std::memory_order_relaxed);
        uint32_t h = s_bwLastH.load(std::memory_order_relaxed);

        bhLog("[probe] bw dec calls=%u ok=%u fail=%u ring_pops=%u pic_types=I:%u,P:%u,B:%u bytes_in=%u ring_underflow=%u w=%u h=%u\n",
              calls, ok, fail, pops, picI, picP, picB, bytesIn, underflow, w, h);
        ps2xSafeLog("[probe] bw dec calls=%u ok=%u fail=%u ring_pops=%u pic_types=I:%u,P:%u,B:%u bytes_in=%u ring_underflow=%u w=%u h=%u\n",
                    calls, ok, fail, pops, picI, picP, picB, bytesIn, underflow, w, h);
    }

    // =========================================================================
    // Task BX: MSCAL path accounting, display pipeline, and screenshot dumps
    // =========================================================================
    static std::atomic<uint32_t> s_bxKicks{0};
    static std::atomic<uint32_t> s_bxNativeMscals{0};
    static std::atomic<uint32_t> s_bxOrigRunMscals{0};

    static std::mutex s_bxHistMutex;
    static uint32_t s_bxNativeRowHist[24] = {0};
    static std::unordered_map<uint32_t, uint32_t> s_bxTopsHist;

    struct BxOrigRunInfo {
        uint32_t startPC;
        uint32_t top;
        int32_t qwTop[4];
        int32_t qwTop1[4];
        int32_t minX, minY, maxX, maxY;
        bool hasVerts;
    };
    static std::vector<BxOrigRunInfo> s_bxSecOrigRuns;
    static BxOrigRunInfo s_bxCurOrigRun;
    static bool s_bxInOrigRun = false;

    void onBxKick() {
        if (!isEnabled()) return;
        s_bxKicks.fetch_add(1, std::memory_order_relaxed);
    }

    void onBxNativeMscal(int32_t mbX, int32_t mbY, uint32_t top) {
        if (!isEnabled()) return;
        s_bxNativeMscals.fetch_add(1, std::memory_order_relaxed);
        int row = mbY / 16;
        if (row >= 0 && row < 24) {
            std::lock_guard<std::mutex> lock(s_bxHistMutex);
            s_bxNativeRowHist[row]++;
            s_bxTopsHist[top]++;
        }
    }

    void onBxOrigRunStart(uint32_t startPC, uint32_t top, const uint8_t *vuData, uint32_t dataSize) {
        if (!isEnabled()) return;
        s_bxOrigRunMscals.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(s_bxHistMutex);
        s_bxTopsHist[top & 0x3FFu]++;

        if (s_bxSecOrigRuns.size() < 3) {
            s_bxInOrigRun = true;
            s_bxCurOrigRun = {};
            s_bxCurOrigRun.startPC = startPC;
            s_bxCurOrigRun.top = top & 0x3FFu;
            s_bxCurOrigRun.minX = 999999;
            s_bxCurOrigRun.minY = 999999;
            s_bxCurOrigRun.maxX = -999999;
            s_bxCurOrigRun.maxY = -999999;
            s_bxCurOrigRun.hasVerts = false;

            if (vuData && dataSize >= 16) {
                uint32_t off0 = ((top + 0u) & 0x3FFu) * 16u;
                uint32_t off1 = ((top + 1u) & 0x3FFu) * 16u;
                if (off0 + 16u <= dataSize) std::memcpy(s_bxCurOrigRun.qwTop, vuData + off0, 16);
                if (off1 + 16u <= dataSize) std::memcpy(s_bxCurOrigRun.qwTop1, vuData + off1, 16);
            }
        }
    }

    void onBxOrigRunXgkick(const uint8_t *data, uint32_t byteCount) {
        if (!isEnabled() || !s_bxInOrigRun || !data || byteCount < 32) return;
        std::lock_guard<std::mutex> lock(s_bxHistMutex);
        // Parse vertices from GIF packet (similar to verify path: 16-byte header then 32-byte verts)
        size_t numVerts = (byteCount >= 16) ? (byteCount - 16) / 32 : 0;
        for (size_t v = 0; v < numVerts; ++v) {
            size_t xyzOff = 16 + v * 32 + 16;
            if (xyzOff + 4 <= byteCount) {
                uint16_t x_raw = static_cast<uint16_t>(data[xyzOff]) | (static_cast<uint16_t>(data[xyzOff + 1]) << 8);
                uint16_t y_raw = static_cast<uint16_t>(data[xyzOff + 2]) | (static_cast<uint16_t>(data[xyzOff + 3]) << 8);
                int px = static_cast<int>(x_raw >> 4);
                int py = static_cast<int>(y_raw >> 4);
                s_bxCurOrigRun.hasVerts = true;
                if (px < s_bxCurOrigRun.minX) s_bxCurOrigRun.minX = px;
                if (py < s_bxCurOrigRun.minY) s_bxCurOrigRun.minY = py;
                if (px > s_bxCurOrigRun.maxX) s_bxCurOrigRun.maxX = px;
                if (py > s_bxCurOrigRun.maxY) s_bxCurOrigRun.maxY = py;
            }
        }
    }

    void onBxOrigRunEnd() {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_bxHistMutex);
        if (s_bxInOrigRun) {
            s_bxInOrigRun = false;
            if (s_bxSecOrigRuns.size() < 3) {
                s_bxSecOrigRuns.push_back(s_bxCurOrigRun);
            }
        }
    }

    // Part B & C: Presentation & GS Prim
    static uint32_t s_bxLastDispFbp = 0xFFFFFFFFu;
    static uint32_t s_bxLastSrcFbp = 0xFFFFFFFFu;
    static int s_bxLastPreferred = -1;
    static uint32_t s_bxLastW = 0, s_bxLastH = 0;
    static int s_bxLastX0 = -1, s_bxLastY0 = -1, s_bxLastX1 = -1, s_bxLastY1 = -1;
    static uint32_t s_bxLastNonblack = 0xFFFFFFFFu;

    static auto s_bxLastShotTime = std::chrono::steady_clock::time_point::min();
    static uint32_t s_bxShotCount = 0;

    void onBxPresentFrame(uint32_t frameNum, uint32_t displayFbp, uint32_t sourceFbp, bool preferred,
                          uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t currentTick) {
        if (!isEnabled() || !rgba || width == 0 || height == 0) return;
        const uint8_t *rdram = s_cachedRdram;
        if (!rdram) return;
        uint32_t st = 0;
        std::memcpy(&st, rdram + (0x772188u & PS2_RAM_MASK), 4);
        if (st != 9) return;

        // Calculate bounding box and nonblack count
        int x0 = 999999, y0 = 999999, x1 = -1, y1 = -1;
        uint32_t nonblack = 0;
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t *row = reinterpret_cast<const uint32_t *>(rgba + y * width * 4u);
            for (uint32_t x = 0; x < width; ++x) {
                if ((row[x] & 0x00FFFFFFu) != 0) {
                    nonblack++;
                    if (static_cast<int>(x) < x0) x0 = static_cast<int>(x);
                    if (static_cast<int>(y) < y0) y0 = static_cast<int>(y);
                    if (static_cast<int>(x) > x1) x1 = static_cast<int>(x);
                    if (static_cast<int>(y) > y1) y1 = static_cast<int>(y);
                }
            }
        }
        if (nonblack == 0) {
            x0 = y0 = x1 = y1 = 0;
        }

        int prefInt = preferred ? 1 : 0;
        if (displayFbp != s_bxLastDispFbp || sourceFbp != s_bxLastSrcFbp || prefInt != s_bxLastPreferred ||
            width != s_bxLastW || height != s_bxLastH || x0 != s_bxLastX0 || y0 != s_bxLastY0 ||
            x1 != s_bxLastX1 || y1 != s_bxLastY1 || nonblack != s_bxLastNonblack)
        {
            s_bxLastDispFbp = displayFbp;
            s_bxLastSrcFbp = sourceFbp;
            s_bxLastPreferred = prefInt;
            s_bxLastW = width;
            s_bxLastH = height;
            s_bxLastX0 = x0; s_bxLastY0 = y0;
            s_bxLastX1 = x1; s_bxLastY1 = y1;
            s_bxLastNonblack = nonblack;

            bhLog("[probe] bx present frame=%u display_fbp=%u source_fbp=%u preferred=%d w=%u h=%u bbox=(%d,%d,%d,%d) nonblack=%u\n",
                  frameNum, displayFbp, sourceFbp, prefInt, width, height, x0, y0, x1, y1, nonblack);
            ps2xSafeLog("[probe] bx present frame=%u display_fbp=%u source_fbp=%u preferred=%d w=%u h=%u bbox=(%d,%d,%d,%d) nonblack=%u\n",
                        frameNum, displayFbp, sourceFbp, prefInt, width, height, x0, y0, x1, y1, nonblack);
        }

        // Part C: Dump frame every 2 seconds while movie state 9, max 40 files
        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - s_bxLastShotTime).count();
        if (elapsedSec >= 2.0 && s_bxShotCount < 40) {
            s_bxLastShotTime = now;
            char filename[64];
            std::snprintf(filename, sizeof(filename), "frames/bx_%02u.png", s_bxShotCount++);
            std::filesystem::create_directories("frames");
            ::writeUncompressedPng(width, height, rgba, filename);
            double t_ms = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
            bhLog("[probe] bx shot %s t_ms=%.1f\n", filename, t_ms);
            ps2xSafeLog("[probe] bx shot %s t_ms=%.1f\n", filename, t_ms);
        }
    }

    struct BxGsPrimInfo {
        const char *type;
        uint32_t fbp;
        uint32_t tbp0;
        float x0, y0, x1, y1;
        float u0, v0, u1, v1;
        uint32_t tex_fbw;
    };
    static std::mutex s_bxPrimMutex;
    static uint32_t s_bxSecGsPrimCount = 0;
    static std::vector<BxGsPrimInfo> s_bxSecGsPrims;

    void onBxDrawPrimitive(const struct GSPrimitiveBatch &batch) {
        if (!isEnabled()) return;
        const auto &ctx = batch.state.context;
        // Check if destination FBP matches DISPFB2.dbp (112)
        uint64_t dispfb2 = 0;
        {
            std::lock_guard<std::mutex> lock(s_bbMutex);
            dispfb2 = s_bbDispfb2;
        }
        uint32_t dbp = static_cast<uint32_t>((dispfb2 >> 0) & 0x1FFu);
        if (dbp == 0) dbp = 112; // fallback to known 112 if not yet set

        if (ctx.frame.fbp == dbp) {
            std::lock_guard<std::mutex> lock(s_bxPrimMutex);
            s_bxSecGsPrimCount++;
            if (s_bxSecGsPrims.size() < 4) {
                BxGsPrimInfo info{};
                info.type = (batch.state.prim.type == GS_PRIM_SPRITE) ? "sprite" :
                            (batch.state.prim.type == GS_PRIM_TRIANGLE || batch.state.prim.type == GS_PRIM_TRISTRIP || batch.state.prim.type == GS_PRIM_TRIFAN) ? "tri" : "other";
                info.fbp = ctx.frame.fbp;
                info.tbp0 = ctx.tex0.tbp0;
                info.tex_fbw = ctx.tex0.tbw;

                if (batch.vertexCount >= 2) {
                    info.x0 = batch.vertices[0].x;
                    info.y0 = batch.vertices[0].y;
                    info.x1 = batch.vertices[1].x;
                    info.y1 = batch.vertices[1].y;

                    info.u0 = batch.vertices[0].u;
                    info.v0 = batch.vertices[0].v;
                    info.u1 = batch.vertices[1].u;
                    info.v1 = batch.vertices[1].v;
                }
                s_bxSecGsPrims.push_back(info);
            }
        }
    }

    void onBxTickSec(const uint8_t *rdram) {
        if (!isEnabled()) return;
        uint32_t st = 0;
        if (rdram) {
            std::memcpy(&st, rdram + (0x772188u & PS2_RAM_MASK), 4);
        }
        if (st != 9) return;

        uint32_t kicks = s_bxKicks.exchange(0, std::memory_order_relaxed);
        uint32_t native = s_bxNativeMscals.exchange(0, std::memory_order_relaxed);
        uint32_t orig_run = s_bxOrigRunMscals.exchange(0, std::memory_order_relaxed);
        uint32_t skipped = (kicks > 0 && kicks * 320u > (native + orig_run)) ? (kicks * 320u - (native + orig_run)) : 0;

        bhLog("[probe] bx path native=%u orig_run=%u skipped_other=%u kicks=%u\n",
              native, orig_run, skipped, kicks);
        ps2xSafeLog("[probe] bx path native=%u orig_run=%u skipped_other=%u kicks=%u\n",
                    native, orig_run, skipped, kicks);

        // Row histogram & TOPs
        char histStr[256];
        int hpos = 0;
        std::string topsStr;
        std::vector<BxOrigRunInfo> origRuns;
        {
            std::lock_guard<std::mutex> lock(s_bxHistMutex);
            for (int r = 0; r < 24; ++r) {
                hpos += std::snprintf(histStr + hpos, sizeof(histStr) - hpos, "%u%s",
                                      s_bxNativeRowHist[r], (r < 23 ? " " : ""));
                s_bxNativeRowHist[r] = 0;
            }

            size_t tCount = 0;
            for (const auto &pair : s_bxTopsHist) {
                if (tCount > 0) topsStr += ",";
                char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%03X:%u", pair.first, pair.second);
                topsStr += buf;
                tCount++;
            }
            s_bxTopsHist.clear();

            origRuns = s_bxSecOrigRuns;
            s_bxSecOrigRuns.clear();
        }

        bhLog("[probe] bx rows native_hist=[%s]\n", histStr);
        ps2xSafeLog("[probe] bx rows native_hist=[%s]\n", histStr);

        bhLog("[probe] bx tops tops=<%s>\n", topsStr.c_str());
        ps2xSafeLog("[probe] bx tops tops=<%s>\n", topsStr.c_str());

        // Orig runs
        for (const auto &run : origRuns) {
            bhLog("[probe] bx orig first3 pc=0x%03X top=0x%03X qwTop=[%d,%d,%d,%d] qwTop1=[%d,%d,%d,%d] bbox=(%d,%d)-(%d,%d)\n",
                  run.startPC, run.top,
                  run.qwTop[0], run.qwTop[1], run.qwTop[2], run.qwTop[3],
                  run.qwTop1[0], run.qwTop1[1], run.qwTop1[2], run.qwTop1[3],
                  run.hasVerts ? run.minX : 0, run.hasVerts ? run.minY : 0,
                  run.hasVerts ? run.maxX : 0, run.hasVerts ? run.maxY : 0);
            ps2xSafeLog("[probe] bx orig first3 pc=0x%03X top=0x%03X qwTop=[%d,%d,%d,%d] qwTop1=[%d,%d,%d,%d] bbox=(%d,%d)-(%d,%d)\n",
                        run.startPC, run.top,
                        run.qwTop[0], run.qwTop[1], run.qwTop[2], run.qwTop[3],
                        run.qwTop1[0], run.qwTop1[1], run.qwTop1[2], run.qwTop1[3],
                        run.hasVerts ? run.minX : 0, run.hasVerts ? run.minY : 0,
                        run.hasVerts ? run.maxX : 0, run.hasVerts ? run.maxY : 0);
        }

        // GS Prims
        uint32_t primCount = 0;
        std::vector<BxGsPrimInfo> prims;
        {
            std::lock_guard<std::mutex> lock(s_bxPrimMutex);
            primCount = s_bxSecGsPrimCount;
            s_bxSecGsPrimCount = 0;
            prims = s_bxSecGsPrims;
            s_bxSecGsPrims.clear();
        }

        for (const auto &p : prims) {
            bhLog("[probe] bx gsprim type=%s fbp=%u tbp0=%u xy=(%.1f,%.1f)-(%.1f,%.1f) uv=(%.1f,%.1f)-(%.1f,%.1f) tex_fbw=%u\n",
                  p.type, p.fbp, p.tbp0, p.x0, p.y0, p.x1, p.y1, p.u0, p.v0, p.u1, p.v1, p.tex_fbw);
            ps2xSafeLog("[probe] bx gsprim type=%s fbp=%u tbp0=%u xy=(%.1f,%.1f)-(%.1f,%.1f) uv=(%.1f,%.1f)-(%.1f,%.1f) tex_fbw=%u\n",
                        p.type, p.fbp, p.tbp0, p.x0, p.y0, p.x1, p.y1, p.u0, p.v0, p.u1, p.v1, p.tex_fbw);
        }
    }

    // =========================================================================
    // Task BZ: Movie dimensions and MSCAL block order
    // =========================================================================
    static std::mutex s_bzMutex;
    static uint32_t s_bzW = 0, s_bzH = 0;
    static std::vector<std::pair<int32_t, int32_t>> s_bzOrderBlocks;
    static bool s_bzOrderPrinted = false;
    static uint32_t s_bzRaw8CallsInMovie = 0;
    static uint32_t s_bzFirstFrameBlocks = 0;
    static uint32_t s_bzFirstFrameRaw8A2 = 0;
    static bool s_bzFirstFrameCounted = false;

    // Part C: per-second mix
    static std::atomic<uint32_t> s_bzSecNative{0};
    static std::atomic<uint32_t> s_bzSecOrig{0};
    static std::mutex s_bzPosMutex;
    static std::set<std::pair<int32_t, int32_t>> s_bzSecPositions;

    void onBzInitEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        onDhBzInitEntry(ctx, rdram);
        if (!isEnabled() || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6); // w
        uint32_t a3 = getRegU32(ctx, 7); // h
        uint32_t t0 = getRegU32(ctx, 8); // flag
        uint32_t ra = getRegU32(ctx, 31);
        uint32_t s3 = getRegU32(ctx, 19);

        char scene110Buf[32] = "n/a";
        if (rdram && s3 >= 0x00100000u && s3 + 0x114u <= PS2_RAM_SIZE) {
            uint32_t val = 0;
            std::memcpy(&val, rdram + ((s3 + 0x110u) & PS2_RAM_MASK), 4);
            std::snprintf(scene110Buf, sizeof(scene110Buf), "0x%08X", val);
        }

        bhLog("[probe] bz init a1=0x%08X w=%u h=%u t0=0x%08X ra=0x%08X scene+0x110=%s\n",
              a1, a2, a3, t0, ra, scene110Buf);
        ps2xSafeLog("[probe] bz init a1=0x%08X w=%u h=%u t0=0x%08X ra=0x%08X scene+0x110=%s\n",
                    a1, a2, a3, t0, ra, scene110Buf);

        std::lock_guard<std::mutex> lock(s_bzMutex);
        s_bzW = a2;
        s_bzH = a3;
        s_bzOrderBlocks.clear();
        s_bzOrderPrinted = false;
        s_bzRaw8CallsInMovie = 0;
        s_bzFirstFrameBlocks = 0;
        s_bzFirstFrameRaw8A2 = 0;
        s_bzFirstFrameCounted = false;
    }

    void onBzInitExit(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isEnabled() || !rdram) return;
        uint32_t cfg204 = 0, cfg208 = 0, cfg20C = 0;
        std::memcpy(&cfg204, rdram + (0x772384u & PS2_RAM_MASK), 4);
        std::memcpy(&cfg208, rdram + (0x772388u & PS2_RAM_MASK), 4);
        std::memcpy(&cfg20C, rdram + (0x77238Cu & PS2_RAM_MASK), 4);

        bhLog("[probe] bz cfg [0x772384]=%u [0x772388]=%u [0x77238C]=%u\n",
              cfg204, cfg208, cfg20C);
        ps2xSafeLog("[probe] bz cfg [0x772384]=%u [0x772388]=%u [0x77238C]=%u\n",
                    cfg204, cfg208, cfg20C);
    }

    void onBzRaw8Call(uint32_t frameCount, uint32_t mbCapacity) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_bzMutex);
        s_bzRaw8CallsInMovie++;
        if (s_bzRaw8CallsInMovie == 1) {
            s_bzFirstFrameRaw8A2 = mbCapacity;
        } else if (s_bzRaw8CallsInMovie == 2 && !s_bzFirstFrameCounted) {
            s_bzFirstFrameCounted = true;
            bhLog("[probe] bz frame_blocks=%u raw8_a2=%u\n", s_bzFirstFrameBlocks, s_bzFirstFrameRaw8A2);
            ps2xSafeLog("[probe] bz frame_blocks=%u raw8_a2=%u\n", s_bzFirstFrameBlocks, s_bzFirstFrameRaw8A2);
        }
    }

    void onBzNativeBlock(int32_t mbX, int32_t mbY) {
        if (!isEnabled()) return;
        s_bzSecNative.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(s_bzPosMutex);
            s_bzSecPositions.insert({mbX, mbY});
        }

        std::lock_guard<std::mutex> lock(s_bzMutex);
        if (s_bzRaw8CallsInMovie >= 1 && !s_bzFirstFrameCounted) {
            s_bzFirstFrameBlocks++;
        }

        if (!s_bzOrderPrinted && s_bzOrderBlocks.size() < 96) {
            s_bzOrderBlocks.push_back({mbX, mbY});
            if (s_bzOrderBlocks.size() == 96) {
                s_bzOrderPrinted = true;
                for (int line = 0; line < 4; ++line) {
                    std::string lineStr;
                    for (int i = 0; i < 24; ++i) {
                        const auto &pos = s_bzOrderBlocks[line * 24 + i];
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%s(%d,%d)", (i > 0 ? " " : ""), pos.first, pos.second);
                        lineStr += buf;
                    }
                    bhLog("[probe] bz order movie=%ux%u n=%u %s\n",
                          s_bzW, s_bzH, (line + 1) * 24, lineStr.c_str());
                    ps2xSafeLog("[probe] bz order movie=%ux%u n=%u %s\n",
                                s_bzW, s_bzH, (line + 1) * 24, lineStr.c_str());
                }
            }
        }
    }

    void onBzOrigRunBlock() {
        if (!isEnabled()) return;
        s_bzSecOrig.fetch_add(1, std::memory_order_relaxed);
    }

    void onBzTickSec() {
        if (!isEnabled()) return;
        uint32_t native = s_bzSecNative.exchange(0, std::memory_order_relaxed);
        uint32_t orig = s_bzSecOrig.exchange(0, std::memory_order_relaxed);

        size_t uniq = 0;
        {
            std::lock_guard<std::mutex> lock(s_bzPosMutex);
            uniq = s_bzSecPositions.size();
            s_bzSecPositions.clear();
        }

        bhLog("[probe] bz mix native=%u orig=%u uniq_pos=%zu\n", native, orig, uniq);
        ps2xSafeLog("[probe] bz mix native=%u orig=%u uniq_pos=%zu\n", native, orig, uniq);
    }

    // =========================================================================
    // Task CA: VU1 program start 0x848 support, dumps, and accounting
    // =========================================================================
    static std::atomic<uint32_t> s_caPc48{0};
    static std::atomic<uint32_t> s_caPc848{0};
    static std::atomic<uint32_t> s_caPcOther{0};

    void recordCaMscal(uint32_t pc) {
        if (!isEnabled()) return;
        if (pc == 0x048) s_caPc48.fetch_add(1, std::memory_order_relaxed);
        else if (pc == 0x848) s_caPc848.fetch_add(1, std::memory_order_relaxed);
        else s_caPcOther.fetch_add(1, std::memory_order_relaxed);
    }

    static std::mutex s_caDumpMutex;
    static std::map<uint32_t, uint32_t> s_caDumpCounts; // top -> count (max 3 per top)
    static uint32_t s_caTotalDumps = 0; // max 6 total

    void checkCaDump(uint32_t startPC, uint32_t top, const uint8_t *vuData, uint32_t dataSize) {
        if (!isEnabled() || startPC != 0x848 || !vuData || dataSize < 16384) return;
        std::lock_guard<std::mutex> lock(s_caDumpMutex);
        if (s_caTotalDumps >= 6) return;

        uint32_t topVal = top & 0x3FFu;
        uint32_t &cnt = s_caDumpCounts[topVal];
        if (cnt >= 3) return;

        uint32_t fileIndex = s_caTotalDumps++;
        cnt++;

        std::filesystem::create_directories("logs/vu1dump");
        char filename[128];
        std::snprintf(filename, sizeof(filename), "logs/vu1dump/i848_%u_top0x%03X.bin", fileIndex, topVal);

        FILE *f = std::fopen(filename, "wb");
        if (f) {
            std::fwrite(vuData, 1, 16384, f);
            std::fclose(f);
        }

        // Check if onBbRecordUnpack knows the EE source for this MSCAL
        uint32_t srcGuest = 0;
        if (!s_bbUnpacksSinceLastMscal.empty()) {
            srcGuest = s_bbUnpacksSinceLastMscal.front().srcGuest;
        }
        if (srcGuest != 0) {
            uint32_t base = s_movieKickFirstUnpackSrc.load(std::memory_order_relaxed);
            uint32_t mb_idx = (base != 0 && srcGuest >= base) ? (srcGuest - base) / 384u : 0;
            bhLog("[probe] ca src mb_index=%u ee_addr=0x%08X\n", mb_idx, srcGuest);
            ps2xSafeLog("[probe] ca src mb_index=%u ee_addr=0x%08X\n", mb_idx, srcGuest);
        }
    }

    void onCaTickSec() {
        if (!isEnabled()) return;
        uint32_t pc48 = s_caPc48.exchange(0, std::memory_order_relaxed);
        uint32_t pc848 = s_caPc848.exchange(0, std::memory_order_relaxed);
        uint32_t other = s_caPcOther.exchange(0, std::memory_order_relaxed);

        bhLog("[probe] ca pcs pc48=%u pc848=%u other=%u\n", pc48, pc848, other);
        ps2xSafeLog("[probe] ca pcs pc48=%u pc848=%u other=%u\n", pc48, pc848, other);
    }

    // =========================================================================
    // Task CB: Stream-group boundary command ordering probe
    // =========================================================================
    static std::atomic<uint32_t> s_cbEventNum{0};
    static uint32_t s_cbLastF58 = 0xFFFFFFFFu;
    static uint32_t s_cbLastFrame60 = 0xFFFFFFFFu;

    static bool isCbActive(const uint8_t *rdram) {
        if (!isEnabled() || !rdram) return false;
        // Check state at 0x772188: 9, 8, 6, 7
        uint32_t st = 0;
        std::memcpy(&st, rdram + (0x772188u & PS2_RAM_MASK), 4);
        if (st != 9 && st != 8 && st != 6 && st != 7) return false;

        // Path at 0x772210 contains "advertise"
        char pathBuf[64] = {0};
        for (int i = 0; i < 63; ++i) {
            char c = static_cast<char>(rdram[(0x772210u + i) & PS2_RAM_MASK]);
            if (c == 0) break;
            pathBuf[i] = c;
        }
        if (std::strstr(pathBuf, "advertise") == nullptr) return false;
        return true;
    }

    static bool shouldLogCb(const uint8_t *rdram, uint32_t &curF58) {
        if (!isCbActive(rdram)) return false;
        std::memcpy(&curF58, rdram + (0x7721D8u & PS2_RAM_MASK), 4); // f58 = +0x58
        uint32_t n = s_cbEventNum.load(std::memory_order_relaxed);
        if (n < 200) return true;
        if (curF58 != s_cbLastF58) {
            s_cbLastF58 = curF58;
            return true;
        }
        return false;
    }

    void onCbDispEntry(const struct R5900Context *ctx, const uint8_t *rdram, CbDispPre *pre) {
        // Task DN: dc9 disp hook (cap 900 lines when g74 >= 195)
        if (isDcProbeEnabled() && rdram) {
            uint32_t g74 = 0;
            std::memcpy(&g74, rdram + ((0x772180u + 0x74u) & 0x1FFFFFFFu), 4);
            if (g74 >= 195) {
                static std::atomic<uint32_t> s_dc9DispCount{0};
                if (s_dc9DispCount.fetch_add(1, std::memory_order_relaxed) < 900) {
                    uint64_t tick = 0;
                    std::memcpy(&tick, rdram + (0x696FD0u & 0x1FFFFFFFu), 8);
                    uint32_t a0 = getRegU32(ctx, 4);
                    uint32_t a1 = getRegU32(ctx, 5);
                    uint32_t a2 = getRegU32(ctx, 6);
                    uint32_t a3 = getRegU32(ctx, 7);
                    uint32_t n58 = 0, f50 = 0, i60 = 0;
                    std::memcpy(&n58, rdram + ((0x772180u + 0x58u) & 0x1FFFFFFFu), 4);
                    std::memcpy(&f50, rdram + ((0x772180u + 0x50u) & 0x1FFFFFFFu), 4);
                    std::memcpy(&i60, rdram + ((0x772180u + 0x60u) & 0x1FFFFFFFu), 4);
                    std::printf("[probe] dc9 disp tick=%llu cmd=%u a0=0x%08X a2=0x%08X a3=0x%08X g74=%u n58=%u f50=%u i60=%u\n",
                                (unsigned long long)tick, a1, a0, a2, a3, g74, n58, f50, i60);
                    std::fflush(stdout);
                }
            }
        }

        if (!ctx || !pre) return;
        pre->a0 = getRegU32(ctx, 4);
        pre->a1 = getRegU32(ctx, 5);
        pre->a2 = getRegU32(ctx, 6);
        pre->a3 = getRegU32(ctx, 7);
    }

    void onCbDispExit(const struct R5900Context *ctx, const uint8_t *rdram, const CbDispPre *pre) {
        if (!ctx || !rdram || !pre) return;
        uint32_t f58 = 0;
        if (!shouldLogCb(rdram, f58)) return;

        uint32_t n = s_cbEventNum.fetch_add(1, std::memory_order_relaxed);
        uint32_t ret = getRegU32(ctx, 2); // v0

        uint32_t st = 0, f60 = 0, f50 = 0, f74 = 0;
        std::memcpy(&st, rdram + (0x772188u & PS2_RAM_MASK), 4);
        std::memcpy(&f60, rdram + (0x7721E0u & PS2_RAM_MASK), 4);
        std::memcpy(&f50, rdram + (0x7721D0u & PS2_RAM_MASK), 4);
        std::memcpy(&f74, rdram + (0x7721F4u & PS2_RAM_MASK), 4);

        uint32_t head = 0, tail = 0;
        if (pre->a0 >= 0x00100000u && pre->a0 + 0x230u <= PS2_RAM_SIZE) {
            std::memcpy(&head, rdram + ((pre->a0 + 0x228u) & PS2_RAM_MASK), 4);
            std::memcpy(&tail, rdram + ((pre->a0 + 0x22Cu) & PS2_RAM_MASK), 4);
        }

        bhLog("[probe] cb disp n=%u cmd=%u a2=0x%X a3=0x%X ret=0x%X state=%u f58=0x%X f60=%u f50=%u f74=0x%X head=%u tail=%u\n",
              n, pre->a1, pre->a2, pre->a3, ret, st, f58, f60, f50, f74, head, tail);
        ps2xSafeLog("[probe] cb disp n=%u cmd=%u a2=0x%X a3=0x%X ret=0x%X state=%u f58=0x%X f60=%u f50=%u f74=0x%X head=%u tail=%u\n",
                    n, pre->a1, pre->a2, pre->a3, ret, st, f58, f60, f50, f74, head, tail);

        // Check frame60 multiple of 18
        if (f60 > 0 && f60 % 18 == 0 && f60 != s_cbLastFrame60) {
            s_cbLastFrame60 = f60;
            bhLog("[probe] cb frame60=%u\n", f60);
            ps2xSafeLog("[probe] cb frame60=%u\n", f60);
        }
    }

    void onCbPumpEntry(const struct R5900Context *ctx, const uint8_t *rdram, CbPumpPre *pre) {
        if (!rdram || !pre) return;
        std::memcpy(&pre->pre58, rdram + (0x7721D8u & PS2_RAM_MASK), 4);
    }

    void onCbPumpExit(const struct R5900Context *ctx, const uint8_t *rdram, const CbPumpPre *pre) {
        if (!ctx || !rdram || !pre) return;
        uint32_t post58 = 0;
        if (!shouldLogCb(rdram, post58)) return;

        uint32_t n = s_cbEventNum.fetch_add(1, std::memory_order_relaxed);
        uint32_t ret = getRegU32(ctx, 2);
        uint32_t postState = 0;
        std::memcpy(&postState, rdram + (0x772188u & PS2_RAM_MASK), 4);

        int32_t id10 = -1, idC = -1;
        std::memcpy(&id10, rdram + (0x772190u & PS2_RAM_MASK), 4); // +0x10
        std::memcpy(&idC, rdram + (0x77218Cu & PS2_RAM_MASK), 4);  // +0x0C

        uint32_t w10[2] = {0, 0};
        uint32_t wC[2] = {0, 0};
        if (id10 >= 0) {
            uint32_t a = (0x771BBCu + 16u * (uint32_t)id10) & PS2_RAM_MASK;
            std::memcpy(w10, rdram + a, 8);
        }
        if (idC >= 0) {
            uint32_t a = (0x771BBCu + 16u * (uint32_t)idC) & PS2_RAM_MASK;
            std::memcpy(wC, rdram + a, 8);
        }

        bhLog("[probe] cb pump n=%u pre58=0x%X post58=0x%X ret=0x%X state_post=%u slot10=[%d:0x%X,0x%X] slotC=[%d:0x%X,0x%X]\n",
              n, pre->pre58, post58, ret, postState, id10, w10[0], w10[1], idC, wC[0], wC[1]);
        ps2xSafeLog("[probe] cb pump n=%u pre58=0x%X post58=0x%X ret=0x%X state_post=%u slot10=[%d:0x%X,0x%X] slotC=[%d:0x%X,0x%X]\n",
                    n, pre->pre58, post58, ret, postState, id10, w10[0], w10[1], idC, wC[0], wC[1]);
    }

    void onCbRingCall(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!ctx || !rdram) return;
        uint32_t f58 = 0;
        if (!shouldLogCb(rdram, f58)) return;

        uint32_t n = s_cbEventNum.fetch_add(1, std::memory_order_relaxed);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);

        bhLog("[probe] cb ring n=%u a1=%u a2=0x%X f58=0x%X\n", n, a1, a2, f58);
        ps2xSafeLog("[probe] cb ring n=%u a1=%u a2=0x%X f58=0x%X\n", n, a1, a2, f58);
    }

    void onCbCid1Submit(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!ctx || !rdram) return;
        uint32_t f58 = 0;
        if (!shouldLogCb(rdram, f58)) return;

        uint32_t n = s_cbEventNum.fetch_add(1, std::memory_order_relaxed);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        uint32_t t0 = getRegU32(ctx, 8);

        bhLog("[probe] cb video n=%u size=0x%X a1=0x%X a2=0x%X a3=0x%X t0=0x%X\n",
              n, f58, a1, a2, a3, t0);
        ps2xSafeLog("[probe] cb video n=%u size=0x%X a1=0x%X a2=0x%X a3=0x%X t0=0x%X\n",
                    n, f58, a1, a2, a3, t0);
    }

    // =========================================================================
    // Task CC: Audio stream position & SG2 query probe
    // =========================================================================
    static auto s_ccStartTime = std::chrono::steady_clock::now();
    static std::atomic<uint32_t> s_ccPolls{0};
    static std::atomic<uint32_t> s_ccChanges{0};
    static std::atomic<uint32_t> s_ccMaxJump{0};

    static std::atomic<uint32_t> s_ccQCalls{0};
    static uint32_t s_ccLastQV0 = 0xFFFFFFFFu;

    static std::atomic<uint32_t> s_ccSg2Calls{0};
    static uint32_t s_ccLastSg2Ret = 0xFFFFFFFFu;

    void onCc3F7780Exit(const struct R5900Context *ctx, const uint8_t *rdram, uint32_t a0) {
        if (!isEnabled() || !ctx || !rdram || a0 != 0x018BA600u) return;

        s_ccPolls.fetch_add(1, std::memory_order_relaxed);
        uint32_t v0 = getRegU32(ctx, 2);

        uint32_t last = 0, size = 0, state = 0;
        uint64_t voiceMask = 0;
        std::memcpy(&last, rdram + ((a0 + 0x2D4u) & PS2_RAM_MASK), 4);
        std::memcpy(&size, rdram + ((a0 + 0x234u) & PS2_RAM_MASK), 4);
        std::memcpy(&state, rdram + (a0 & PS2_RAM_MASK), 4);
        std::memcpy(&voiceMask, rdram + ((a0 + 0xA0u) & PS2_RAM_MASK), 8);

        if (v0 != last) {
            s_ccChanges.fetch_add(1, std::memory_order_relaxed);
            uint32_t diff = 0;
            if (size > 0) {
                diff = (v0 >= last) ? (v0 - last) : ((v0 + size) - last);
                diff = diff % size;
            } else {
                diff = (v0 >= last) ? (v0 - last) : (last - v0);
            }

            uint32_t curMax = s_ccMaxJump.load(std::memory_order_relaxed);
            while (diff > curMax && !s_ccMaxJump.compare_exchange_weak(curMax, diff, std::memory_order_relaxed)) {}

            auto now = std::chrono::steady_clock::now();
            double t_ms = std::chrono::duration<double, std::milli>(now - s_ccStartTime).count();

            bhLog("[probe] cc pos obj=0x%08X v0=%u last=%u size=%u t_ms=%.1f state=0x%08X mask=0x%016llX\n",
                  a0, v0, last, size, t_ms, state, (unsigned long long)voiceMask);
            ps2xSafeLog("[probe] cc pos obj=0x%08X v0=%u last=%u size=%u t_ms=%.1f state=0x%08X mask=0x%016llX\n",
                        a0, v0, last, size, t_ms, state, (unsigned long long)voiceMask);
        }
    }

    void onCc3F7850Entry(const struct R5900Context *ctx, const uint8_t *rdram) {
        onDoEnqEntry(ctx, rdram);
        if (!isEnabled() || !ctx || !rdram) return;
        uint32_t a0 = getRegU32(ctx, 4);
        if (a0 != 0x018BA600u) return;

        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        uint32_t last = 0;
        std::memcpy(&last, rdram + ((a0 + 0x2D4u) & PS2_RAM_MASK), 4);

        bhLog("[probe] cc enq obj=0x%08X a1=%u a2=0x%08X a3=%u(0x%X) last=%u\n",
              a0, a1, a2, a3, a3, last);
        ps2xSafeLog("[probe] cc enq obj=0x%08X a1=%u a2=0x%08X a3=%u(0x%X) last=%u\n",
                    a0, a1, a2, a3, a3, last);
    }

    struct CcQPre {
        uint32_t a0, a1, a2, a3, ra;
    };
    static CcQPre s_ccLastQPre{};

    void onCc3FBE78Entry(const struct R5900Context *ctx) {
        if (!isEnabled() || !ctx) return;
        s_ccLastQPre.a0 = getRegU32(ctx, 4);
        s_ccLastQPre.a1 = getRegU32(ctx, 5);
        s_ccLastQPre.a2 = getRegU32(ctx, 6);
        s_ccLastQPre.a3 = getRegU32(ctx, 7);
        s_ccLastQPre.ra = getRegU32(ctx, 31);
    }

    void onCc3FBEACExit(const struct R5900Context *ctx) {
        if (!isEnabled() || !ctx) return;
        uint32_t v0 = getRegU32(ctx, 2);
        uint32_t callCount = s_ccQCalls.fetch_add(1, std::memory_order_relaxed);

        if (callCount < 40 || v0 != s_ccLastQV0) {
            s_ccLastQV0 = v0;
            bhLog("[probe] cc q a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X v0=0x%08X ra=0x%08X\n",
                  s_ccLastQPre.a0, s_ccLastQPre.a1, s_ccLastQPre.a2, s_ccLastQPre.a3, v0, s_ccLastQPre.ra);
            ps2xSafeLog("[probe] cc q a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X v0=0x%08X ra=0x%08X\n",
                        s_ccLastQPre.a0, s_ccLastQPre.a1, s_ccLastQPre.a2, s_ccLastQPre.a3, v0, s_ccLastQPre.ra);
        }
    }

    void onCcSg2Call(uint32_t op, uint32_t arg, uint32_t ret) {
        if (!isEnabled()) return;
        uint32_t callCount = s_ccSg2Calls.fetch_add(1, std::memory_order_relaxed);
        if (callCount < 60 || ret != s_ccLastSg2Ret) {
            s_ccLastSg2Ret = ret;
            bhLog("[probe] cc sg2 op=0x%02X arg=0x%08X ret=0x%08X\n", op, arg, ret);
            ps2xSafeLog("[probe] cc sg2 op=0x%02X arg=0x%08X ret=0x%08X\n", op, arg, ret);
        }
    }

    void onCcTickSec() {
        if (!isEnabled()) return;
        uint32_t polls = s_ccPolls.exchange(0, std::memory_order_relaxed);
        uint32_t changes = s_ccChanges.exchange(0, std::memory_order_relaxed);
        uint32_t maxjump = s_ccMaxJump.exchange(0, std::memory_order_relaxed);

        bhLog("[probe] cc sec polls=%u changes=%u maxjump=%u\n", polls, changes, maxjump);
        ps2xSafeLog("[probe] cc sec polls=%u changes=%u maxjump=%u\n", polls, changes, maxjump);
    }

    // =========================================================================
    // Task CE: Frame-locked guest time (PS2X_VTIME)
    // =========================================================================
    static std::atomic<uint32_t> s_ceProduced{0};
    static std::atomic<uint32_t> s_ceDelivered{0};
    static std::atomic<uint32_t> s_ceDropped{0};
    static std::atomic<bool> s_cePendingCause3{false};
    static std::atomic<bool> s_cePendingCause2{false};
    static std::atomic<uint64_t> s_ceDeliveredVblanksTotal{0};
    static int s_ceVTimeEnabled = -1;

    bool isVTimeEnabled() {
        if (s_ceVTimeEnabled == -1) {
            const char *env = std::getenv("PS2X_VTIME");
            if (!env) {
                s_ceVTimeEnabled = 1;
            } else if (env[0] == '0' || env[0] == 'f' || env[0] == 'F' ||
                       !_stricmp(env, "off") || !_stricmp(env, "false")) {
                s_ceVTimeEnabled = 0;
            } else {
                s_ceVTimeEnabled = 1;
            }
        }
        return s_ceVTimeEnabled == 1;
    }

    bool onCeVblankProduced(uint32_t cause) {
        // The virtual clock counts one tick per accepted VBLANK EVENT (cause 3). It must not count handler
        // invocations: cause 3 has two guest handlers, so counting deliveries doubled the audio clock.
        if (!isVTimeEnabled()) {
            s_ceProduced.fetch_add(1, std::memory_order_relaxed);
            if (cause == 3u) s_ceDeliveredVblanksTotal.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (cause == 3u) {
            if (s_cePendingCause3.exchange(true, std::memory_order_acq_rel)) {
                s_ceDropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            s_ceProduced.fetch_add(1, std::memory_order_relaxed);
            s_ceDeliveredVblanksTotal.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (cause == 2u) {
            if (s_cePendingCause2.exchange(true, std::memory_order_acq_rel)) {
                s_ceDropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            s_ceProduced.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return true;
    }

    void onCeVblankDelivered(uint32_t cause) {
        if (cause == 3u) {
            s_cePendingCause3.store(false, std::memory_order_release);
            s_ceDelivered.fetch_add(1, std::memory_order_relaxed);
        } else if (cause == 2u) {
            s_cePendingCause2.store(false, std::memory_order_release);
            s_ceDelivered.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64_t getCeDeliveredVblanks() {
        return s_ceDeliveredVblanksTotal.load(std::memory_order_relaxed);
    }

    void onCeTickSec() {
        if (!isEnabled()) return;
        uint32_t prod = s_ceProduced.exchange(0, std::memory_order_relaxed);
        uint32_t deliv = s_ceDelivered.exchange(0, std::memory_order_relaxed);
        uint32_t drop = s_ceDropped.exchange(0, std::memory_order_relaxed);
        uint64_t totalDeliv = s_ceDeliveredVblanksTotal.load(std::memory_order_relaxed);
        uint32_t sg2_pos = s_ccLastSg2Ret;
        uint32_t guest_ticks = s_ceLastGuestTicks.load(std::memory_order_relaxed);

        bhLog("[probe] ce vblank produced=%u delivered=%u dropped=%u\n", prod, deliv, drop);
        ps2xSafeLog("[probe] ce vblank produced=%u delivered=%u dropped=%u\n", prod, deliv, drop);

        bhLog("[probe] ce sec vblank_delivered=%llu sg2_pos=0x%08X guest_ticks=%u\n",
              (unsigned long long)totalDeliv, sg2_pos, guest_ticks);
        ps2xSafeLog("[probe] ce sec vblank_delivered=%llu sg2_pos=0x%08X guest_ticks=%u\n",
                    (unsigned long long)totalDeliv, sg2_pos, guest_ticks);
    }

    // =========================================================================
    // Task DC / DD Probes: sceGsSyncV & GS wait tracking
    // =========================================================================
    bool isDcProbeEnabled() {
        static int s_dcEnv = -1;
        if (s_dcEnv == -1) {
            const char *env = std::getenv("PS2X_PROBE_DC");
            const char *envAe = std::getenv("PS2X_PROBE_AE");
            bool on = (env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0)) ||
                      (envAe && (std::strcmp(envAe, "1") == 0 || std::strcmp(envAe, "true") == 0));
            s_dcEnv = on ? 1 : 0;
        }
        return s_dcEnv == 1;
    }

    static void dcLog(const char *fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        std::printf("%s", buf);
        std::fflush(stdout);

        FILE *f = bhSink();
        if (f) {
            std::fputs(buf, f);
            std::fflush(f);
        }
    }

    static std::mutex s_dcMutex;
    static uint32_t s_dcCalls = 0;
    static std::map<uint32_t, uint32_t> s_dcRaCounts;
    static std::map<uint32_t, double> s_dcRaMs;
    static uint32_t s_dcLastMode = 0;
    static uint32_t s_dcLastA0 = 0;
    static uint32_t s_dcRetParity0 = 0;
    static uint32_t s_dcRetParity1 = 0;
    static uint32_t s_dcDelta1 = 0;
    static uint32_t s_dcDelta2 = 0;
    static uint32_t s_dcDelta3 = 0;
    static uint32_t s_dcDelta4p = 0;
    static uint32_t s_dcSwapDBuff = 0;
    static uint32_t s_dcResetPath = 0;
    static uint32_t s_dcSyncPath = 0;
    static uint32_t s_dcLastCtr = 0;
    static auto s_dcLastSec = std::chrono::steady_clock::now();

    // DC2 (DD) state
    static int s_dc2MovieThid = -1;
    static bool s_dc2MovieWoken = false;
    static std::chrono::steady_clock::time_point s_dc2WakeTime{};
    static double s_dc2SecWakeToRunMs = 0.0;
    static uint32_t s_dc2SecWaitN = 0;
    static double s_dc2SecWaitMsTotal = 0.0;

    static int s_dc2CurRunningThid = -1;
    static uint32_t s_dc2CurRunningEntry = 0;
    static std::chrono::steady_clock::time_point s_dc2CurSliceStart{};
    static std::map<std::pair<int, uint32_t>, double> s_dc2InterveningThreads;

    static uint32_t s_dc2VbProduced = 0;
    static uint32_t s_dc2VbProcessed = 0;
    static double s_dc2LateMsMax = 0.0;

    // Task DE (dc3) state
    static uint32_t s_dc3LastG686fd0 = 0;
    static bool s_dc3HasLastG686fd0 = false;
    static uint32_t s_dc3Irq3Accepted = 0;
    static uint32_t s_dc3Irq3Delivered = 0;
    static uint32_t s_dc3Irq3Dropped = 0;
    static uint32_t s_dc3Handler31a3e8Calls = 0;

    static uint32_t s_dc3Irq2Accepted = 0;
    static uint32_t s_dc3Irq2Delivered = 0;
    static uint32_t s_dc3Irq2Dropped = 0;

    
    // =========================================================================
    // Task DI: Telemetry & Event Ring
    // =========================================================================
    extern "C" uint32_t ps2_getSg2Pos();
    extern "C" void ps2_dumpDkSchedulerState();
    extern "C" void ps2_getDkThread1Info(int *outCurTh, const char **outTh1Status, uint32_t *outTh1Pc, uint32_t *outTh1Ra);
extern "C" void ps2_dumpDkCid1State(const uint8_t *rdram);


    struct DiEvent {
        char kind[16];
        uint32_t a0, a1, a2, a3;
        uint64_t tick;
        uint32_t n58;
        uint32_t g74;
    };
    static std::mutex s_diMutex;
    static DiEvent s_diEvents[32];
    static uint64_t s_diEventTotal = 0;
    static std::atomic<uint32_t> s_diRingCalls{0};
    static std::atomic<uint32_t> s_diVideoCalls{0};
    static std::atomic<uint32_t> s_diDispCalls{0};
    static std::atomic<uint32_t> s_diPumpCalls{0};
    static std::atomic<uint32_t> s_diFCallsSec{0};
    static bool s_diFEverStarted = false;
    static uint32_t s_diFZeroSecConsecutive = 0;
    static bool s_diStallDumped = false;

    static void recordDiEvent(const char *kind, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, const uint8_t *rdram) {
        if (!isDcProbeEnabled()) return;
        const uint8_t *ram = rdram ? rdram : s_cachedRdram;
        uint32_t n58 = 0, g74 = 0;
        uint64_t tick = 0;
        if (ram) {
            std::memcpy(&n58, ram + ((0x772180u + 0x58u) & 0x1FFFFFFFu), 4);
            std::memcpy(&g74, ram + ((0x772180u + 0x74u) & 0x1FFFFFFFu), 4);
            std::memcpy(&tick, ram + (0x696FD0u & 0x1FFFFFFFu), 8);
        }
        std::lock_guard<std::mutex> lock(s_diMutex);
        uint32_t slot = static_cast<uint32_t>(s_diEventTotal % 32);
        s_diEventTotal++;
        DiEvent &ev = s_diEvents[slot];
        std::strncpy(ev.kind, kind, sizeof(ev.kind) - 1);
        ev.kind[sizeof(ev.kind) - 1] = '\0';
        ev.a0 = a0; ev.a1 = a1; ev.a2 = a2; ev.a3 = a3;
        ev.tick = tick;
        ev.n58 = n58;
        ev.g74 = g74;
    }

    
    std::atomic<uint32_t> s_duDecodesSinceLastPresent{0};
    std::atomic<uint32_t> s_duDecodesThisSec{0};
    std::atomic<uint32_t> s_duPresentsThisSec{0};

    static void dumpDcStatsLocked() {
        std::vector<std::pair<uint32_t, uint32_t>> raSorted(s_dcRaCounts.begin(), s_dcRaCounts.end());
        std::sort(raSorted.begin(), raSorted.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });

        std::string raHistStr;
        const size_t raLimit = std::min<size_t>(4u, raSorted.size());
        for (size_t i = 0; i < raLimit; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s0x%08X:%u", (i > 0 ? " " : ""), raSorted[i].first, raSorted[i].second);
            raHistStr += buf;
        }

        dcLog("[probe] dc syncv calls=%u ra_hist=<%s> mode=%u a0=%u ret_parity0=%u ret_parity1=%u\n",
              s_dcCalls, raHistStr.c_str(), s_dcLastMode, s_dcLastA0, s_dcRetParity0, s_dcRetParity1);

        dcLog("[probe] dc syncv_delta hist={1:%u,2:%u,3:%u,4+:%u}\n",
              s_dcDelta1, s_dcDelta2, s_dcDelta3, s_dcDelta4p);

        for (const auto &[ra, ms] : s_dcRaMs) {
            dcLog("[probe] dc syncv_ms ra=0x%08X ms=%.2f\n", ra, ms);
        }

        dcLog("[probe] dc stubs swap_dbuff=%u reset_path=%u sync_path=%u\n",
              s_dcSwapDBuff, s_dcResetPath, s_dcSyncPath);

        dcLog("[probe] dc ctr=%u\n", s_dcLastCtr);

        // Task DD (dc2) dumps
        dcLog("[probe] dc2 wait ms_total=%.2f n=%u wake_to_run_ms=%.2f\n",
              s_dc2SecWaitMsTotal, s_dc2SecWaitN, s_dc2SecWakeToRunMs);

        // Who ran
        std::vector<std::pair<std::pair<int, uint32_t>, double>> whoSorted(s_dc2InterveningThreads.begin(), s_dc2InterveningThreads.end());
        std::sort(whoSorted.begin(), whoSorted.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });
        std::string whoStr;
        const size_t whoLimit = std::min<size_t>(4u, whoSorted.size());
        for (size_t i = 0; i < whoLimit; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s[th%d:0x%08X]=%.2fms",
                          (i > 0 ? " " : ""), whoSorted[i].first.first, whoSorted[i].first.second, whoSorted[i].second);
            whoStr += buf;
        }
        dcLog("[probe] dc2 who %s\n", whoStr.c_str());

        // VBlank start timing
        dcLog("[probe] dc2 vb produced=%u processed_ticks=%u late_ms_max=%.2f\n",
              s_dc2VbProduced, s_dc2VbProcessed, s_dc2LateMsMax);

        // Task DE (dc3) dump
        const uint8_t *ram = s_cachedRdram;
        uint32_t g686fd0 = 0;
        uint32_t ctx1f8 = 0;
        uint32_t ctx_ptr_a1src = 0;
        if (ram) {
            std::memcpy(&g686fd0, ram + (0x686FD0u & PS2_RAM_MASK), 4);
            std::memcpy(&ctx1f8, ram + (0x772378u & PS2_RAM_MASK), 4);
            std::memcpy(&ctx_ptr_a1src, ram + ((0x772180u + 0x58u) & PS2_RAM_MASK), 4);
        }
        uint32_t g686fd0_delta = 0;
        if (s_dc3HasLastG686fd0) {
            g686fd0_delta = (g686fd0 >= s_dc3LastG686fd0) ? (g686fd0 - s_dc3LastG686fd0) : (g686fd0 + (0xFFFFFFFFu - s_dc3LastG686fd0) + 1u);
        }
        s_dc3LastG686fd0 = g686fd0;
        s_dc3HasLastG686fd0 = true;

        dcLog("[probe] dc3 g686fd0=%u g686fd0_delta=%u ctx1f8=%u ctx_ptr_a1src=0x%08X\n",
              g686fd0, g686fd0_delta, ctx1f8, ctx_ptr_a1src);
        dcLog("[probe] dc3 irq3 accepted=%u delivered=%u coalesced_dropped=%u handler31a3e8_calls=%u\n",
              s_dc3Irq3Accepted, s_dc3Irq3Delivered, s_dc3Irq3Dropped, s_dc3Handler31a3e8Calls);
        dcLog("[probe] dc3 irq2 accepted=%u delivered=%u coalesced_dropped=%u\n",
              s_dc3Irq2Accepted, s_dc3Irq2Delivered, s_dc3Irq2Dropped);

        // Task DI: 1. dc5 ctx per second
        {
            uint32_t st = 0, b50 = 0, n58 = 0, i60 = 0, i64 = 0, g74 = 0, p70 = 0, rdy = 0;
            uint64_t vbl = 0;
            if (ram) {
                std::memcpy(&st, ram + ((0x772180u + 0x08u) & 0x1FFFFFFFu), 4);
                std::memcpy(&b50, ram + ((0x772180u + 0x50u) & 0x1FFFFFFFu), 4);
                std::memcpy(&n58, ram + ((0x772180u + 0x58u) & 0x1FFFFFFFu), 4);
                std::memcpy(&i60, ram + ((0x772180u + 0x60u) & 0x1FFFFFFFu), 4);
                std::memcpy(&i64, ram + ((0x772180u + 0x64u) & 0x1FFFFFFFu), 4);
                std::memcpy(&g74, ram + ((0x772180u + 0x74u) & 0x1FFFFFFFu), 4);
                std::memcpy(&p70, ram + ((0x772180u + 0x70u) & 0x1FFFFFFFu), 4);
                uint32_t p4c = 0;
                std::memcpy(&p4c, ram + ((0x772180u + 0x4Cu) & 0x1FFFFFFFu), 4);
                if (p4c != 0 && (p4c & 0x1FFFFFFFu) + 0x14u <= PS2_RAM_SIZE) {
                    std::memcpy(&rdy, ram + (((p4c & 0x1FFFFFFFu) + 0x10u) & 0x1FFFFFFFu), 4);
                }
                std::memcpy(&vbl, ram + (0x696FD0u & 0x1FFFFFFFu), 8);
            }
            uint32_t sg2pos = ps2_getSg2Pos();
            dcLog("[probe] dc5 ctx st=%u b50=%u n58=%u i60=%u i64=%u g74=%u p70=%u rdy=%u sg2pos=%u vbl=%llu\n",
                  st, b50, n58, i60, i64, g74, p70, rdy, sg2pos, (unsigned long long)vbl);
        }

        // Task DI: 2. dc5 rdcnt per second
        {
            uint32_t ringCnt = s_diRingCalls.exchange(0, std::memory_order_relaxed);
            uint32_t videoCnt = s_diVideoCalls.exchange(0, std::memory_order_relaxed);
            uint32_t dispCnt = s_diDispCalls.exchange(0, std::memory_order_relaxed);
            uint32_t pumpCnt = s_diPumpCalls.exchange(0, std::memory_order_relaxed);
            dcLog("[probe] dc5 rdcnt ring=%u video=%u disp=%u pump=%u\n",
                  ringCnt, videoCnt, dispCnt, pumpCnt);
        dcLog("[probe] dc12 sched preempt_policy=EeScheduler::checkCheckpoint on requestPreemptionIfHigher or timeslice expiry\n");
        ps2_vu1_dumpDyPerSec(ram);
        // Task DU: dc13 rate
        {
            uint32_t p = s_duPresentsThisSec.exchange(0, std::memory_order_relaxed);
            uint32_t d = s_duDecodesThisSec.exchange(0, std::memory_order_relaxed);
            std::printf("[probe] dc13 rate presents=%u guest_display_copies=0 decodes=%u\n", p, d);
            std::fflush(stdout);
        }

        }

        // Task DI: 3. Stall dump when F==0 for 3 consecutive seconds after F>0
        {
            uint32_t fCalls = s_diFCallsSec.exchange(0, std::memory_order_relaxed);
            if (fCalls > 0) {
                s_diFEverStarted = true;
                s_diFZeroSecConsecutive = 0;
            } else if (s_diFEverStarted) {
                s_diFZeroSecConsecutive++;
                if (s_diFZeroSecConsecutive == 3 || s_diFZeroSecConsecutive == 13) {
                    if (s_diFZeroSecConsecutive == 3) s_diStallDumped = true;
                    dcLog("[probe] dc5 STALL (seq=%u)\n", (s_diFZeroSecConsecutive == 3 ? 1u : 2u));
                    dumpDmState(ram, "stall");
                    ps2_dumpDkSchedulerState();
                    ps2_dumpDkCid1State(ram);
                    std::lock_guard<std::mutex> lock(s_diMutex);
                    uint64_t total = s_diEventTotal;
                    uint64_t count = (total < 32) ? total : 32;
                    uint64_t start = total - count;
                    for (uint64_t i = 0; i < count; ++i) {
                        uint64_t idx = start + i;
                        const DiEvent &ev = s_diEvents[idx % 32];
                        dcLog("[probe] dc5 ev i=%llu kind=%s a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X tick=%llu n58=%u g74=%u\n",
                              (unsigned long long)i, ev.kind, ev.a0, ev.a1, ev.a2, ev.a3,
                              (unsigned long long)ev.tick, ev.n58, ev.g74);
                    }
                }
            }
        }

        s_dc3Irq3Accepted = 0;
        s_dc3Irq3Delivered = 0;
        s_dc3Irq3Dropped = 0;
        s_dc3Handler31a3e8Calls = 0;
        s_dc3Irq2Accepted = 0;
        s_dc3Irq2Delivered = 0;
        s_dc3Irq2Dropped = 0;

        s_dcCalls = 0;
        s_dcRaCounts.clear();
        s_dcRaMs.clear();
        s_dcRetParity0 = 0;
        s_dcRetParity1 = 0;
        s_dcDelta1 = 0;
        s_dcDelta2 = 0;
        s_dcDelta3 = 0;
        s_dcDelta4p = 0;
        s_dcSwapDBuff = 0;
        s_dcResetPath = 0;
        s_dcSyncPath = 0;

        s_dc2SecWakeToRunMs = 0.0;
        s_dc2SecWaitN = 0;
        s_dc2SecWaitMsTotal = 0.0;
        s_dc2InterveningThreads.clear();
        s_dc2VbProduced = 0;
        s_dc2VbProcessed = 0;
        s_dc2LateMsMax = 0.0;
    }

    static void checkDcSecDumpLocked() {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_dcLastSec).count() >= 1000) {
            s_dcLastSec = now;
            dumpDcStatsLocked();
        }
    }

    void onDcSyncVStart(uint32_t ra, uint32_t a0, bool interlace, const uint8_t *rdram, int thid) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        s_dcCalls++;
        s_dcRaCounts[ra]++;
        s_dcLastMode = interlace ? 1u : 0u;
        s_dcLastA0 = a0;
        if (ra == 0x51E3D0u) {
            s_dc2MovieThid = thid;
        }

        const uint8_t *ram = rdram ? rdram : s_cachedRdram;
        if (ram) {
            uint32_t val = 0;
            std::memcpy(&val, ram + (0x72D498u & PS2_RAM_MASK), 4);
            s_dcLastCtr = val;
        }

        checkDcSecDumpLocked();
    }

    void onDcSyncVComplete(uint32_t ra, uint64_t deltaTicks, uint32_t retVal, double waitMs) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        if ((retVal & 1u) == 0u) {
            s_dcRetParity0++;
        } else {
            s_dcRetParity1++;
        }

        if (deltaTicks <= 1u) {
            s_dcDelta1++;
        } else if (deltaTicks == 2u) {
            s_dcDelta2++;
        } else if (deltaTicks == 3u) {
            s_dcDelta3++;
        } else {
            s_dcDelta4p++;
        }

        s_dcRaMs[ra] += waitMs;

        if (ra == 0x51E3D0u) {
            s_dc2SecWaitN++;
            s_dc2SecWaitMsTotal += waitMs;
        }

        checkDcSecDumpLocked();
    }

    void onDcSwapDBuff() {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        s_dcSwapDBuff++;
    }

    void onDcResetPath() {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        s_dcResetPath++;
    }

    void onDcSyncPath() {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        s_dcSyncPath++;
    }

    void onDc2VBlankStart(double lateMs) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        s_dc2VbProcessed++;
        if (lateMs > s_dc2LateMsMax) {
            s_dc2LateMsMax = lateMs;
        }
        checkDcSecDumpLocked();
    }

    void onDc2VBlankProduced() {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        s_dc2VbProduced++;
    }

    void onDc2ThreadWake(int thid) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        if (thid == s_dc2MovieThid && s_dc2MovieThid != -1) {
            s_dc2MovieWoken = true;
            s_dc2WakeTime = std::chrono::steady_clock::now();
        }
    }

    void onDc2ThreadDispatch(int thid, uint32_t entryPc) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        const auto now = std::chrono::steady_clock::now();

        if (s_dc2CurRunningThid != -1 && s_dc2MovieWoken && s_dc2CurRunningThid != s_dc2MovieThid) {
            const double elapsed = std::chrono::duration<double, std::milli>(now - s_dc2CurSliceStart).count();
            s_dc2InterveningThreads[{s_dc2CurRunningThid, s_dc2CurRunningEntry}] += elapsed;
        }

        if (s_dc2MovieWoken && thid == s_dc2MovieThid && s_dc2MovieThid != -1) {
            const double wakeToRun = std::chrono::duration<double, std::milli>(now - s_dc2WakeTime).count();
            s_dc2SecWakeToRunMs += wakeToRun;
            s_dc2MovieWoken = false;
        }

        s_dc2CurRunningThid = thid;
        s_dc2CurRunningEntry = entryPc;
        s_dc2CurSliceStart = now;

        checkDcSecDumpLocked();
    }

    void onDc3IrqProduced(uint32_t cause, bool accepted) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        if (cause == 3u) {
            if (accepted) s_dc3Irq3Accepted++;
            else s_dc3Irq3Dropped++;
        } else if (cause == 2u) {
            if (accepted) s_dc3Irq2Accepted++;
            else s_dc3Irq2Dropped++;
        }
    }

    void onDc3IrqDelivered(uint32_t cause, uint32_t pc) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dcMutex);
        if (cause == 3u) {
            s_dc3Irq3Delivered++;
            if (pc == 0x31A3E8u) {
                s_dc3Handler31a3e8Calls++;
            }
        } else if (cause == 2u) {
            s_dc3Irq2Delivered++;
        }
    }

    // =========================================================================
    // Task DF Probes: values flowing through movie tick
    // =========================================================================
    static std::mutex s_dc4Mutex;
    static uint32_t s_dc4GCalls = 0;
    static uint32_t s_dc4LoggedCount = 0;

    void onDc4GEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dc4Mutex);
        s_dc4GCalls++;
        if (s_dc4GCalls > 200 && s_dc4LoggedCount < 150) {
            s_dc4LoggedCount++;
            uint32_t delta = ctx ? getRegU32(ctx, 4) : 0u;
            const uint8_t *ram = rdram ? rdram : s_cachedRdram;
            uint64_t cnt696fd0 = 0;
            if (ram) {
                std::memcpy(&cnt696fd0, ram + (0x696FD0u & PS2_RAM_MASK), 8);
            }
            dcLog("[probe] dc4 G delta=%u cnt696fd0=%llu\n", delta, (unsigned long long)cnt696fd0);
        }
    }

    void onDc4FEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        onDhFEntry(ctx, rdram);
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dc4Mutex);
        if (s_dc4GCalls > 200 && s_dc4LoggedCount < 150) {
            s_dc4LoggedCount++;
            uint32_t a0 = ctx ? getRegU32(ctx, 4) : 0u;
            uint32_t s4 = ctx ? getRegU32(ctx, 5) : 0u;
            const uint8_t *ram = rdram ? rdram : s_cachedRdram;
            uint32_t state = 0, idx60 = 0, idx64 = 0, lim5c = 0;
            if (ram && a0 != 0u) {
                std::memcpy(&state, ram + ((a0 + 8u) & PS2_RAM_MASK), 4);
                std::memcpy(&idx60, ram + ((a0 + 0x60u) & PS2_RAM_MASK), 4);
                std::memcpy(&idx64, ram + ((a0 + 0x64u) & PS2_RAM_MASK), 4);
                std::memcpy(&lim5c, ram + ((a0 + 0x5Cu) & PS2_RAM_MASK), 4);
            }
            dcLog("[probe] dc4 F s4=%u state=%u idx60=%u idx64=%u lim5c=%u\n",
                  s4, state, idx60, idx64, lim5c);
        }
    }

    void onDc4DEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dc4Mutex);
        if (s_dc4GCalls > 200 && s_dc4LoggedCount < 150) {
            s_dc4LoggedCount++;
            uint32_t a0 = ctx ? getRegU32(ctx, 4) : 0u;
            uint32_t a1 = ctx ? getRegU32(ctx, 5) : 0u;
            uint32_t a2 = ctx ? getRegU32(ctx, 6) : 0u;
            const uint8_t *ram = rdram ? rdram : s_cachedRdram;
            uint32_t thr1f8 = 0;
            if (ram && a0 != 0u) {
                std::memcpy(&thr1f8, ram + ((a0 + 0x1F8u) & PS2_RAM_MASK), 4);
            }
            dcLog("[probe] dc4 D a0=0x%08X a1=%u a2=%u thr1f8=%u\n",
                  a0, a1, a2, thr1f8);
        }
    }

    void onDc4DecEntry() {
        s_duDecodesSinceLastPresent.fetch_add(1, std::memory_order_relaxed);
        s_duDecodesThisSec.fetch_add(1, std::memory_order_relaxed);
        if (!isDcProbeEnabled()) return;
        std::lock_guard<std::mutex> lock(s_dc4Mutex);
        if (s_dc4GCalls > 200 && s_dc4LoggedCount < 150) {
            s_dc4LoggedCount++;
            dcLog("[probe] dc4 DEC\n");
        }
    }
}

    
namespace probe_ae {
// =========================================================================
    // Task DH: Movie stream feed deadlock diagnostics
    // =========================================================================
    extern "C" uint32_t ps2_getSg2Pos();
    extern "C" void ps2_dumpDkSchedulerState();
    extern "C" void ps2_getDkThread1Info(int *outCurTh, const char **outTh1Status, uint32_t *outTh1Pc, uint32_t *outTh1Ra);
extern "C" void ps2_dumpDkCid1State(const uint8_t *rdram);


    struct DhEvent {
        char name[16];
        uint32_t a0, a1, a2, a3;
        uint64_t tick;
        uint32_t st, b50, n58, i60, i64, g74, c70;
    };

    static std::mutex s_dhMutex;
    static DhEvent s_dhRing[64];
    static uint64_t s_dhRingTotal = 0;
    static std::atomic<bool> s_dhRingDumped{false};
    static std::atomic<bool> s_dhIntroMovieStarted{false};
    static std::atomic<uint32_t> s_dhReadLogsCount{0};
    static std::chrono::steady_clock::time_point s_dhLastFTime;
    static std::atomic<bool> s_dhFEverEntered{false};

    static void dhRecordEvent(const char *name, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, const uint8_t *rdram) {
        if (!isDcProbeEnabled()) return;
        uint32_t st = 0, b50 = 0, n58 = 0, i60 = 0, i64 = 0, g74 = 0, c70 = 0;
        uint64_t tick = 0;
        if (rdram) {
            std::memcpy(&st, rdram + ((0x772180u + 0x08u) & PS2_RAM_MASK), 4);
            std::memcpy(&b50, rdram + ((0x772180u + 0x50u) & PS2_RAM_MASK), 4);
            std::memcpy(&n58, rdram + ((0x772180u + 0x58u) & PS2_RAM_MASK), 4);
            std::memcpy(&i60, rdram + ((0x772180u + 0x60u) & PS2_RAM_MASK), 4);
            std::memcpy(&i64, rdram + ((0x772180u + 0x64u) & PS2_RAM_MASK), 4);
            std::memcpy(&c70, rdram + ((0x772180u + 0x70u) & PS2_RAM_MASK), 4);
            std::memcpy(&g74, rdram + ((0x772180u + 0x74u) & PS2_RAM_MASK), 4);
            std::memcpy(&tick, rdram + (0x696FD0u & PS2_RAM_MASK), 8);
        }
        std::lock_guard<std::mutex> lock(s_dhMutex);
        uint32_t slot = static_cast<uint32_t>(s_dhRingTotal % 64);
        s_dhRingTotal++;
        DhEvent &ev = s_dhRing[slot];
        std::strncpy(ev.name, name, sizeof(ev.name) - 1);
        ev.name[sizeof(ev.name) - 1] = '\0';
        ev.a0 = a0; ev.a1 = a1; ev.a2 = a2; ev.a3 = a3;
        ev.tick = tick;
        ev.st = st; ev.b50 = b50; ev.n58 = n58; ev.i60 = i60; ev.i64 = i64; ev.g74 = g74; ev.c70 = c70;
    }

    void onDhFEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isDcProbeEnabled()) return;
        s_dhLastFTime = std::chrono::steady_clock::now();
        s_dhFEverEntered.store(true, std::memory_order_relaxed);
        s_diFCallsSec.fetch_add(1, std::memory_order_relaxed);
    }

    void onDhBzInitEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isDcProbeEnabled() || !ctx) return;
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        if (a2 == 512 && a3 == 320) {
            s_dhIntroMovieStarted.store(true, std::memory_order_relaxed);
        }
    }

    void onDhDispEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isDcProbeEnabled() || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        dhRecordEvent("dispatcher", a0, a1, a2, 0, rdram);
        recordDiEvent("disp", a0, a1, a2, 0, rdram);
        if (a1 == 1) s_diDispCalls.fetch_add(1, std::memory_order_relaxed);
        if (a1 == 8) s_diPumpCalls.fetch_add(1, std::memory_order_relaxed);
    }

    void onDhPumpEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isDcProbeEnabled() || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        dhRecordEvent("pump", a0, a1, a2, a3, rdram);
        recordDiEvent("pump", a0, a1, a2, a3, rdram);
    }

    void onDhRingReadEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isDcProbeEnabled() || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        dhRecordEvent("ring_read", a0, a1, a2, a3, rdram);
        recordDiEvent("ring", a0, a1, a2, a3, rdram);
        s_diRingCalls.fetch_add(1, std::memory_order_relaxed);
        if (s_dhIntroMovieStarted.load(std::memory_order_relaxed)) {
            uint32_t cnt = s_dhReadLogsCount.fetch_add(1, std::memory_order_relaxed);
            if (cnt < 400) {
                uint64_t tick = 0;
                uint32_t n58 = 0;
                if (rdram) {
                    std::memcpy(&tick, rdram + (0x696FD0u & PS2_RAM_MASK), 8);
                    std::memcpy(&n58, rdram + ((0x772180u + 0x58u) & PS2_RAM_MASK), 4);
                }
                dcLog("[probe] dc5 rd kind=ring a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X tick=%llu n58=%u\n",
                      a0, a1, a2, a3, (unsigned long long)tick, n58);
            }
        }
    }

    void onDhVideoReadEntry(const struct R5900Context *ctx, const uint8_t *rdram) {
        if (!isDcProbeEnabled() || !ctx) return;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t a3 = getRegU32(ctx, 7);
        dhRecordEvent("video_read", a0, a1, a2, a3, rdram);
        recordDiEvent("video", a0, a1, a2, a3, rdram);
        s_diVideoCalls.fetch_add(1, std::memory_order_relaxed);
        if (s_dhIntroMovieStarted.load(std::memory_order_relaxed)) {
            uint32_t cnt = s_dhReadLogsCount.fetch_add(1, std::memory_order_relaxed);
            if (cnt < 400) {
                uint64_t tick = 0;
                uint32_t n58 = 0;
                if (rdram) {
                    std::memcpy(&tick, rdram + (0x696FD0u & PS2_RAM_MASK), 8);
                    std::memcpy(&n58, rdram + ((0x772180u + 0x58u) & PS2_RAM_MASK), 4);
                }
                dcLog("[probe] dc5 rd kind=video a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X tick=%llu n58=%u\n",
                      a0, a1, a2, a3, (unsigned long long)tick, n58);
            }
        }
    }

} // namespace probe_ae
