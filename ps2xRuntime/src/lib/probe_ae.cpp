#include "probe_ae.h"
#include <mutex>
#include <map>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstdio>

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

    std::atomic<uint32_t> reg_F_a0{0};
    std::atomic<uint32_t> reg_F_a1{0};
    std::atomic<uint32_t> reg_F_a2{0};
    std::atomic<uint32_t> reg_F_s0{0};
    std::atomic<uint32_t> reg_F_s1{0};
    std::atomic<uint32_t> reg_G_a0{0};
    std::atomic<uint32_t> reg_G_a1{0};
    std::atomic<uint32_t> cnt_G_calls{0};

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
            std::memcpy(&st_word, rdram + (0x772188u & PS2_RAM_MASK), sizeof(st_word));
        }

        std::string raList = getAndResetRa24c64c();
        std::string exitList = getAndResetExits();

        std::string cd04Cmds = getAndResetCd04Cmds();
        std::string a1_1dc = getAndReset1dcA1();
        uint32_t c_24cd04 = cnt_24cd04.exchange(0, std::memory_order_relaxed);
        uint32_t c_oob = cnt_cd34_oob.exchange(0, std::memory_order_relaxed);
        uint32_t neg_v0 = cnt_3f7a74_v0_neg.exchange(0, std::memory_order_relaxed);

        flushFirstCallsIfDue(sec);

        // Print unified [probe] line with old fields + Task AF + Task AG fields
        std::printf("[probe] t=%u F=%u state=%u st=%u s0=%X a1=%X v0_1DA880=%u(tk=%u) f72C7A0=%u f72D3A0=%u w635480=%u s4C=%X s2=%X s4C10=%X s0_70=%X s484=%X tk558=%u v0_508684=%X w72D44C=%X s1_0C=%X t7=%X c24C24C=%u c24C4F8=%u c24C620=%u c24C64C=%u c24C650=%u c24C680=%u c24CE0C=%u v24CBFC=%u(tk=%u) c24CBFC=%u a0_cbfc=%X cbfc10=%X cbfc0C=%X a1DA880=%u,%u c24CE24=%u c24CB38=%u a0_cb38=%X cb38_8=%u b7C=%u c24CB64=%u v3F75E8=%u c24CBB8=%u t6_cbb8=%u s6_7C=%u s6_30=%X ra=[%s] exits=%s c24CD04=%u cmds=%s last_a0=%X last_a2=%X last_a3=%X last_ra=%X t7_cd50=%X oob_cmd=%u v0_3f7=%d neg_v0=%u s1_3f7=%X head_3f7=%u tail_3f7=%u slot8_1dc=%X ret_1dc578=%d a1_1dc=%s\n",
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
                    val_1dc650_slot8.load(), val_1dc578_ret.load(), a1_1dc.c_str());
        std::fflush(stdout);
    }
}
