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
};

// Hash (FNV-1a 64) of the microcode image these executors were generated from.
extern const uint64_t kVu1NativeMainHash;

// Runs native pairs starting at vu.m_state.pc until a pair without a native executor is reached.
// Returns 1 when the interpreter loop must stop (budget reached / stop requested / program ended),
// 2 when the interpreter has to execute the current pair itself.
int vu1NativeRun(VU1Interpreter &vu, VuNativeCtx &ctx);
