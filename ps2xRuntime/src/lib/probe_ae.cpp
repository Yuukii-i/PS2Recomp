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
            std::memcpy(&st_word, rdram + (0x772188u & PS2_RAM_MASK), sizeof(st_word));
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
        std::printf("[probe] t=%u F=%u state=%u st=%u s0=%X a1=%X v0_1DA880=%u(tk=%u) f72C7A0=%u f72D3A0=%u w635480=%u s4C=%X s2=%X s4C10=%X s0_70=%X s484=%X tk558=%u v0_508684=%X w72D44C=%X s1_0C=%X t7=%X c24C24C=%u c24C4F8=%u c24C620=%u c24C64C=%u c24C650=%u c24C680=%u c24CE0C=%u v24CBFC=%u(tk=%u) c24CBFC=%u a0_cbfc=%X cbfc10=%X cbfc0C=%X a1DA880=%u,%u c24CE24=%u c24CB38=%u a0_cb38=%X cb38_8=%u b7C=%u c24CB64=%u v3F75E8=%u c24CBB8=%u t6_cbb8=%u s6_7C=%u s6_30=%X ra=[%s] exits=%s c24CD04=%u cmds=%s last_a0=%X last_a2=%X last_a3=%X last_ra=%X t7_cd50=%X oob_cmd=%u v0_3f7=%d neg_v0=%u s1_3f7=%X head_3f7=%u tail_3f7=%u slot8_1dc=%X ret_1dc578=%d a1_1dc=%s c438EA8=%u c1DCEE0=%u t6_1dcf8c=%X s1_1dcf8c=%X w771bd4=%X w76a518=%X pkt=%X,%X,%X,%X w77218c=%X w771bc4=%X c1DCC70=%u v0_1dcc=%d t7_1dc1ec=%X s1_1dc1ec=%X c_intc=%u c24C4C0=%u v0_c4e0=%u s0_84=%X lt_c4e0=%u c24C4F0=%u a0_3f7780=%X a1_3f7780=%X mask_3f7780=%llX c3FBE78=%u v_3fbe78=%u v0_3fbe=%X w74B140=%X,%X,%X m1900EA0=%llX s0_84_mem=%X s0_7C_mem=%X c24C5A0=%u c24C5AC=%u c3F7E38=%u c31A240=%u c31A2C0=%u s4C_refill=%X\n",
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
                    c_24c5a0, c_24c5ac, c_3f7e38, c_31a240, c_31a2c0, s4c_refill);
        std::fflush(stdout);
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
}
