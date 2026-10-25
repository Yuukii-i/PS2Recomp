#include "probe_ae.h"

extern std::atomic<uint64_t> g_profPrims;
extern std::atomic<uint64_t> g_profPixels;
extern void ps2xSafeLog(const char *fmt, ...);

#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include <mutex>
#include <map>
#include <deque>
#include <vector>
#include <array>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <unordered_set>
#include <set>

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

    void recordMainThreadDispatch(uint32_t pc) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(s_ringMutex);
        s_mainRing[s_mainRingIdx++ % 16] = pc;
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
    enum class KickDecision {
        Undecided,
        Native,
        Interpret
    };
    static KickDecision s_kickDecision = KickDecision::Undecided;

    void onBbChainStart(uint32_t tadr, uint32_t tagCount, const std::vector<AySpan> &spans) {
        s_movieKickMscalIndex.store(0, std::memory_order_relaxed);
        s_movieKickFirstUnpackSrc.store(0, std::memory_order_relaxed);
        s_kickDecision = KickDecision::Undecided;
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

    void onBbRecordUnpack(uint32_t posPayload, uint32_t totalBytes, uint32_t vuAddr, uint8_t fmt, uint8_t num) {
        if (!isEnabled() && !isMovieHleActive() && !isMovieHleVerify()) return;
        std::lock_guard<std::mutex> lock(s_bbMutex);
        uint32_t srcGuest = 0;
        const auto &spans = !s_bbSpans.empty() ? s_bbSpans : s_aySpans;
        for (const auto &sp : spans) {
            if (posPayload >= sp.bufOffset && posPayload < sp.bufOffset + sp.byteCount) {
                srcGuest = sp.srcGuestAddr + (posPayload - sp.bufOffset);
                break;
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
                ps2xSafeLog("[probe] bh movie_active reason=%s\n", reason);
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

        // Preallocated packets:
        // Packet 1: 96 bytes header + 393,216 bytes image data (rows 0..191)
        // Packet 2: 96 bytes header + 393,216 bytes image data (rows 192..383)
        // Packet 3: 80 bytes restore A+D
        static std::vector<uint8_t> s_pkt1(96 + 393216, 0);
        static std::vector<uint8_t> s_pkt2(96 + 393216, 0);
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
        const uint64_t trxreg_val = (512ull) | (192ull << 32u);
        const uint64_t trxdir_val = 0ull;

        writeSetupPacket(s_pkt1, bitbltbuf_val, 0ull, trxreg_val, trxdir_val, 24576);
        const uint64_t trxpos2_val = (static_cast<uint64_t>(192 & 0x7FFu) << 48u);
        writeSetupPacket(s_pkt2, bitbltbuf_val, trxpos2_val, trxreg_val, trxdir_val, 24576);

        uint32_t *pkt1_pixels = reinterpret_cast<uint32_t*>(s_pkt1.data() + 96);
        uint32_t *pkt2_pixels = reinterpret_cast<uint32_t*>(s_pkt2.data() + 96);

        // Native YCbCr -> RGB conversion directly into packet payloads
        for (uint32_t k = 0; k < 768u; ++k) {
            const uint32_t mbx = k / 24u;
            const uint32_t mby = k % 24u;
            const uint32_t mb_base = (base + k * 384u) & PS2_RAM_MASK;
            if (mb_base + 384u > PS2_RAM_SIZE) continue;

            const uint8_t *mbY = rdram + mb_base;
            const uint8_t *mbCb = rdram + mb_base + 256u;
            const uint8_t *mbCr = rdram + mb_base + 320u;

            const uint32_t startX = mbx * 16u;
            const uint32_t startY = mby * 16u;

            for (uint32_t py = 0; py < 16u; ++py) {
                const uint32_t y = startY + py;
                const uint32_t py_chroma = (py >> 1u) * 8u;

                uint32_t *rowDst = (y < 192u) ? (pkt1_pixels + y * 512u + startX)
                                              : (pkt2_pixels + (y - 192u) * 512u + startX);

                for (uint32_t px = 0; px < 16u; ++px) {
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
            memory->submitGifPacket(GifPathId::Path1, s_pkt2.data(), static_cast<uint32_t>(s_pkt2.size()));
            memory->submitGifPacket(GifPathId::Path1, s_pktRestore.data(), static_cast<uint32_t>(s_pktRestore.size()));
        } else {
            gs.processGIFPacket(s_pkt1.data(), static_cast<uint32_t>(s_pkt1.size()));
            gs.processGIFPacket(s_pkt2.data(), static_cast<uint32_t>(s_pkt2.size()));
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

    bool handleMovieHleMscal(uint32_t startPC, const uint8_t *vuCode, uint32_t codeSize, GS &gs, PS2Memory *memory) {
        if (!vuCode || codeSize < 0xE00u) {
            return false;
        }

        uint64_t hash = 0xCBF29CE484222325ULL;
        for (uint32_t i = 0; i < 0xE00u; ++i) {
            hash ^= vuCode[i];
            hash *= 0x100000001B3ULL;
        }

        bool match = (hash == 0xF72A6639E95D7AC8ULL);

        static std::mutex s_sigMutex;
        static std::vector<uint64_t> s_loggedSignatures;
        {
            std::lock_guard<std::mutex> lock(s_sigMutex);
            if (std::find(s_loggedSignatures.begin(), s_loggedSignatures.end(), hash) == s_loggedSignatures.end()) {
                s_loggedSignatures.push_back(hash);
                std::printf("[hle] sig=0x%016llX match=%d\n", (unsigned long long)hash, match ? 1 : 0);
                std::fflush(stdout);
            }
            s_sigCountsThisSec[hash]++;
        }

        if (startPC != 0x048) {
            s_movieKickMscalIndex.store(0, std::memory_order_relaxed);
            s_kickDecision = KickDecision::Undecided;
            return false;
        }

        if (isMovieHleVerify()) {
            return false;
        }

        if (!isMovieHleActive()) {
            return false;
        }

        uint32_t curIdx = s_movieKickMscalIndex.fetch_add(1, std::memory_order_relaxed);
        if (curIdx == 0) {
            const uint8_t *rdram = memory ? memory->getRDRAM() : s_cachedRdram;
            const bool movieActive = isMovieActive(rdram);
            uint32_t base = s_movieKickFirstUnpackSrc.load(std::memory_order_relaxed);

            const char *failReason = nullptr;
            if (!movieActive) {
                failReason = "movie_not_active";
            } else if (base == 0) {
                failReason = "no_y_unpack";
            } else if (base + 768u * 384u > PS2_RAM_SIZE) {
                failReason = "base_out_of_ram";
            } else if (isMovieHleStrict() && !match) {
                failReason = "strict_sig_mismatch";
            }

            if (failReason != nullptr) {
                s_kickDecision = KickDecision::Interpret;
                s_secInterpretedKicks++;
                if (movieActive) {
                    if (s_reportedInterpretReasons.insert(failReason).second) {
                        uint32_t st = 0;
                        if (rdram) std::memcpy(&st, rdram + (0x772188u & PS2_RAM_MASK), 4);
                        std::printf("[probe] bg kick interpreted reason=%s base=0x%08X st=%u sig=0x%016llX\n",
                                    failReason, base, st, (unsigned long long)hash);
                        std::fflush(stdout);
                    }
                }
            } else {
                s_kickDecision = KickDecision::Native;
                s_secNativeKicks++;

                double native_ms = 0.0, xfer_ms = 0.0;
                doMovieFrameTransfer(gs, memory, base, native_ms, xfer_ms);
                s_secNativeMsTotal += native_ms;
                s_secXferMsTotal += xfer_ms;
                onBhMovieHleTime(static_cast<uint64_t>((native_ms + xfer_ms) * 1000.0));
            }

            // Per-second probe dump
            auto now = std::chrono::steady_clock::now();
            double secDiff = std::chrono::duration<double>(now - s_bgProbeLastSec).count();
            if (secDiff >= 1.0) {
                double avg_native = s_secNativeKicks > 0 ? (s_secNativeMsTotal / s_secNativeKicks) : 0.0;
                double avg_xfer = s_secNativeKicks > 0 ? (s_secXferMsTotal / s_secNativeKicks) : 0.0;

                std::string sigStr;
                for (const auto &pair : s_sigCountsThisSec) {
                    if (!sigStr.empty()) sigStr += ",";
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "0x%llX:%u", (unsigned long long)pair.first, pair.second);
                    sigStr += buf;
                }
                if (sigStr.empty()) sigStr = "none";

                std::printf("[probe] bg native_kicks=%u interpreted_kicks=%u mixed=%u sig_counts=<%s> native_ms=%.2f xfer_ms=%.2f\n",
                            s_secNativeKicks, s_secInterpretedKicks, s_secMixedKicks,
                            sigStr.c_str(), avg_native, avg_xfer);
                std::fflush(stdout);

                s_secNativeKicks = 0;
                s_secInterpretedKicks = 0;
                s_secMixedKicks = 0;
                s_secNativeMsTotal = 0.0;
                s_secXferMsTotal = 0.0;
                s_sigCountsThisSec.clear();
                s_bgProbeLastSec = now;
            }

            return (s_kickDecision == KickDecision::Native);
        }

        // LATER MSCALs of the same kick (1..767)
        if (s_kickDecision == KickDecision::Native) {
            return true; // Skip
        } else {
            return false; // Interpret
        }
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

    static std::atomic<bool> s_bhStartupPrinted{false};
    void bhInitStartup() {
        if (!isEnabled()) return;
        bool expected = false;
        if (s_bhStartupPrinted.compare_exchange_strong(expected, true)) {
            for (const auto &site : s_sleepSites) {
                ps2xSafeLog("[probe] bh sleepsite %s ms=%s\n", site.desc, site.durationConst);
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
        if (!isEnabled()) return;
        if (tl_vif1Depth == 0) {
            tl_vif1Start = std::chrono::steady_clock::now();
            tl_vif1GifUs = 0;
        }
        tl_vif1Depth++;
    }

    void onBhVif1End() {
        if (!isEnabled()) return;
        tl_vif1Depth--;
        if (tl_vif1Depth == 0) {
            auto now = std::chrono::steady_clock::now();
            uint64_t totalUs = std::chrono::duration_cast<std::chrono::microseconds>(now - tl_vif1Start).count();
            uint64_t selfUs = (totalUs > tl_vif1GifUs) ? (totalUs - tl_vif1GifUs) : 0;
            s_bhVif1Us.fetch_add(selfUs, std::memory_order_relaxed);
        }
    }

    void onBhSubmitGifStart() {
        if (!isEnabled()) return;
        if (tl_submitGifDepth == 0) {
            tl_submitGifStart = std::chrono::steady_clock::now();
        }
        tl_submitGifDepth++;
    }

    void onBhSubmitGifEnd() {
        if (!isEnabled()) return;
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
        if (!isEnabled()) return;
        s_bhGsBlockUs.fetch_add(waitUs, std::memory_order_relaxed);
        s_bhGsBlockCnt.fetch_add(1, std::memory_order_relaxed);
    }

    void onBhRaw8FeedTime(uint64_t us) {
        if (!isEnabled()) return;
        s_bhRaw8FeedUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhRaw8DecTime(uint64_t us) {
        if (!isEnabled()) return;
        s_bhRaw8DecUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhRaw8YccTime(uint64_t us) {
        if (!isEnabled()) return;
        s_bhRaw8YccUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhRaw8Call() {
        if (!isEnabled()) return;
        s_bhRaw8Calls.fetch_add(1, std::memory_order_relaxed);
        onBhIteration();
    }

    void onBhMovieHleTime(uint64_t us) {
        if (!isEnabled()) return;
        s_bhMovieHleUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhSchedWait(uint64_t waitUs) {
        if (!isEnabled()) return;
        s_bhSchedWaitUs.fetch_add(waitUs, std::memory_order_relaxed);
        s_bhSchedWaitCnt.fetch_add(1, std::memory_order_relaxed);
    }

    void onBhDmaPollTime(uint64_t us) {
        if (!isEnabled()) return;
        s_bhDmaPollUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhCid1Time(uint64_t us) {
        if (!isEnabled()) return;
        s_bhCid1Us.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhIntcHandler(uint32_t cause, uint64_t execUs) {
        if (!isEnabled()) return;
        s_bhIntcUs.fetch_add(execUs, std::memory_order_relaxed);
        s_bhIntcCnt.fetch_add(1, std::memory_order_relaxed);
        if (cause < 32) {
            s_bhIntcCauseUs[cause].fetch_add(execUs, std::memory_order_relaxed);
            s_bhIntcCauseCnt[cause].fetch_add(1, std::memory_order_relaxed);
        }
    }

    void onBhGsPacket(uint64_t packetUs, uint32_t primType, uint32_t pixelCount, bool texOn, bool blendOn, uint32_t tw, uint32_t th) {
        if (!isEnabled()) return;
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
        if (!isEnabled()) return;
        s_bhGsBusyUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhRecordQueueDepth(uint32_t depth) {
        if (!isEnabled()) return;
        uint32_t curMax = s_bhQueueDepthMax.load(std::memory_order_relaxed);
        while (depth > curMax && !s_bhQueueDepthMax.compare_exchange_weak(curMax, depth, std::memory_order_relaxed)) {}
        s_bhQueueDepthSum.fetch_add(depth, std::memory_order_relaxed);
        s_bhQueueDepthSamples.fetch_add(1, std::memory_order_relaxed);
    }

    void onBhSleepSiteHit(int siteId, uint64_t us) {
        if (!isEnabled() || siteId < 0 || siteId >= 3) return;
        bhInitStartup();
        s_sleepSites[siteId].hits.fetch_add(1, std::memory_order_relaxed);
        s_sleepSites[siteId].totalUs.fetch_add(us, std::memory_order_relaxed);
    }

    void onBhIteration() {
        if (!isEnabled()) return;
        s_bhIters.fetch_add(1, std::memory_order_relaxed);
    }

    static auto s_bhStartTime = std::chrono::steady_clock::now();

    void bhTickCheckPerSec(const uint8_t *rdram) {
        if (!isEnabled()) return;
        if (rdram) setRdram(rdram);
        bhInitStartup();

        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - s_bhLastSec).count();
        if (elapsedSec < 1.0) return;

        bool movieActive = isMovieActive(rdram ? rdram : s_cachedRdram);
        uint32_t t = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now - s_bhStartTime).count());
        ps2xSafeLog("[probe] bh alive movie=%d t=%u\n", movieActive ? 1 : 0, t);

        if (!movieActive) {
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
        ps2xSafeLog("[probe] bh ee vif1=%.1f gif=%.1f gs_block=%.1f(%u) raw8=%.1f(feed=%.1f,dec=%.1f,ycc=%.1f,calls=%u) hle=%.1f sched_wait=%.1f(%u) dma_poll=%.1f cid1=%.1f intc=%.1f(%u)[%s] other=%.1f\n",
                    vif1Ms, gifMs, gsBlockMs, gsBlockCnt, raw8TotalMs, raw8FeedMs, raw8DecMs, raw8YccMs, raw8Calls,
                    hleMs, schedWaitMs, schedWaitCnt, dmaPollMs, cid1Ms, intcMs, intcCnt, intcCausesStr.c_str(), otherMs);

        ps2xSafeLog("[probe] bh gs busy=%.1f idle=%.1f q_max=%u q_avg=%.1f px/s=%llu prims/s=%llu slow_pkts=<%s>\n",
                    gsBusyMs, gsIdleMs, qMax, qAvg, (unsigned long long)pixelsSec, (unsigned long long)primsSec, slowPktsStr.c_str());

        ps2xSafeLog("[probe] bh iter iters/s=%u avg_ms=%.1f ring_head=%u ring_tail=%u\n",
                    iters, avgIterMs, ringHead, ringTail);

        ps2xSafeLog("[probe] bh sleep hits: %s\n", sleepHitsStr.c_str());

        s_bhLastSec = now;
    }
}
