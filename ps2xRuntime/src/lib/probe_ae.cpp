#include "probe_ae.h"
#include <mutex>
#include <map>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstdio>

namespace probe_ae {
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

    void printOncePerSec(uint32_t sec) {
        if (!isEnabled()) return;
        uint32_t f_cnt = cnt_F.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c24c = cnt_24c24c.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c4f8 = cnt_24c4f8.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c620 = cnt_24c620.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c64c = cnt_24c64c.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c650 = cnt_24c650.exchange(0, std::memory_order_relaxed);
        uint32_t c_24c680 = cnt_24c680.exchange(0, std::memory_order_relaxed);
        std::string raList = getAndResetRa24c64c();
        std::string exitList = getAndResetExits();

        // Print ONE [probe] t=N line matching task specification
        std::printf("[probe] t=%u F=%u state=%u s0=%X a1=%X v0_1DA880=%u(tk=%u) f72C7A0=%u f72D3A0=%u w635480=%u s4C=%X s2=%X s4C10=%X s0_70=%X s484=%X tk558=%u v0_508684=%X w72D44C=%X s1_0C=%X t7=%X c24C24C=%u c24C4F8=%u c24C620=%u c24C64C=%u c24C650=%u c24C680=%u ra=[%s] exits=%s\n",
                    sec, f_cnt, val_state.load(), val_s0.load(), val_a1_s4.load(),
                    val_v0_1DA880.load(), val_v0_taken.load(),
                    val_f72C7A0.load(), val_f72D3A0.load(), val_w635480.load(),
                    val_s4C.load(), val_s2.load(), val_s4C_10.load(), val_s0_70.load(), val_s48_4.load(), val_taken_558.load(),
                    val_v0_508684.load(), val_w72D44C.load(), val_s1_0C.load(), val_t7_jalr.load(),
                    c_24c24c, c_24c4f8, c_24c620, c_24c64c, c_24c650, c_24c680,
                    raList.c_str(), exitList.empty() ? "none" : exitList.c_str());
        std::fflush(stdout);
    }
}
