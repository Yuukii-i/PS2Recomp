// vu1_native_api.h -- interface between the VU1 interpreter (ps2_vu1_core.cpp) and the generated native pair executors
// (generated/vu1_native_exec_*.cpp, tools/vu1gen/gen_vu1_native.py). No private access needed here.
#pragma once

#include <cstdint>

class VU1Interpreter;
class GS;
class PS2Memory;

// Operations that still live inside the interpreter (they need private state / file-static helpers).
struct VuNativeOps
{
    void (*commit)(VU1Interpreter &);
    void (*advanceTo)(VU1Interpreter &, uint64_t);
    void (*advance1)(VU1Interpreter &);
    void (*execUpper)(VU1Interpreter &, uint32_t);
    void (*execLower)(VU1Interpreter &, uint32_t lower, uint8_t *vuData, uint32_t dataSize, GS &, PS2Memory *, uint32_t upper);
    void (*qVf)(VU1Interpreter &, uint8_t reg, uint8_t lanes, const float *value, uint32_t latency);
    void (*qVi)(VU1Interpreter &, uint8_t reg, int32_t value, uint32_t latency);
    void (*qAcc)(VU1Interpreter &, uint8_t lanes, const float *value, uint32_t latency);
    void (*xgProgress)(VU1Interpreter &); // PERF-IC: progressXgkick (called only while a transfer is active)
    uint64_t (*blkHash)(VU1Interpreter &); // PERF-IE: timing-state hash (identical to the PROBE-BLK entry/exit hash)
    void (*qStore)(VU1Interpreter &, uint32_t address, const uint32_t *words, uint8_t laneMask); // PERF-IE: queueStore
};

struct VuNativeCtx
{
    const VuNativeOps *ops;
    uint64_t budgetEnd;
    uint32_t instrs;       // pairs executed natively since the last reset (added to the interpreter's counter)
    bool programEnded;     // set when the program's E/D/T-bit end was reached
    uint8_t *vuData;
    uint32_t dataSize;
    GS *gs;
    PS2Memory *memory;
    uint32_t codeSize;
    const uint64_t *nextReady; // PERF-IC: the interpreter's next-ready-cycle cache (commit early-out)
    uint64_t nextHash;         // PERF-IE: exit-state hash of the last hot block (valid while hashValid)
    bool hashValid;
    bool check;                // PS2X_VU_BLK_CHECK=1: compare each hot block's constant exit hash with the real state
    uint64_t hotBlocks, hotPairs, checkFail;
    uint32_t *storeMask;       // PERF-IF: pending-store slot mask of this unit (inline queueStore in the hot blocks)
    uint64_t *nextReadyW;      // PERF-IF: writable next-ready cache
};

// PERF-IE: generated hot blocks (generated/vu1_hot_blocks.cpp)
using VuHotFn = int (*)(VU1Interpreter &, VuNativeCtx &);
VuHotFn vu1HotLookup(uint32_t pc, uint64_t entryHash);
extern const bool kVu1HotStart[2048];

// Hash (FNV-1a 64) of the microcode image these executors were generated from.
extern const uint64_t kVu1NativeMainHash;

// Runs native pairs starting at vu.m_state.pc until a pair without a native executor is reached.
// Returns 1 when the interpreter loop must stop (budget reached / stop requested / program ended),
// 2 when the interpreter has to execute the current pair itself.
int vu1NativeRun(VU1Interpreter &vu, VuNativeCtx &ctx);
