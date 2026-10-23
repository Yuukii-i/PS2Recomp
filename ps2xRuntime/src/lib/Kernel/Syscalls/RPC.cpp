#include "Common.h"
#include "RPC.h"
#include "../Stubs/SIF.h"
#include "../Stubs/Pad.h"
#include "../Stubs/VfiArchive.h"
#include "../../ps2_iop_transport.h"

void ps2xSafeLog(const char *fmt, ...);
extern "C" bool IsGamepadAvailable(int gamepad);
extern std::atomic<uint32_t> g_hostPadButtons;
extern std::atomic<uint32_t> g_trackedCurVtable;
extern std::atomic<bool> g_movieSeekDone;
extern std::atomic<uint32_t> g_movieReadsCount;

#include <atomic>
#include <mutex>
#include <cstring>
#include <string>
#include <set>
#include <tuple>
#include <chrono>

struct DbcProfKey {
    uint32_t sid;
    uint32_t fno;
    bool operator==(const DbcProfKey &o) const { return sid == o.sid && fno == o.fno; }
};
struct DbcProfKeyHash {
    size_t operator()(const DbcProfKey &k) const { return (static_cast<size_t>(k.sid) * 31) ^ static_cast<size_t>(k.fno); }
};
static std::mutex s_dbcProfMutex;
static std::unordered_map<DbcProfKey, uint32_t, DbcProfKeyHash> s_dbcProfCounts;
static void recordDbcCall(uint32_t sid, uint32_t fno) {
    std::lock_guard<std::mutex> lock(s_dbcProfMutex);
    s_dbcProfCounts[{sid, fno}]++;
}
static std::atomic<uint32_t> s_padUpdateCalledCount{0};
static auto s_lastPadSecLog = std::chrono::steady_clock::now();

struct Sg2PacketKey {
    uint32_t op, a1, a2, a3;
    bool operator<(const Sg2PacketKey &o) const {
        return std::tie(op, a1, a2, a3) < std::tie(o.op, o.a1, o.a2, o.a3);
    }
};
static std::mutex s_sg2ProfMutex;
static std::unordered_map<uint32_t, uint32_t> s_sg2OpCounts;
static std::set<Sg2PacketKey> s_sg2DistinctPackets;
static auto s_lastSg2SecLog = std::chrono::steady_clock::now();

std::atomic<uint32_t> g_diagSg2Fno0Calls{0};
std::atomic<uint32_t> g_diagDbcBufferWrites{0};
std::atomic<uint32_t> g_diagCid1Requests{0};
std::atomic<uint64_t> g_diagCid1BytesServed{0};
static std::mutex s_diagCid1PathMutex;
static char s_diagLastCid1Path[256] = "";

void diagGetLastCid1Path(char *out, size_t maxLen)
{
    if (!out || maxLen == 0) return;
    std::lock_guard<std::mutex> lock(s_diagCid1PathMutex);
    std::strncpy(out, s_diagLastCid1Path, maxLen - 1);
    out[maxLen - 1] = '\0';
}

void diagSetLastCid1Path(const std::string &path)
{
    std::lock_guard<std::mutex> lock(s_diagCid1PathMutex);
    std::strncpy(s_diagLastCid1Path, path.c_str(), sizeof(s_diagLastCid1Path) - 1);
    s_diagLastCid1Path[sizeof(s_diagLastCid1Path) - 1] = '\0';
}

namespace ps2_syscalls
{
    namespace
    {
        static inline bool isVerboseLog()
        {
            static int s_verbose = -1;
            if (s_verbose == -1)
            {
                const char *v = std::getenv("PS2X_VERBOSE_LOG");
                s_verbose = (v && (v[0] == '1' || v[0] == 't')) ? 1 : 0;
            }
            return s_verbose == 1;
        }
        SifRpcDebugEvent makeRpcDebugEvent(const char *op, R5900Context *ctx, PS2Runtime *runtime)
        {
            SifRpcDebugEvent event{};
            event.op = op;
            event.pc = ctx ? ctx->pc : 0u;
            event.ra = ctx ? getRegU32(ctx, 31) : 0u;
            event.threadId = runtime ? static_cast<uint32_t>(runtime->eeScheduler().currentThreadId()) : 0u;
            return event;
        }

        void pushSifRpcDebugEventLocked(SifRpcDebugEvent event)
        {
            event.seq = ++g_sif_rpc_debug_next_seq;
            g_sif_rpc_debug_history[event.seq % kSifRpcDebugHistoryCount] = event;
        }

        void pushSifRpcDebugEvent(SifRpcDebugEvent event)
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            pushSifRpcDebugEventLocked(event);
        }

        struct RpcHleCallContext
        {
            uint8_t *rdram = nullptr;
            R5900Context *ctx = nullptr;
            PS2Runtime *runtime = nullptr;
            uint32_t clientPtr = 0;
            uint32_t sid = 0;
            uint32_t rpcNum = 0;
            uint32_t mode = 0;
            uint32_t sendBuf = 0;
            uint32_t sendSize = 0;
            uint32_t recvBuf = 0;
            uint32_t recvSize = 0;
            uint32_t endFunc = 0;
            uint32_t endParam = 0;
        };

        using RpcHleCallHandler = std::function<int32_t(const RpcHleCallContext &)>;
        using RpcHleBindHandler = std::function<void(uint32_t sid, uint32_t clientPtr)>;

        struct RpcHleService
        {
            uint32_t sid = 0;
            std::string name;
            RpcHleBindHandler onBind;
            RpcHleCallHandler onCall;
        };

        static std::unordered_map<uint32_t, RpcHleService> s_rpcHleServices;
        static uint32_t s_iopSregs[32] = {};

        static uint32_t s_dbcSeq = 1u;
        static uint32_t s_dbcPort0BufA = 0x00784D40u;
        static uint32_t s_dbcPort0BufB = 0x00784DC0u;
        static uint32_t s_dbcPort1BufA = 0x00784E80u;
        static uint32_t s_dbcPort1BufB = 0x00784F00u;
        static uint32_t s_dbcWorkAddr = 0x00757D40u;

        static void dbcWritePacket(uint8_t *rdram, uint32_t addr, const ps2_stubs::PadInputState &pad, uint32_t seq)
        {
            if (!rdram || addr == 0 || addr >= 0x02000000u) return;
            uint8_t *dst = getMemPtr(rdram, addr);
            if (!dst) return;

            dst[0x00] = 0x01; // status: DBC_STAT_CONNECTED
            dst[0x01] = 0x00;
            dst[0x02] = 0x20; // packet len: 32 bytes
            dst[0x03] = 0x00;
            dst[0x04] = 0x01; // valid / connected word

            std::memset(dst + 0x1C, 0, 32);
            dst[0x1C + 0x00] = 0x00; // terminal/status
            dst[0x1C + 0x01] = 0x73; // pad type: DS2
            dst[0x1C + 0x02] = static_cast<uint8_t>(pad.buttons & 0xFFu);
            dst[0x1C + 0x03] = static_cast<uint8_t>((pad.buttons >> 8u) & 0xFFu);
            dst[0x1C + 0x04] = pad.rx;
            dst[0x1C + 0x05] = pad.ry;
            dst[0x1C + 0x06] = pad.lx;
            dst[0x1C + 0x07] = pad.ly;

            // Pressure values: 0xFF if pressed, 0x00 if not:
            dst[0x1C + 8]  = (pad.buttons & (1 << 5)) ? 0x00 : 0xFF; // Right
            dst[0x1C + 9]  = (pad.buttons & (1 << 7)) ? 0x00 : 0xFF; // Left
            dst[0x1C + 10] = (pad.buttons & (1 << 4)) ? 0x00 : 0xFF; // Up
            dst[0x1C + 11] = (pad.buttons & (1 << 6)) ? 0x00 : 0xFF; // Down
            dst[0x1C + 12] = (pad.buttons & (1 << 12)) ? 0x00 : 0xFF; // Triangle
            dst[0x1C + 13] = (pad.buttons & (1 << 13)) ? 0x00 : 0xFF; // Circle
            dst[0x1C + 14] = (pad.buttons & (1 << 14)) ? 0x00 : 0xFF; // Cross
            dst[0x1C + 15] = (pad.buttons & (1 << 15)) ? 0x00 : 0xFF; // Square
            dst[0x1C + 16] = (pad.buttons & (1 << 10)) ? 0x00 : 0xFF; // L1
            dst[0x1C + 17] = (pad.buttons & (1 << 8)) ? 0x00 : 0xFF; // L2
            dst[0x1C + 18] = (pad.buttons & (1 << 11)) ? 0x00 : 0xFF; // R1
            dst[0x1C + 19] = (pad.buttons & (1 << 9)) ? 0x00 : 0xFF; // R2

            // sequence counter at +0x7C:
            std::memcpy(dst + 0x7C, &seq, sizeof(seq));
        }

        static void dbcUpdatePadBuffers(uint8_t *rdram, PS2Runtime *runtime)
        {
            if (!rdram) return;
            ps2_stubs::PadInputState pad0 = ps2_stubs::getCurrentPadInput(0, runtime);

            s_dbcSeq++;
            uint32_t activeBuf0 = (s_dbcSeq & 1) ? s_dbcPort0BufB : s_dbcPort0BufA;
            uint32_t olderBuf0  = (s_dbcSeq & 1) ? s_dbcPort0BufA : s_dbcPort0BufB;

            if (olderBuf0)  dbcWritePacket(rdram, olderBuf0, pad0, s_dbcSeq - 1);
            if (activeBuf0) dbcWritePacket(rdram, activeBuf0, pad0, s_dbcSeq);

            if (s_dbcPort1BufA && s_dbcPort1BufB) {
                ps2_stubs::PadInputState pad1 = ps2_stubs::getCurrentPadInput(1, runtime);
                uint32_t activeBuf1 = (s_dbcSeq & 1) ? s_dbcPort1BufB : s_dbcPort1BufA;
                uint32_t olderBuf1  = (s_dbcSeq & 1) ? s_dbcPort1BufA : s_dbcPort1BufB;
                dbcWritePacket(rdram, olderBuf1, pad1, s_dbcSeq - 1);
                dbcWritePacket(rdram, activeBuf1, pad1, s_dbcSeq);
            }

            if (s_dbcWorkAddr) {
                if (uint8_t *workDst = getMemPtr(rdram, s_dbcWorkAddr)) {
                    uint32_t status0 = 1; // Port 0 connected
                    uint32_t status1 = 1; // Port 1 connected
                    std::memcpy(workDst, &status0, 4);
                    std::memcpy(workDst + 4, &status1, 4);
                }
            }
            g_diagDbcBufferWrites.fetch_add(1, std::memory_order_relaxed);

            s_padUpdateCalledCount.fetch_add(1, std::memory_order_relaxed);
            auto padNow = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(padNow - s_lastPadSecLog).count() >= 1000)
            {
                s_lastPadSecLog = padNow;
                {
                    std::lock_guard<std::mutex> lock(s_dbcProfMutex);
                    for (const auto &pair : s_dbcProfCounts)
                    {
                        if (pair.second > 0)
                        {
                            ps2xSafeLog("[dbc] sid=0x%08x fno=0x%04x n=%u\n", pair.first.sid, pair.first.fno, pair.second);
                        }
                    }
                    s_dbcProfCounts.clear();
                }
                uint32_t upd = s_padUpdateCalledCount.exchange(0, std::memory_order_relaxed);
                if (upd > 0)
                {
                    bool kd = (::g_hostPadButtons.load(std::memory_order_relaxed) != 0) || (pad0.buttons != 0xFFFFu);
                    bool gp = IsGamepadAvailable(0);
                    ps2xSafeLog("[pad] updateCalled=%u keyDown=%s gamepad=%s\n", upd, kd ? "true" : "false", gp ? "true" : "false");
                }
            }
        }

        static void initRpcHleServices()
        {
            static bool s_initialized = false;
            if (s_initialized) return;
            s_initialized = true;

            // 0x80001300: DBC Base service
            s_rpcHleServices[0x80001300u] = {
                0x80001300u,
                "DBC_BASE",
                [](uint32_t sid, uint32_t clientPtr) {
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:dbc] Bound DBC_BASE (0x%08x) client=0x%08x\n", sid, clientPtr);
                        fflush(stderr);
                    }
                },
                [](const RpcHleCallContext &call) -> int32_t {
                    recordDbcCall(call.sid, call.rpcNum);
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:dbc] Call DBC_BASE fno=0x%x sendBuf=0x%08x (%u) recvBuf=0x%08x (%u)\n",
                                call.rpcNum, call.sendBuf, call.sendSize, call.recvBuf, call.recvSize);
                        fflush(stderr);
                    }
                    if (call.rpcNum == 0x80001363u) // sceDbcGetModVersion
                    {
                        if (call.recvBuf && call.recvSize >= 4)
                        {
                            uint32_t version = 0x0310u; // (0x310 >> 4) == 0x31
                            if (uint8_t *dst = getMemPtr(call.rdram, call.recvBuf))
                            {
                                std::memcpy(dst, &version, 4);
                            }
                        }
                        return 0;
                    }
                    if (call.rpcNum == 0x80001304u) // sceDbcSetWorkAddr
                    {
                        uint32_t workAddr = 0;
                        if (call.sendBuf && call.sendSize >= 8)
                        {
                            if (const uint8_t *src = getConstMemPtr(call.rdram, call.sendBuf))
                            {
                                std::memcpy(&workAddr, src + 4, 4);
                            }
                        }
                        ps2xSafeLog( "[rpc:dbc] sceDbcSetWorkAddr workAddr=0x%08x\n", workAddr);
                        fflush(stderr);

                        if (workAddr != 0) {
                            s_dbcWorkAddr = workAddr;
                        }
                        dbcUpdatePadBuffers(call.rdram, call.runtime);
                        return 0;
                    }
                    if (call.rpcNum == 0x80001301u) // DBC port double-buffer registration
                    {
                        if (call.sendBuf && call.sendSize >= 0x30)
                        {
                            if (const uint8_t *src = getConstMemPtr(call.rdram, call.sendBuf))
                            {
                                uint32_t port = 0;
                                std::memcpy(&port, src + 8, 4);
                                uint32_t bufA = 0, bufB = 0;
                                std::memcpy(&bufA, src + 0x28, 4);
                                std::memcpy(&bufB, src + 0x2C, 4);
                                if (port == 0)
                                {
                                    if (bufA) s_dbcPort0BufA = bufA;
                                    if (bufB) s_dbcPort0BufB = bufB;
                                }
                                else
                                {
                                    if (bufA) s_dbcPort1BufA = bufA;
                                    if (bufB) s_dbcPort1BufB = bufB;
                                }
                                if (call.recvBuf && call.recvSize >= 0x28)
                                {
                                    if (uint8_t *dst = getMemPtr(call.rdram, call.recvBuf))
                                    {
                                        uint32_t one = 1;
                                        std::memcpy(dst, &one, 4);
                                        std::memcpy(dst + 0x24, &port, 4);
                                    }
                                }
                                ps2xSafeLog("[rpc:dbc] Registered port %u buffers: A=0x%08x B=0x%08x\n", port, bufA, bufB);
                                fflush(stderr);
                            }
                        }
                        dbcUpdatePadBuffers(call.rdram, call.runtime);
                        return 0;
                    }
                    if (call.rpcNum == 0x80001303u) // DBC check/poll port connection
                    {
                        if (call.recvBuf && call.recvSize >= 8)
                        {
                            if (uint8_t *dst = getMemPtr(call.rdram, call.recvBuf))
                            {
                                uint32_t status = 1; // 1 = connected
                                std::memcpy(dst + 4, &status, 4);
                            }
                        }
                        return 0;
                    }
                    if (call.rpcNum == 0x8000131au) // DBC get pad mode / capabilities
                    {
                        if (call.recvBuf && call.recvSize >= 0x90)
                        {
                            if (uint8_t *dst = getMemPtr(call.rdram, call.recvBuf))
                            {
                                uint32_t retLen = 1;
                                uint32_t retCode = 0;
                                std::memcpy(dst + 8, &retLen, 4);
                                dst[0xC] = 0x07; // DualShock 2 analog controller mode
                                std::memcpy(dst + 0x8C, &retCode, 4);
                            }
                        }
                        return 0;
                    }
                    return 0;
                }
            };

            // 0x8000131Cu: DBC Pad Service
            s_rpcHleServices[0x8000131Cu] = {
                0x8000131Cu,
                "DBC_PAD",
                [](uint32_t sid, uint32_t clientPtr) {
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:dbc] Bound DBC_PAD (0x%08x) client=0x%08x\n", sid, clientPtr);
                        fflush(stderr);
                    }
                },
                [](const RpcHleCallContext &call) -> int32_t {
                    recordDbcCall(call.sid, call.rpcNum);
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:dbc] Call DBC_PAD fno=0x%x sendBuf=0x%08x (%u) recvBuf=0x%08x (%u)\n",
                                call.rpcNum, call.sendBuf, call.sendSize, call.recvBuf, call.recvSize);
                        fflush(stderr);
                    }
                    if (call.recvBuf && call.recvSize > 0)
                    {
                        rpcZeroRdram(call.rdram, call.recvBuf, call.recvSize);
                    }
                    return 0;
                }
            };

            // 0x8000131Eu: DBC Port 0
            s_rpcHleServices[0x8000131Eu] = {
                0x8000131Eu,
                "DBC_PORT0",
                [](uint32_t sid, uint32_t clientPtr) {
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:dbc] Bound DBC_PORT0 (0x%08x) client=0x%08x\n", sid, clientPtr);
                        fflush(stderr);
                    }
                },
                [](const RpcHleCallContext &call) -> int32_t {
                    recordDbcCall(call.sid, call.rpcNum);
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:dbc] Call DBC_PORT0 fno=0x%x sendBuf=0x%08x (%u) recvBuf=0x%08x (%u)\n",
                                call.rpcNum, call.sendBuf, call.sendSize, call.recvBuf, call.recvSize);
                        fflush(stderr);
                    }
                    if (call.recvBuf && call.recvSize > 0)
                    {
                        rpcZeroRdram(call.rdram, call.recvBuf, call.recvSize);
                    }
                    return 0;
                }
            };

            // 0x8000131Fu: DBC Port 1
            s_rpcHleServices[0x8000131Fu] = {
                0x8000131Fu,
                "DBC_PORT1",
                [](uint32_t sid, uint32_t clientPtr) {
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:dbc] Bound DBC_PORT1 (0x%08x) client=0x%08x\n", sid, clientPtr);
                        fflush(stderr);
                    }
                },
                [](const RpcHleCallContext &call) -> int32_t {
                    recordDbcCall(call.sid, call.rpcNum);
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:dbc] Call DBC_PORT1 fno=0x%x sendBuf=0x%08x (%u) recvBuf=0x%08x (%u)\n",
                                call.rpcNum, call.sendBuf, call.sendSize, call.recvBuf, call.recvSize);
                        fflush(stderr);
                    }
                    if (call.recvBuf && call.recvSize > 0)
                    {
                        rpcZeroRdram(call.rdram, call.recvBuf, call.recvSize);
                    }
                    return 0;
                }
            };

            // 0x11223344u: SG2 Audio Server
            s_rpcHleServices[0x11223344u] = {
                0x11223344u,
                "SG2_AUDIO",
                [](uint32_t sid, uint32_t clientPtr) {
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:sg2] Bound SG2_AUDIO (0x%08x) client=0x%08x\n", sid, clientPtr);
                        fflush(stderr);
                    }
                },
                [](const RpcHleCallContext &call) -> int32_t {
                    static bool s_firstCallLogged = false;
                    if (isVerboseLog()) {
                        ps2xSafeLog( "[rpc:sg2] Call SG2_AUDIO fno=0x%x sendBuf=0x%08x (%u) recvBuf=0x%08x (%u)\n",
                                call.rpcNum, call.sendBuf, call.sendSize, call.recvBuf, call.recvSize);
                    }
                    if (!s_firstCallLogged && call.sendBuf && call.sendSize >= 16)
                    {
                        s_firstCallLogged = true;
                        const uint8_t *src = getConstMemPtr(call.rdram, call.sendBuf);
                        if (src)
                        {
                            ps2xSafeLog( "[SG2:first_call] buffer (%u bytes):\n", call.sendSize);
                            for (uint32_t i = 0; i < call.sendSize; ++i)
                            {
                                ps2xSafeLog( "%02x%c", src[i], ((i + 1) % 16 == 0 || i + 1 == call.sendSize) ? '\n' : ' ');
                            }
                            uint32_t numPackets = call.sendSize / 16;
                            static const char *const kOpcodeNames[] = {
                                "Ping/No-Op", "Play SFX", "Key On", "Key Off", "Set Voice Param", "Stop Voice/Fade",
                                "Master Volume", "Reverb Mode", "Core Effects", "Voice Status", "Core Volume",
                                "Pause Voices", "Resume Voices", "Stop All Voices", "Group Volume", "Group Pitch",
                                "Group Pan", "Load Sound Bank", "Unload Sound Bank", "BGM Volume", "BGM Start",
                                "BGM Stop", "BGM Tempo", "BGM Mute/Solo", "Transfer Waveform", "Wave DMA Poll",
                                "Voice Pitch Bend", "Voice Modulation", "Voice Envelope", "Reset Audio Driver",
                                "Reverb Workarea", "Digital Output Rate", "Voice Noise Mode", "Flush Audio Buffers",
                                "Voice Panpot/3D", "Driver Heartbeat/Sync"
                            };
                            for (uint32_t p = 0; p < numPackets; ++p)
                            {
                                const uint32_t *pkt = reinterpret_cast<const uint32_t *>(src + p * 16);
                                uint32_t op = pkt[0];
                                const char *opName = (op < sizeof(kOpcodeNames)/sizeof(kOpcodeNames[0])) ? kOpcodeNames[op] : "UNKNOWN";
                                ps2xSafeLog( "  pkt[%02u]: opcode=0x%08x (%s) arg1=0x%08x arg2=0x%08x arg3=0x%08x\n",
                                        p, op, opName, pkt[1], pkt[2], pkt[3]);
                            }
                        }
                    }
                    fflush(stderr);
                    if (call.sendBuf && call.sendSize >= 16)
                    {
                        const uint8_t *sgSrc = getConstMemPtr(call.rdram, call.sendBuf);
                        if (sgSrc)
                        {
                            uint32_t numPackets = call.sendSize / 16;
                            const uint32_t curVtable = ::g_trackedCurVtable.load(std::memory_order_relaxed);
                            bool voiceStatusSeen = false;
                            {
                                std::lock_guard<std::mutex> lock(s_sg2ProfMutex);
                                for (uint32_t p = 0; p < numPackets; ++p)
                                {
                                    const uint32_t *pkt = reinterpret_cast<const uint32_t *>(sgSrc + p * 16);
                                    uint32_t op = pkt[0];
                                    s_sg2OpCounts[op]++;
                                    if (op == 0x09) {
                                        voiceStatusSeen = true;
                                    }
                                    if (curVtable == 0x006ae410u && s_sg2DistinctPackets.size() < 60)
                                    {
                                        Sg2PacketKey key{op, pkt[1], pkt[2], pkt[3]};
                                        if (s_sg2DistinctPackets.insert(key).second)
                                        {
                                            ps2xSafeLog("[sg2:pkt] op=0x%02x arg1=0x%08x arg2=0x%08x arg3=0x%08x\n",
                                                        op, pkt[1], pkt[2], pkt[3]);
                                        }
                                    }
                                }
                            }
                            if (voiceStatusSeen)
                            {
                                uint32_t vStatus = 0;
                                if (call.recvBuf && call.recvSize >= 4) {
                                    if (const uint8_t *r = getConstMemPtr(call.rdram, call.recvBuf)) {
                                        std::memcpy(&vStatus, r, 4);
                                    }
                                }
                                ps2xSafeLog("[sg2:voice_status] ret=0x%08x\n", vStatus);
                            }
                        }
                    }
                    auto sg2Now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(sg2Now - s_lastSg2SecLog).count() >= 1000)
                    {
                        s_lastSg2SecLog = sg2Now;
                        std::lock_guard<std::mutex> lock(s_sg2ProfMutex);
                        for (const auto &pair : s_sg2OpCounts)
                        {
                            if (pair.second > 0)
                            {
                                ps2xSafeLog("[sg2] op=0x%02x n=%u\n", pair.first, pair.second);
                            }
                        }
                        s_sg2OpCounts.clear();
                    }

                    if (call.recvBuf && call.recvSize > 0)
                    {
                        uint32_t zeroBytes = std::min(call.recvSize, 448u);
                        rpcZeroRdram(call.rdram, call.recvBuf, zeroBytes);
                    }
                    dbcUpdatePadBuffers(call.rdram, call.runtime);
                    g_diagSg2Fno0Calls.fetch_add(1, std::memory_order_relaxed);
                    return 0;
                }
            };

            std::string initMsg = "[RPC:init] Registered HLE SIDs:";
            char hbuf[64];
            for (const auto &pair : s_rpcHleServices)
            {
                std::snprintf(hbuf, sizeof(hbuf), " 0x%08X (%s)", pair.first, pair.second.name.c_str());
                initMsg += hbuf;
            }
            initMsg += " | 0x80001800 registered: ";
            initMsg += (s_rpcHleServices.find(0x80001800u) != s_rpcHleServices.end()) ? "YES\n" : "NO (myth confirmed)\n";
            ps2xSafeLog("%s", initMsg.c_str());
            std::fflush(stderr);
        }

        void fillRpcDebugPreview(const uint8_t *rdram, uint32_t addr, uint32_t size, uint8_t *preview, uint32_t &previewSize)
        {
            previewSize = 0u;
            if (!rdram || !addr || !preview || size == 0u)
            {
                return;
            }

            const uint32_t count = std::min<uint32_t>(size, static_cast<uint32_t>(kSifRpcDebugPreviewBytes));
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t *ptr = getConstMemPtr(rdram, addr + i);
                if (!ptr)
                {
                    break;
                }
                preview[i] = *ptr;
                ++previewSize;
            }
        }

        std::string formatRpcTraceBytes(const uint8_t *bytes, uint32_t count)
        {
            std::string out;
            if (!bytes || count == 0u)
            {
                return out;
            }

            char item[4] = {};
            for (uint32_t i = 0; i < count; ++i)
            {
                std::snprintf(item, sizeof(item), "%02X", bytes[i]);
                if (!out.empty())
                {
                    out.push_back(' ');
                }
                out += item;
            }
            return out;
        }

#ifndef PS2X_ENABLE_IOP_RPC_TRACE
#define PS2X_ENABLE_IOP_RPC_TRACE 1
#endif

#if PS2X_ENABLE_IOP_RPC_TRACE
        std::string loadedIopModuleTraceSummary()
        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            std::string out;
            uint32_t count = 0u;
            for (const auto &entry : g_sif_modules_by_id)
            {
                const SifModuleRecord &module = entry.second;
                if (!module.loaded)
                {
                    continue;
                }

                if (!out.empty())
                {
                    out += "; ";
                }
                out += module.pathKey.empty() ? module.path : module.pathKey;
                ++count;
                if (count >= 6u)
                {
                    break;
                }
            }
            return out;
        }

        void logUnhandledRpcTrace(const SifRpcDebugEvent &event)
        {
            static std::unordered_set<uint64_t> loggedSignatures;
            if (std::strcmp(event.op ? event.op : "", "CallRpc") != 0)
            {
                return;
            }

            uint64_t signature = 1469598103934665603ull;
            auto mixSignature = [&](uint32_t value)
            {
                signature ^= static_cast<uint64_t>(value);
                signature *= 1099511628211ull;
            };
            mixSignature(event.sid);
            mixSignature(event.rpcNum);
            mixSignature(event.sendSize);
            mixSignature(event.recvSize);
            if (!loggedSignatures.insert(signature).second || loggedSignatures.size() > 128u)
            {
                return;
            }

            const std::string modules = loadedIopModuleTraceSummary();
            std::cerr << "[IOP/RPC trace:unhandled]"
                      << " sid=0x" << std::hex << event.sid
                      << " rpc=0x" << event.rpcNum
                      << " pc=0x" << event.pc
                      << " ra=0x" << event.ra
                      << " send=0x" << event.sendBuf << "/" << std::dec << event.sendSize
                      << " recv=0x" << std::hex << event.recvBuf << "/" << std::dec << event.recvSize
                      << " sendBytes=[" << formatRpcTraceBytes(event.sendPreview, event.sendPreviewSize) << "]"
                      << " loadedModules=[" << modules << "]"
                      << std::dec << std::endl;
        }
#endif

        bool signalRpcCompletionSema(PS2Runtime *runtime, uint32_t semaId)
        {
            if (!runtime || semaId == 0u || semaId > 0xFFFFu)
            {
                return false;
            }
            return runtime->eeScheduler().signalSemaphore(static_cast<int>(semaId), true) >= 0;
        }

    } // namespace

    void ps2xUpdateDbcPadBuffers(uint8_t *rdram, PS2Runtime *runtime)
    {
        dbcUpdatePadBuffers(rdram, runtime);
    }

    void SifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t moduleId = static_cast<int32_t>(getRegU32(ctx, 4)); // $a0
        const uint32_t resultAddr = getRegU32(ctx, 7);                    // $a3 (int* result, optional)

        int32_t moduleResult = -1;
        const bool stoppedByEmulator = runtime->stopIopModule(moduleId, &moduleResult);
        uint32_t refsLeft = 0;
        const bool knownModule = trackSifModuleStop(moduleId, &refsLeft);
        const int32_t ret = (stoppedByEmulator || knownModule) ? 0 : -1;

        if (resultAddr != 0)
        {
            int32_t *hostResult = reinterpret_cast<int32_t *>(getMemPtr(rdram, resultAddr));
            if (hostResult)
            {
                *hostResult = stoppedByEmulator ? moduleResult : (knownModule ? 0 : -1);
            }
        }

        if (stoppedByEmulator || knownModule)
        {
            std::string modulePath;
            {
                std::lock_guard<std::mutex> lock(g_sif_module_mutex);
                auto it = g_sif_modules_by_id.find(moduleId);
                if (it != g_sif_modules_by_id.end())
                {
                    modulePath = it->second.path;
                }
            }
            logSifModuleAction(stoppedByEmulator ? "stop-emulated" : "stop", moduleId, modulePath, refsLeft);
        }

        setReturnS32(ctx, ret);
    }

    void SifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pathAddr = getRegU32(ctx, 4);     // $a0
        const uint32_t argumentSize = getRegU32(ctx, 5); // $a1
        const uint32_t argumentAddr = getRegU32(ctx, 6); // $a2
        const std::string modulePath = readGuestCStringBounded(rdram, pathAddr, kMaxSifModulePathBytes);
        if (modulePath.empty())
        {
            setReturnS32(ctx, -1);
            return;
        }

        std::vector<uint8_t> arguments;
        constexpr uint32_t kMaxIopModuleArguments = 64u * 1024u;
        if (!copyGuestBytesBounded(rdram, argumentAddr, argumentSize, kMaxIopModuleArguments, arguments))
        {
            setReturnS32(ctx, -1);
            return;
        }

        if (!runtime)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const auto loaded = runtime->loadIopModule(modulePath, arguments.empty() ? nullptr : arguments.data(), static_cast<uint32_t>(arguments.size()));
        if (!loaded.handled || loaded.moduleId <= 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        trackSifModuleLoadExternal(modulePath, loaded.moduleId);
        logSifModuleAction("load-emulated", loaded.moduleId, modulePath, 1u);
        setReturnS32(ctx, loaded.moduleId);
    }

    void SifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (!g_rpc_initialized)
        {
            g_rpc_servers.clear();
            g_rpc_clients.clear();
            g_rpc_next_id = 1;
            g_rpc_packet_index = 0;
            g_rpc_server_index = 0;
            g_rpc_active_queue = 0;
            g_sif_rpc_debug_next_seq = 0;
            for (size_t i = 0; i < kSifRpcDebugHistoryCount; ++i)
            {
                g_sif_rpc_debug_history[i] = SifRpcDebugEvent{};
            }
            g_rpc_initialized = true;
            ps2xSafeLog("[SifInitRpc] Initialized\n");
        }

        SifRpcDebugEvent event = makeRpcDebugEvent("InitRpc", ctx, runtime);
        event.result = 0;
        pushSifRpcDebugEventLocked(event);
        setReturnS32(ctx, 0);
    }

    void SifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clientPtr = getRegU32(ctx, 4);
        uint32_t rpcId = getRegU32(ctx, 5);
        uint32_t mode = getRegU32(ctx, 6);

        t_SifRpcClientData *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));

        if (!client)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("BindRpc", ctx, runtime);
            event.clientPtr = clientPtr;
            event.sid = rpcId;
            event.mode = mode;
            event.flags = kSifRpcDebugFlagMissingClient;
            event.result = -1;
            pushSifRpcDebugEvent(event);
            setReturnS32(ctx, -1);
            return;
        }

        client->command = 0;
        client->buf = 0;
        client->cbuf = 0;
        client->end_function = 0;
        client->end_param = 0;
        client->server = 0;
        client->hdr.pkt_addr = 0;
        client->hdr.sema_id = -1;
        client->hdr.mode = mode;

        initRpcHleServices();
        uint32_t serverPtr = 0;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            client->hdr.rpc_id = g_rpc_next_id++;
            auto it = g_rpc_servers.find(rpcId);
            if (it != g_rpc_servers.end())
            {
                serverPtr = it->second.sd_ptr;
            }
            g_rpc_clients[clientPtr] = {};
            g_rpc_clients[clientPtr].sid = rpcId;
        }

        auto hleIt = s_rpcHleServices.find(rpcId);
        const bool isHle = (hleIt != s_rpcHleServices.end());

        if (!serverPtr && (isHle || PS2IopTransport::canBindRpc(runtime, rpcId)))
        {
            // EE-side servers and HLE routes need a descriptor in guest RAM.
            // With an emulated IOP, only publish it after the IRX has actually
            // registered the SID; cd->server == nullptr is the SDK's retry
            // signal while the IOP server thread is still starting.
            serverPtr = rpcAllocServerAddr(rdram);
            if (serverPtr)
            {
                t_SifRpcServerData *dummy = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr));
                if (dummy)
                {
                    std::memset(dummy, 0, sizeof(*dummy));
                    dummy->sid = static_cast<int>(rpcId);
                }
                std::lock_guard<std::mutex> lock(g_rpc_mutex);
                g_rpc_servers[rpcId] = {rpcId, serverPtr};
            }
            if (isHle && hleIt->second.onBind)
            {
                hleIt->second.onBind(rpcId, clientPtr);
            }
        }

        if (serverPtr)
        {
            t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr));
            client->server = serverPtr;
            client->buf = sd ? sd->buf : 0;
            client->cbuf = sd ? sd->cbuf : 0;
            ps2xSafeLog( "[rpc:bind] Bound sid=0x%08x client=0x%08x -> server=0x%08x (%s)\n",
                    rpcId, clientPtr, serverPtr, isHle ? hleIt->second.name.c_str() : "IOP/RPC");
            fflush(stderr);
        }
        else
        {
            client->server = 0;
            client->buf = 0;
            client->cbuf = 0;
            ps2xSafeLog( "[rpc:bind] UNIMPLEMENTED / refused sid=0x%08x client=0x%08x\n",
                    rpcId, clientPtr);
            fflush(stderr);
        }

        SifRpcDebugEvent event = makeRpcDebugEvent("BindRpc", ctx, runtime);
        event.clientPtr = clientPtr;
        event.serverPtr = serverPtr;
        event.sid = rpcId;
        event.mode = mode;
        event.result = 0;
        pushSifRpcDebugEvent(event);
        setReturnS32(ctx, 0);
    }

    void SifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::recursive_mutex> rpcCallLock(g_sif_call_rpc_mutex);

        const uint32_t clientPtr = getRegU32(ctx, 4);
        const uint32_t rpcNum = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);
        const uint32_t sendBuf = getRegU32(ctx, 7);
        const uint32_t stackPointer = getRegU32(ctx, 29);

        const uint32_t sendSizeRegisters = getRegU32(ctx, 8);
        const uint32_t receiveBufferRegisters = getRegU32(ctx, 9);
        const uint32_t receiveSizeRegisters = getRegU32(ctx, 10);
        const uint32_t endFunctionRegisters = getRegU32(ctx, 11);
        uint32_t endParameterRegisters = 0u;
        (void)readStackU32(rdram, stackPointer, 0x0u, endParameterRegisters);

        uint32_t sendSizeStack = 0u;
        uint32_t receiveBufferStack = 0u;
        uint32_t receiveSizeStack = 0u;
        uint32_t endFunctionStack = 0u;
        uint32_t endParameterStack = 0u;
        (void)readStackU32(rdram, stackPointer, 0x10u, sendSizeStack);
        (void)readStackU32(rdram, stackPointer, 0x14u, receiveBufferStack);
        (void)readStackU32(rdram, stackPointer, 0x18u, receiveSizeStack);
        (void)readStackU32(rdram, stackPointer, 0x1Cu, endFunctionStack);
        (void)readStackU32(rdram, stackPointer, 0x20u, endParameterStack);

        const auto looksLikeGuestPointer = [](uint32_t value)
        {
            if (value == 0u)
            {
                return true;
            }
            const uint32_t normalized = value & 0x1FFFFFFFu;
            return normalized >= 0x10000u && normalized < PS2_RAM_SIZE;
        };
        const auto looksLikeSize = [](uint32_t value)
        {
            return value <= 0x02000000u;
        };
        const auto plausiblePack = [&](uint32_t sendSize,
                                       uint32_t receiveBuffer,
                                       uint32_t receiveSize,
                                       uint32_t endFunction)
        {
            return looksLikeSize(sendSize) &&
                   looksLikeGuestPointer(receiveBuffer) &&
                   looksLikeSize(receiveSize) &&
                   (endFunction == 0u || looksLikeGuestPointer(endFunction));
        };

        const bool registerPackPlausible =
            plausiblePack(sendSizeRegisters,
                          receiveBufferRegisters,
                          receiveSizeRegisters,
                          endFunctionRegisters);
        const bool stackPackPlausible =
            plausiblePack(sendSizeStack,
                          receiveBufferStack,
                          receiveSizeStack,
                          endFunctionStack);

        uint32_t sidHint = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            const auto clientIt = g_rpc_clients.find(clientPtr);
            if (clientIt != g_rpc_clients.end())
            {
                sidHint = clientIt->second.sid;
            }
        }

        ps2x::iop::RpcAbi selectedAbi = ps2x::iop::RpcAbi::RuntimeDefault;
        {
            ps2x::iop::RpcAbiRequest request{};
            request.boundSid = sidHint;
            request.function = rpcNum;
            request.registers = {
                sendSizeRegisters,
                receiveBufferRegisters,
                receiveSizeRegisters,
                endFunctionRegisters,
                endParameterRegisters,
                registerPackPlausible,
            };
            request.stack = {
                sendSizeStack,
                receiveBufferStack,
                receiveSizeStack,
                endFunctionStack,
                endParameterStack,
                stackPackPlausible,
            };
            selectedAbi = PS2IopTransport::selectRpcAbi(runtime, request);
        }

        bool useRegisterConvention = selectedAbi != ps2x::iop::RpcAbi::Stack;
        if (selectedAbi == ps2x::iop::RpcAbi::RuntimeDefault && !registerPackPlausible && stackPackPlausible)
        {
            const bool registersHaveCallback = endFunctionRegisters != 0u && looksLikeGuestPointer(endFunctionRegisters);
            const bool stackHasCallback = endFunctionStack != 0u && looksLikeGuestPointer(endFunctionStack);
            if (!(registersHaveCallback && !stackHasCallback))
            {
                useRegisterConvention = false;
            }
        }
        else if (selectedAbi == ps2x::iop::RpcAbi::Registers)
        {
            useRegisterConvention = true;
        }

        const uint32_t sendSize = useRegisterConvention ? sendSizeRegisters : sendSizeStack;
        const uint32_t receiveBuffer = useRegisterConvention ? receiveBufferRegisters : receiveBufferStack;
        const uint32_t receiveSize = useRegisterConvention ? receiveSizeRegisters : receiveSizeStack;
        const uint32_t endFunction = useRegisterConvention ? endFunctionRegisters : endFunctionStack;
        const uint32_t endParameter = useRegisterConvention ? endParameterRegisters : endParameterStack;

        auto *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));
        if (!client)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("CallRpc", ctx, runtime);
            event.clientPtr = clientPtr;
            event.sid = sidHint;
            event.rpcNum = rpcNum;
            event.mode = mode;
            event.sendBuf = sendBuf;
            event.sendSize = sendSize;
            event.recvBuf = receiveBuffer;
            event.recvSize = receiveSize;
            event.endFunc = endFunction;
            event.endParam = endParameter;
            event.flags = kSifRpcDebugFlagMissingClient | ((mode & kSifRpcModeNowait) ? kSifRpcDebugFlagNowait : 0u);
            event.result = -1;
            pushSifRpcDebugEvent(event);
            setReturnS32(ctx, -1);
            return;
        }

        client->command = rpcNum;
        client->end_function = endFunction;
        client->end_param = endParameter;
        client->hdr.mode = mode;

        uint32_t sid = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            auto &state = g_rpc_clients[clientPtr];
            state.busy = true;
            state.last_rpc = rpcNum;
            sid = state.sid;
            if (sid != 0u)
            {
                const auto serverIt = g_rpc_servers.find(sid);
                if (serverIt != g_rpc_servers.end() &&
                    serverIt->second.sd_ptr != 0u)
                {
                    client->server = serverIt->second.sd_ptr;
                }
            }
        }

        const uint32_t serverPtr = client->server;
        static int s_traceRpc = -1;
        if (s_traceRpc == -1) {
            const char *env = std::getenv("PS2X_TRACE_RPC");
            s_traceRpc = (env && (env[0] == '1' || env[0] == 't')) ? 1 : 0;
        }
        if (s_traceRpc)
        {
            char rpcLine[512];
            std::snprintf(rpcLine, sizeof(rpcLine),
                          "[rpc:call] sid=0x%08x fno=0x%x mode=0x%x sendBuf=0x%08x (%u bytes) recvBuf=0x%08x (%u bytes) endFunc=0x%08x endParam=0x%08x\n",
                          sid, rpcNum, mode, sendBuf, sendSize, receiveBuffer, receiveSize, endFunction, endParameter);
            if (sendBuf != 0u && sendSize > 0u)
            {
                const uint32_t dumpLen = std::min<uint32_t>(sendSize, 64u);
                if (const uint8_t *sptr = getConstMemPtr(rdram, sendBuf))
                {
                    std::string hexDump = "  send hex[" + std::to_string(dumpLen) + "]:";
                    char hexByte[8];
                    for (uint32_t i = 0; i < dumpLen; ++i)
                    {
                        std::snprintf(hexByte, sizeof(hexByte), " %02x", sptr[i]);
                        hexDump += hexByte;
                    }
                    hexDump += "\n";
                    ps2xSafeLog("%s%s", rpcLine, hexDump.c_str());
                }
                else
                {
                    ps2xSafeLog("%s", rpcLine);
                }
            }
            else
            {
                ps2xSafeLog("%s", rpcLine);
            }
        }

        auto *server = serverPtr
                           ? reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr))
                           : nullptr;
        if (server)
        {
            server->client = clientPtr;
            server->pkt_addr = client->hdr.pkt_addr;
            server->rpc_number = rpcNum;
            server->size = static_cast<int>(sendSize);
            server->recvbuf = receiveBuffer;
            server->rsize = static_cast<int>(receiveSize);
            server->rmode = ((mode & kSifRpcModeNowait) && endFunction == 0u) ? 0 : 1;
            server->rid = 0;

            if (server->buf != 0u && sendBuf != 0u && sendSize != 0u)
            {
                rpcCopyToRdram(rdram, server->buf, sendBuf, sendSize);
            }
        }

        ps2x::iop::RpcResult iopResult{};
        uint32_t completionSemaphore = static_cast<uint32_t>(client->hdr.sema_id);
        if (completionSemaphore == 0xFFFFFFFFu || completionSemaphore == 0u)
        {
            completionSemaphore = endParameter;
        }

        {
            ps2x::iop::RpcRequest request{};
            request.clientAddress = clientPtr;
            request.serverAddress = serverPtr;
            request.serverFunction = server ? server->func : 0u;
            request.serverBuffer = server ? server->buf : 0u;
            request.sid = sid;
            request.function = rpcNum;
            request.mode = mode;
            request.send = {sendBuf, sendSize};
            request.receive = {receiveBuffer, receiveSize};
            request.endFunction = endFunction;
            request.endParameter = endParameter;

            iopResult = PS2IopTransport::handleRpc(runtime, rdram, ctx, request);

            if (iopResult.signalNowaitCompletion &&
                (mode & kSifRpcModeNowait) != 0u)
            {
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
            }
            if (iopResult.signalCompletion)
            {
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
            }
        }

        uint32_t guestFunction = iopResult.guestFunction;
        uint32_t guestA0 = iopResult.guestArguments[0];
        uint32_t guestA1 = iopResult.guestArguments[1];
        uint32_t guestA2 = iopResult.guestArguments[2];
        uint32_t guestA3 = iopResult.guestArguments[3];
        uint32_t guestDefaultResult = iopResult.guestDefaultResultAddress;
        if (guestFunction == 0u && server && server->func != 0u &&
            iopResult.serverDispatchPolicy != ps2x::iop::ServerDispatchPolicy::Suppress)
        {
            guestFunction = server->func;
            guestA0 = rpcNum;
            guestA1 = server->buf;
            guestA2 = sendSize;
            guestDefaultResult = server->buf != 0u ? server->buf : receiveBuffer;
        }
        const bool serverDispatched = guestFunction != 0u && runtime->hasFunction(guestFunction);

        auto hleCallIt = s_rpcHleServices.find(sid);
        if (hleCallIt != s_rpcHleServices.end() && hleCallIt->second.onCall)
        {
            RpcHleCallContext hleCall{};
            hleCall.rdram = rdram;
            hleCall.ctx = ctx;
            hleCall.runtime = runtime;
            hleCall.clientPtr = clientPtr;
            hleCall.sid = sid;
            hleCall.rpcNum = rpcNum;
            hleCall.mode = mode;
            hleCall.sendBuf = sendBuf;
            hleCall.sendSize = sendSize;
            hleCall.recvBuf = receiveBuffer;
            hleCall.recvSize = receiveSize;
            hleCall.endFunc = endFunction;
            hleCall.endParam = endParameter;

            hleCallIt->second.onCall(hleCall);

            iopResult.handled = true;
            iopResult.resultAddress = receiveBuffer;
            if (completionSemaphore != 0u && completionSemaphore != 0xFFFFFFFFu)
            {
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
            }
        }

        auto finishCall = [=](const R5900Context *guestResult, R5900Context &parent)
        {
            bool handled = iopResult.handled;
            uint32_t resultPointer = iopResult.resultAddress;
            bool copiedFallback = false;
            bool zeroedFallback = false;
            if (guestResult)
            {
                handled = true;
                resultPointer = getRegU32(guestResult, 2);
                if (resultPointer == 0u)
                {
                    resultPointer = guestDefaultResult;
                }
            }

            if (receiveBuffer != 0u && receiveSize != 0u)
            {
                if (handled && resultPointer != 0u && resultPointer != receiveBuffer)
                {
                    rpcCopyToRdram(rdram, receiveBuffer, resultPointer, receiveSize);
                }
                else if (!handled && sendBuf != 0u && sendSize != 0u && sendBuf != receiveBuffer)
                {
                    rpcCopyToRdram(rdram, receiveBuffer, sendBuf, std::min(sendSize, receiveSize));
                    copiedFallback = true;
                }
                else if (!handled)
                {
                    rpcZeroRdram(rdram, receiveBuffer, receiveSize);
                    zeroedFallback = true;
                }
            }

            auto completeClient = [=](R5900Context &base, bool callbackCompleted)
            {
                {
                    std::lock_guard<std::mutex> lock(g_rpc_mutex);
                    g_rpc_clients[clientPtr].busy = false;
                }
                SifRpcDebugEvent event = makeRpcDebugEvent("CallRpc", &base, runtime);
                event.clientPtr = clientPtr;
                event.serverPtr = serverPtr;
                event.sid = sid;
                event.rpcNum = rpcNum;
                event.mode = mode;
                event.sendBuf = sendBuf;
                event.sendSize = sendSize;
                event.recvBuf = receiveBuffer;
                event.recvSize = receiveSize;
                event.resultPtr = resultPointer;
                event.endFunc = endFunction;
                event.endParam = endParameter;
                event.semaId = completionSemaphore;
                event.flags =
                    ((mode & kSifRpcModeNowait) ? kSifRpcDebugFlagNowait : 0u) |
                    (iopResult.handled ? kSifRpcDebugFlagHandledByHle : 0u) |
                    (callbackCompleted ? kSifRpcDebugFlagCallback : 0u) |
                    (serverDispatched ? kSifRpcDebugFlagServerDispatch : 0u) |
                    (!handled ? kSifRpcDebugFlagUnhandled : 0u) |
                    (copiedFallback ? kSifRpcDebugFlagFallbackCopy : 0u) |
                    (zeroedFallback ? kSifRpcDebugFlagFallbackZero : 0u);
                fillRpcDebugPreview(rdram, sendBuf, sendSize, event.sendPreview, event.sendPreviewSize);
                fillRpcDebugPreview(rdram, receiveBuffer, receiveSize, event.recvPreview, event.recvPreviewSize);
                event.result = 0;
#if PS2X_ENABLE_IOP_RPC_TRACE
                if ((event.flags & kSifRpcDebugFlagUnhandled) != 0u)
                {
                    logUnhandledRpcTrace(event);
                }
#endif
                pushSifRpcDebugEvent(event);
            };

            setReturnS32(&parent, 0);
            if (endFunction == 0u || iopResult.callbackPolicy == ps2x::iop::CallbackPolicy::Suppress)
            {
                completeClient(parent, endFunction != 0u);
                return;
            }

            uint32_t callbackFunction = endFunction;
            if (!runtime->hasFunction(callbackFunction) && callbackFunction >= 0x10000u &&
                runtime->hasFunction(callbackFunction - 0x10000u))
            {
                callbackFunction -= 0x10000u;
            }
            if (!runtime->hasFunction(callbackFunction))
            {
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
                completeClient(parent, false);
                return;
            }

            GuestInvocation callback{};
            callback.kind = GuestInvocationKind::RpcCallback;
            callback.context = parent;
            callback.context.pc = callbackFunction;
            SET_GPR_U32(&callback.context, 4, endParameter);
            SET_GPR_U32(&callback.context, 29, runtime->eeScheduler().invocationStackTop());
            SET_GPR_U32(&callback.context, 31, 0u);
            callback.onComplete = [completeClient](const R5900Context &, R5900Context &base)
            {
                completeClient(base, true);
            };
            runtime->eeScheduler().invokeCurrent(std::move(callback));
        };

        if (hleCallIt != s_rpcHleServices.end() && hleCallIt->second.onCall)
        {
            finishCall(nullptr, *ctx);
            return;
        }

        if (serverDispatched)
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::RpcCallback;
            invocation.context = *ctx;
            invocation.context.pc = guestFunction;
            SET_GPR_U32(&invocation.context, 4, guestA0);
            SET_GPR_U32(&invocation.context, 5, guestA1);
            SET_GPR_U32(&invocation.context, 6, guestA2);
            SET_GPR_U32(&invocation.context, 7, guestA3);
            SET_GPR_U32(&invocation.context, 29, runtime->eeScheduler().invocationStackTop());
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [finishCall](const R5900Context &completed, R5900Context &parent)
            {
                finishCall(&completed, parent);
            };
            runtime->eeScheduler().invokeCurrent(std::move(invocation));
        }
        finishCall(nullptr, *ctx);
    }

    void SifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sdPtr = getRegU32(ctx, 4);
        uint32_t sid = getRegU32(ctx, 5);
        uint32_t func = getRegU32(ctx, 6);
        uint32_t buf = getRegU32(ctx, 7);
        // stack args: cfunc, cbuf, qd...
        uint32_t sp = getRegU32(ctx, 29);
        uint32_t cfunc = 0;
        uint32_t cbuf = 0;
        uint32_t qd = 0;
        readStackU32(rdram, sp, 0x10, cfunc);
        readStackU32(rdram, sp, 0x14, cbuf);
        readStackU32(rdram, sp, 0x18, qd);

        t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
        if (!sd)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("RegisterRpc", ctx, runtime);
            event.serverPtr = sdPtr;
            event.sid = sid;
            event.sendBuf = buf;
            event.recvBuf = cbuf;
            event.resultPtr = qd;
            event.endFunc = cfunc;
            event.flags = kSifRpcDebugFlagMissingClient;
            event.result = -1;
            pushSifRpcDebugEvent(event);
            setReturnS32(ctx, -1);
            return;
        }

        sd->sid = static_cast<int>(sid);
        sd->func = func;
        sd->buf = buf;
        sd->size = 0;
        sd->cfunc = cfunc;
        sd->cbuf = cbuf;
        sd->size2 = 0;
        sd->client = 0;
        sd->pkt_addr = 0;
        sd->rpc_number = 0;
        sd->recvbuf = 0;
        sd->rsize = 0;
        sd->rmode = 0;
        sd->rid = 0;
        sd->base = qd;
        sd->link = 0;
        sd->next = 0;

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);

            if (qd)
            {
                t_SifRpcDataQueue *queue = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qd));
                if (queue)
                {
                    if (!queue->link)
                    {
                        queue->link = sdPtr;
                    }
                    else
                    {
                        uint32_t curPtr = queue->link;
                        for (int guard = 0; guard < 1024 && curPtr; ++guard)
                        {
                            t_SifRpcServerData *cur = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, curPtr));
                            if (!cur)
                                break;
                            if (!cur->link)
                            {
                                cur->link = sdPtr;
                                break;
                            }
                            if (cur->link == sdPtr)
                                break;
                            curPtr = cur->link;
                        }
                    }
                }
            }

            g_rpc_servers[sid] = {sid, sdPtr};
            for (auto &entry : g_rpc_clients)
            {
                if (entry.second.sid == sid)
                {
                    t_SifRpcClientData *cd = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, entry.first));
                    if (cd)
                    {
                        cd->server = sdPtr;
                        cd->buf = sd->buf;
                        cd->cbuf = sd->cbuf;
                    }
                }
            }
        }

        RUNTIME_LOG("[SifRegisterRpc] sid=0x" << std::hex << sid << " sd=0x" << sdPtr << std::dec);
        SifRpcDebugEvent event = makeRpcDebugEvent("RegisterRpc", ctx, runtime);
        event.serverPtr = sdPtr;
        event.sid = sid;
        event.sendBuf = buf;
        event.recvBuf = cbuf;
        event.resultPtr = qd;
        event.endFunc = cfunc;
        event.result = 0;
        pushSifRpcDebugEvent(event);
        setReturnS32(ctx, 0);
    }

    void SifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clientPtr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        auto it = g_rpc_clients.find(clientPtr);
        if (it == g_rpc_clients.end())
        {
            setReturnS32(ctx, 0);
            return;
        }
        setReturnS32(ctx, it->second.busy ? 1 : 0);
    }

    void SifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t qdPtr = getRegU32(ctx, 4);
        int threadId = static_cast<int>(getRegU32(ctx, 5));

        t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
        if (!qd)
        {
            setReturnS32(ctx, -1);
            return;
        }

        qd->thread_id = threadId;
        qd->active = 0;
        qd->link = 0;
        qd->start = 0;
        qd->end = 0;
        qd->next = 0;

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            if (!g_rpc_active_queue)
            {
                g_rpc_active_queue = qdPtr;
            }
            else
            {
                uint32_t curPtr = g_rpc_active_queue;
                for (int guard = 0; guard < 1024 && curPtr; ++guard)
                {
                    if (curPtr == qdPtr)
                        break;
                    t_SifRpcDataQueue *cur = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, curPtr));
                    if (!cur)
                        break;
                    if (!cur->next)
                    {
                        cur->next = qdPtr;
                        break;
                    }
                    curPtr = cur->next;
                }
            }
        }

        setReturnS32(ctx, 0);
    }

    void SifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t qdPtr = getRegU32(ctx, 4);
        if (!qdPtr)
        {
            setReturnU32(ctx, 0);
            return;
        }

        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (!g_rpc_active_queue)
        {
            setReturnU32(ctx, 0);
            return;
        }

        if (g_rpc_active_queue == qdPtr)
        {
            t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
            g_rpc_active_queue = qd ? qd->next : 0;
            setReturnU32(ctx, qdPtr);
            return;
        }

        uint32_t curPtr = g_rpc_active_queue;
        for (int guard = 0; guard < 1024 && curPtr; ++guard)
        {
            t_SifRpcDataQueue *cur = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, curPtr));
            if (!cur)
                break;
            if (cur->next == qdPtr)
            {
                t_SifRpcDataQueue *rem = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
                cur->next = rem ? rem->next : 0;
                setReturnU32(ctx, qdPtr);
                return;
            }
            curPtr = cur->next;
        }

        setReturnU32(ctx, 0);
    }

    void SifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sdPtr = getRegU32(ctx, 4);
        uint32_t qdPtr = getRegU32(ctx, 5);

        t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
        if (!qd || !sdPtr)
        {
            setReturnU32(ctx, 0);
            return;
        }

        std::lock_guard<std::mutex> lock(g_rpc_mutex);

        if (qd->link == sdPtr)
        {
            t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
            qd->link = sd ? sd->link : 0;
            if (sd)
                sd->link = 0;
            setReturnU32(ctx, sdPtr);
            return;
        }

        uint32_t curPtr = qd->link;
        for (int guard = 0; guard < 1024 && curPtr; ++guard)
        {
            t_SifRpcServerData *cur = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, curPtr));
            if (!cur)
                break;
            if (cur->link == sdPtr)
            {
                t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
                cur->link = sd ? sd->link : 0;
                if (sd)
                    sd->link = 0;
                setReturnU32(ctx, sdPtr);
                return;
            }
            curPtr = cur->link;
        }

        setReturnU32(ctx, 0);
    }

    void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SifCallRpc(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t cid = getRegU32(ctx, 4);
        uint32_t packetAddr = getRegU32(ctx, 5);
        uint32_t packetSize = getRegU32(ctx, 6);
        uint32_t srcExtra = getRegU32(ctx, 7);
        uint32_t destExtra = getRegU32(ctx, 8);
        uint32_t sizeExtra = getRegU32(ctx, 9);

        if (sizeExtra > 0 && srcExtra != 0u && destExtra != 0u && runtime != nullptr)
        {
            if (const uint8_t *src = getConstMemPtr(rdram, srcExtra))
            {
                runtime->writeIopMemory(destExtra, src, sizeExtra);
            }
        }

        static int logCount = 0;
        if (logCount < 10)
        {
            ps2xSafeLog( "[sceSifSendCmd] cid=0x%08x packet=0x%08x psize=%u src=0x%08x dst=0x%08x size=%u (ra=0x%08x)\n",
                         cid, packetAddr, packetSize, srcExtra, destExtra, sizeExtra, getRegU32(ctx, 31));
            std::fflush(stderr);
            ++logCount;
        }

        // Handle file3.irx command 1 (handshake and task dispatch)
        if (cid == 1u)
        {
            struct VfiDescriptorSlot
            {
                bool open = false;
                ps2x::vfi::VfiEntry entry;
                uint64_t offset = 0;
            };
            static VfiDescriptorSlot s_vfiDescriptors[16] = {};

            ps2x::vfi::VfiArchive::instance().initialize();

            const uint8_t *src = (srcExtra != 0u) ? getConstMemPtr(rdram, srcExtra) : nullptr;
            if (!src)
            {
                ps2xSafeLog( "[CID1] ERROR: srcExtra 0x%08x is invalid\n", srcExtra);
                std::fflush(stderr);
                return;
            }

            uint32_t cmdSize = *reinterpret_cast<const uint32_t *>(src + 0);
            uint32_t opcode = *reinterpret_cast<const uint32_t *>(src + 4);
            uint32_t slotPtr = *reinterpret_cast<const uint32_t *>(src + 8);
            uint32_t seq = *reinterpret_cast<const uint32_t *>(src + 12);

            uint32_t alignedCmdSize = (cmdSize + 15u) & ~15u;
            uint32_t ringTail = destExtra + alignedCmdSize;
            g_diagCid1Requests.fetch_add(1, std::memory_order_relaxed);

            const char *traceEnv = std::getenv("PS2X_CID1_TRACE");
            const bool doTrace = (traceEnv && std::strcmp(traceEnv, "1") == 0);
            if (doTrace)
            {
                ps2xSafeLog( "[CID1:trace request] srcExtra=0x%08x size=%u (aligned=%u) op=0x%x slot=0x%08x seq=%u\n",
                             srcExtra, cmdSize, alignedCmdSize, opcode, slotPtr, seq);
                const uint32_t dumpLen = std::min(cmdSize, 64u);
                std::string reqDump = "  req bytes[" + std::to_string(dumpLen) + "]:";
                char bbuf[8];
                for (uint32_t i = 0; i < dumpLen; ++i) { std::snprintf(bbuf, sizeof(bbuf), " %02x", src[i]); reqDump += bbuf; }
                reqDump += "\n";
                ps2xSafeLog("%s", reqDump.c_str());
                std::fflush(stderr);
            }

#pragma pack(push, 1)
            struct SifReplyPacketCid1Full
            {
                uint32_t sifData = 0;
                uint32_t sifPacketSize = sizeof(SifReplyPacketCid1Full);
                uint32_t sifCid = 1;
                uint32_t sifPad0 = 0;

                uint32_t pad1 = 0;
                uint32_t cmd = 0;
                uint32_t slotPtr = 0;
                uint32_t seq = 0;
                uint32_t result = 0;
                uint32_t ringTail = 0;
                uint32_t isDone = 1;
                uint32_t statDstPtr = 0;

                uint8_t statData[64] = {};
            } reply{};
#pragma pack(pop)
            static_assert(sizeof(SifReplyPacketCid1Full) == 112, "SifReplyPacketCid1Full must be 112 bytes");

            reply.cmd = opcode;
            reply.slotPtr = slotPtr;
            reply.seq = seq;
            reply.ringTail = ringTail;
            reply.isDone = 1;

            switch (opcode)
            {
            case 0x03: // Open
            {
                const char *pathPtr = reinterpret_cast<const char *>(src + 0x20);
                std::string path(pathPtr);
                    diagSetLastCid1Path(path);
                const auto *entry = ps2x::vfi::VfiArchive::instance().find(path);
                if (!entry)
                {
                    ps2xSafeLog( "[CID1:Open] NOT FOUND: '%s'\n", path.c_str());
                    reply.result = static_cast<uint32_t>(-1);
                }
                else
                {
                    constexpr int kCid1FdBase = 3;
                    int allocSlot = -1;
                    for (int i = 0; i < 16; ++i)
                    {
                        if (!s_vfiDescriptors[i].open)
                        {
                            allocSlot = i;
                            break;
                        }
                    }
                    if (allocSlot < 0)
                    {
                        ps2xSafeLog( "[CID1:Open] ERROR: out of FDs for '%s'\n", path.c_str());
                        reply.result = static_cast<uint32_t>(-1);
                    }
                    else
                    {
                        s_vfiDescriptors[allocSlot].open = true;
                        s_vfiDescriptors[allocSlot].entry = *entry;
                        s_vfiDescriptors[allocSlot].offset = 0;
                        int allocFd = allocSlot + kCid1FdBase;
                        reply.result = static_cast<uint32_t>(allocFd);
                        ps2xSafeLog( "[CID1:Open] fd=%d (slot=%d) -> '%s' (size=%llu, LSN=%u)\n",
                                     allocFd, allocSlot, path.c_str(), static_cast<unsigned long long>(entry->fileSize), entry->isoLsn);
                    }
                }
                break;
            }
            case 0x04: // Close
            {
                constexpr int kCid1FdBase = 3;
                uint32_t fd = *reinterpret_cast<const uint32_t *>(src + 0x10);
                int fdSlot = static_cast<int>(fd) - kCid1FdBase;
                if (fdSlot >= 0 && fdSlot < 16 && s_vfiDescriptors[fdSlot].open)
                {
                    s_vfiDescriptors[fdSlot].open = false;
                    reply.result = 0;
                    ps2xSafeLog( "[CID1:Close] fd=%u (slot=%d)\n", fd, fdSlot);
                }
                else
                {
                    ps2xSafeLog( "[CID1:Close] fd=%u (slot=%d) INVALID\n", fd, fdSlot);
                    reply.result = static_cast<uint32_t>(-1);
                }
                break;
            }
            case 0x06: // Read fd
            {
                constexpr int kCid1FdBase = 3;
                uint32_t fd = *reinterpret_cast<const uint32_t *>(src + 0x10);
                uint32_t eeDst = *reinterpret_cast<const uint32_t *>(src + 0x14);
                uint32_t reqSize = *reinterpret_cast<const uint32_t *>(src + 0x18);
                int fdSlot = static_cast<int>(fd) - kCid1FdBase;
                if (fdSlot >= 0 && fdSlot < 16 && s_vfiDescriptors[fdSlot].open)
                {
                    auto &slot = s_vfiDescriptors[fdSlot];
                    uint64_t avail = (slot.entry.fileSize > slot.offset) ? (slot.entry.fileSize - slot.offset) : 0ull;
                    uint64_t toRead = std::min(static_cast<uint64_t>(reqSize), avail);
                    if (eeDst + toRead > PS2_RAM_SIZE)
                    {
                        ps2xSafeLog( "[CID1:ReadFd] ERROR: eeDst 0x%08x + %llu exceeds RAM\n", eeDst, (unsigned long long)toRead);
                        reply.result = static_cast<uint32_t>(-1);
                    }
                    else
                    {
                        if (toRead > 0)
                        {
                            ps2x::vfi::VfiArchive::instance().readFileData(slot.entry, slot.offset, toRead, rdram + eeDst);
                        }
                        slot.offset += toRead;
                        g_diagCid1BytesServed.fetch_add(toRead, std::memory_order_relaxed);
                        g_movieReadsCount.fetch_add(1, std::memory_order_relaxed);
                        diagSetLastCid1Path(slot.entry.fullPath);
                        reply.result = static_cast<uint32_t>(toRead);

                        uint32_t crc = 0xFFFFFFFFu;
                        for (size_t i = 0; i < toRead; ++i)
                        {
                            crc ^= (rdram + eeDst)[i];
                            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                        }
                        crc ^= 0xFFFFFFFFu;

                        ps2xSafeLog( "[CID1:ReadFd] fd=%u eeDst=0x%08x req=%u read=%llu (newOff=%llu CRC32=0x%08X)\n",
                                     fd, eeDst, reqSize, (unsigned long long)toRead, (unsigned long long)slot.offset, crc);
                    }
                }
                else
                {
                    ps2xSafeLog( "[CID1:ReadFd] fd=%u INVALID\n", fd);
                    reply.result = static_cast<uint32_t>(-1);
                }
                break;
            }
            case 0x07: // Seek
            {
                constexpr int kCid1FdBase = 3;
                uint32_t fd = *reinterpret_cast<const uint32_t *>(src + 0x10);
                int32_t seekOffset = *reinterpret_cast<const int32_t *>(src + 0x14);
                uint32_t whence = *reinterpret_cast<const uint32_t *>(src + 0x18);

                // Requirement 1: Log all raw 32-bit words of the CID1:Seek request
                {
                    std::string reqDump;
                    char buf[32];
                    uint32_t wordCount = std::min(cmdSize / 4u, 16u);
                    for (uint32_t i = 0; i < wordCount; ++i)
                    {
                        uint32_t w = *reinterpret_cast<const uint32_t *>(src + i * 4);
                        std::snprintf(buf, sizeof(buf), "w[%u]=0x%08x ", i, w);
                        reqDump += buf;
                    }
                    ps2xSafeLog("[CID1:Seek:raw_req] %s\n", reqDump.c_str());
                }

                int fdSlot = static_cast<int>(fd) - kCid1FdBase;
                if (fdSlot >= 0 && fdSlot < 16 && s_vfiDescriptors[fdSlot].open)
                {
                    auto &slot = s_vfiDescriptors[fdSlot];
                    int64_t newOff = 0;
                    uint32_t seekMode = whence & 0xFFFFu;
                    if (seekMode == 0) // SEEK_SET
                        newOff = seekOffset;
                    else if (seekMode == 1) // SEEK_CUR
                        newOff = static_cast<int64_t>(slot.offset) + seekOffset;
                    else if (seekMode == 2) // SEEK_END
                        newOff = static_cast<int64_t>(slot.entry.fileSize) + seekOffset;
                    else
                    {
                        ps2xSafeLog("[CID1:Seek] UNKNOWN whence=0x%08x (mode=%u)\n", whence, seekMode);
                    }
                    if (newOff < 0) newOff = 0;
                    if (static_cast<uint64_t>(newOff) > slot.entry.fileSize) newOff = static_cast<int64_t>(slot.entry.fileSize);
                    slot.offset = static_cast<uint64_t>(newOff);
                    reply.result = 0; // file3.irx:0x0c30 returns 0 (SCE_OK)
                    g_movieSeekDone.store(true, std::memory_order_relaxed);
                    ps2xSafeLog("[CID1:Seek] fd=%u raw_whence=0x%08x (mode=%u) raw_offset=%d file_size=%llu -> returned_newOff=%lld\n",
                                fd, whence, seekMode, seekOffset, (unsigned long long)slot.entry.fileSize, (long long)newOff);
                }
                else
                {
                    ps2xSafeLog( "[CID1:Seek] fd=%u INVALID\n", fd);
                    reply.result = static_cast<uint32_t>(-1);
                }

                // Requirement 1: Log all raw 32-bit words of the CID1:Seek reply
                {
                    std::string repDump;
                    char buf[32];
                    const uint32_t *repWords = reinterpret_cast<const uint32_t *>(&reply);
                    uint32_t repWordCount = sizeof(reply) / 4u;
                    for (uint32_t i = 0; i < repWordCount; ++i)
                    {
                        std::snprintf(buf, sizeof(buf), "w[%u]=0x%08x ", i, repWords[i]);
                        repDump += buf;
                    }
                    ps2xSafeLog("[CID1:Seek:raw_reply] %s\n", repDump.c_str());
                }
                break;
            }
            case 0x09: // Getstat
            {
                uint32_t statDst = *reinterpret_cast<const uint32_t *>(src + 0x10);
                const char *pathPtr = reinterpret_cast<const char *>(src + 0x18);
                std::string path(pathPtr);
                const auto *entry = ps2x::vfi::VfiArchive::instance().find(path);
                const auto *dirEntry = entry ? nullptr : ps2x::vfi::VfiArchive::instance().findDir(path);
                if (!entry && !dirEntry)
                {
                    ps2xSafeLog( "[CID1:Getstat] NOT FOUND: '%s'\n", path.c_str());
                    reply.result = static_cast<uint32_t>(-1);
                }
                else
                {
                    uint8_t statBuf[64] = {};
                    if (entry)
                    {
                        *reinterpret_cast<uint32_t *>(statBuf + 0x00) = 0x2000u; // mode: regular file (file3.irx: 0x2000)
                        *reinterpret_cast<uint32_t *>(statBuf + 0x04) = entry->startSector;
                        *reinterpret_cast<uint32_t *>(statBuf + 0x08) = static_cast<uint32_t>(entry->fileSize);
                        *reinterpret_cast<uint64_t *>(statBuf + 0x08) = entry->fileSize;
                        ps2xSafeLog( "[CID1:Getstat] '%s' -> FILE size=%llu startSec=%u\n",
                                     path.c_str(), static_cast<unsigned long long>(entry->fileSize), entry->startSector);
                    }
                    else
                    {
                        *reinterpret_cast<uint32_t *>(statBuf + 0x00) = 0x1000u; // mode: directory (file3.irx: 0x1000)
                        *reinterpret_cast<uint32_t *>(statBuf + 0x04) = 0u;
                        *reinterpret_cast<uint32_t *>(statBuf + 0x08) = 0u;
                        *reinterpret_cast<uint64_t *>(statBuf + 0x08) = 0ull;
                        ps2xSafeLog( "[CID1:Getstat] '%s' -> DIR (mode=0x1000)\n", path.c_str());
                    }
                    reply.statDstPtr = statDst;
                    std::memcpy(reply.statData, statBuf, 64);
                    if (statDst != 0u && statDst + 64 <= PS2_RAM_SIZE)
                    {
                        std::memcpy(rdram + statDst, statBuf, 64);
                    }
                    reply.result = 0;
                }
                break;
            }
            case 0x0A: // Directory Enumeration (file3.irx:0x0f54 dopen/dread)
            {
                uint32_t dstEeAddr = *reinterpret_cast<const uint32_t *>(src + 0x10);
                uint32_t maxEntries = *reinterpret_cast<const uint32_t *>(src + 0x14);
                uint32_t bufSize = *reinterpret_cast<const uint32_t *>(src + 0x18);
                uint32_t flags = (cmdSize >= 0x20u) ? *reinterpret_cast<const uint32_t *>(src + 0x1c) : 0u;
                const char *pathPtr = reinterpret_cast<const char *>(src + 0x20);
                std::string path(pathPtr ? pathPtr : "");
                diagSetLastCid1Path(path);

                if (bufSize < 0x104u)
                {
                    ps2xSafeLog("[CID1:EnumDir-0x0A] ERROR: bufferSize %u < 0x104 for '%s'\n", bufSize, path.c_str());
                    reply.result = static_cast<uint32_t>(-12);
                    break;
                }

                const auto *dirEntry = ps2x::vfi::VfiArchive::instance().findDir(path);
                if (!dirEntry)
                {
                    ps2xSafeLog("[CID1:EnumDir-0x0A] NOT FOUND: '%s'\n", path.c_str());
                    reply.result = static_cast<uint32_t>(-2);
                    break;
                }

                auto children = ps2x::vfi::VfiArchive::instance().enumerateDirectory(path);
                uint32_t totalEntries = static_cast<uint32_t>(children.size());

                if (maxEntries > 0u && totalEntries > maxEntries)
                {
                    ps2xSafeLog("[CID1:EnumDir-0x0A] ERROR: totalEntries %u > maxEntries %u for '%s'\n", totalEntries, maxEntries, path.c_str());
                    reply.result = static_cast<uint32_t>(-12);
                    break;
                }

                uint32_t totalStrBytes = 0u;
                for (const auto &c : children)
                {
                    totalStrBytes += static_cast<uint32_t>(c.name.size() + 1u);
                }

                // Layout in EE RDRAM matching file3.irx:0x0f54:
                // 1. Array of N stat structs (64 bytes each) at dstEeAddr
                // 2. Array of N string pointers (4 bytes each) at dstEeAddr + N * 64
                // 3. Packed null-terminated string bytes at dstEeAddr + N * 64 + N * 4
                uint32_t statArrayEe = dstEeAddr;
                uint32_t ptrArrayEe = dstEeAddr + totalEntries * 64u;
                uint32_t strPoolEe = ptrArrayEe + totalEntries * 4u;
                uint32_t totalBytes = totalEntries * 64u + totalEntries * 4u + totalStrBytes;

                if (totalBytes > bufSize || (dstEeAddr != 0u && dstEeAddr + totalBytes > PS2_RAM_SIZE))
                {
                    ps2xSafeLog("[CID1:EnumDir-0x0A] ERROR: totalBytes %u exceeds bufSize %u or RAM for '%s'\n", totalBytes, bufSize, path.c_str());
                    reply.result = static_cast<uint32_t>(-12);
                    break;
                }

                if (dstEeAddr != 0u && dstEeAddr + totalBytes <= PS2_RAM_SIZE)
                {
                    std::memset(rdram + statArrayEe, 0, totalEntries * 64u);
                    uint32_t curStrEe = strPoolEe;
                    for (uint32_t i = 0; i < totalEntries; ++i)
                    {
                        const auto &child = children[i];
                        uint32_t statEe = statArrayEe + i * 64u;

                        uint32_t mode = child.isDirectory ? 0x1000u : 0x2000u; // dir=0x1000, file=0x2000 (file3.irx:0x0DEC)
                        *reinterpret_cast<uint32_t *>(rdram + statEe + 0x00) = mode;
                        *reinterpret_cast<uint32_t *>(rdram + statEe + 0x04) = child.startSector;
                        *reinterpret_cast<uint32_t *>(rdram + statEe + 0x08) = static_cast<uint32_t>(child.fileSize);
                        *reinterpret_cast<uint64_t *>(rdram + statEe + 0x08) = child.fileSize;

                        *reinterpret_cast<uint32_t *>(rdram + ptrArrayEe + i * 4u) = curStrEe;
                        std::memcpy(rdram + curStrEe, child.name.c_str(), child.name.size() + 1u);
                        curStrEe += static_cast<uint32_t>(child.name.size() + 1u);
                    }
                }

                reply.result = totalEntries;
                ps2xSafeLog("[CID1:EnumDir-0x0A] '%s' -> %u entries to 0x%08x (bufSize=%u, totalBytes=%u)\n",
                            path.c_str(), totalEntries, dstEeAddr, bufSize, totalBytes);
                for (uint32_t i = 0; i < totalEntries; ++i)
                {
                    ps2xSafeLog("  [%u] %s%s (size=%llu)\n", i, children[i].name.c_str(),
                                children[i].isDirectory ? "/" : "", (unsigned long long)children[i].fileSize);
                }
                break;
            }
            case 0x0B: // Read path
            {
                uint32_t eeDst = *reinterpret_cast<const uint32_t *>(src + 0x10);
                uint32_t fileOffset = *reinterpret_cast<const uint32_t *>(src + 0x14);
                uint32_t reqSize = *reinterpret_cast<const uint32_t *>(src + 0x18);
                const char *pathPtr = reinterpret_cast<const char *>(src + 0x20);
                std::string path(pathPtr);
                const auto *entry = ps2x::vfi::VfiArchive::instance().find(path);
                if (!entry)
                {
                    ps2xSafeLog( "[CID1:ReadPath] NOT FOUND: '%s'\n", path.c_str());
                    reply.result = static_cast<uint32_t>(-1);
                }
                else
                {
                    uint64_t avail = (entry->fileSize > fileOffset) ? (entry->fileSize - fileOffset) : 0ull;
                    uint64_t toRead = (reqSize == 0u) ? avail : std::min(static_cast<uint64_t>(reqSize), avail);
                    if (eeDst + toRead > PS2_RAM_SIZE)
                    {
                        ps2xSafeLog( "[CID1:ReadPath] ERROR: eeDst 0x%08x + %llu exceeds RAM\n", eeDst, (unsigned long long)toRead);
                        reply.result = static_cast<uint32_t>(-1);
                    }
                    else
                    {
                        if (toRead > 0)
                        {
                            ps2x::vfi::VfiArchive::instance().readFileData(*entry, fileOffset, toRead, rdram + eeDst);
                        }
                        g_diagCid1BytesServed.fetch_add(toRead, std::memory_order_relaxed);
                        diagSetLastCid1Path(path);
                        reply.result = static_cast<uint32_t>(toRead);

                        uint32_t crc = 0xFFFFFFFFu;
                        for (size_t i = 0; i < toRead; ++i)
                        {
                            crc ^= (rdram + eeDst)[i];
                            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                        }
                        crc ^= 0xFFFFFFFFu;

                        ps2xSafeLog( "[CID1:ReadPath] '%s' off=%u reqSize=%u -> read %llu bytes to 0x%08x (CRC32=0x%08X)\n",
                                     path.c_str(), fileOffset, reqSize, (unsigned long long)toRead, eeDst, crc);
                    }
                }
                break;
            }
            case 0x0D: // Mount
            {
                reply.result = 0;
                ps2xSafeLog( "[CID1:Mount] Mount acknowledged\n");
                break;
            }
            default:
            {
                uint32_t flags = (cmdSize >= 0x18) ? *reinterpret_cast<const uint32_t *>(src + 0x14) : 0u;
                ps2xSafeLog( "[cid1] UNIMPLEMENTED cmd=0x%02x flags=0x%x\n", opcode, flags);
                reply.result = static_cast<uint32_t>(-1);
                break;
            }
            }

            if (doTrace)
            {
                ps2xSafeLog( "[CID1:trace reply] destExtra=0x%08x ringTail=0x%08x res=0x%x\n",
                             destExtra, reply.ringTail, reply.result);
                const uint8_t *rptr = reinterpret_cast<const uint8_t *>(&reply);
                std::string repDump;
                char rbuf[8];
                for (size_t i = 0; i < sizeof(reply); ++i) { std::snprintf(rbuf, sizeof(rbuf), "%02x ", rptr[i]); repDump += rbuf; }
                repDump += "\n";
                ps2xSafeLog("%s", repDump.c_str());
            }
            std::fflush(stderr);

            ps2_stubs::dispatchSifCommand(rdram, runtime, 1u, &reply, sizeof(reply));
        }

        // Handle SET_SREG command (0x80000001)
        if (cid == 0x80000001u && packetAddr != 0u)
        {
            if (uint8_t *p = getMemPtr(rdram, packetAddr))
            {
                uint32_t regIndex = 0;
                uint32_t regVal = 0;
                std::memcpy(&regIndex, p + 0x10, 4);
                std::memcpy(&regVal, p + 0x14, 4);

                ps2xSafeLog( "[sceSifSendCmd:SETSREG] index=%u value=0x%x\n", regIndex, regVal);
                std::fflush(stderr);

                if (regIndex < 32)
                {
                    s_iopSregs[regIndex] = regVal;
                    if (uint8_t *sregMem = getMemPtr(rdram, 0x0076A6C0u))
                    {
                        std::memcpy(sregMem + regIndex * 4, &regVal, 4);

                        // Emulate IOP handshake response when EE signals ready on sreg 31
                        if (regIndex == 31)
                        {
                            uint32_t ack = 1;
                            std::memcpy(sregMem + 31 * 4, &ack, 4);
                            uint32_t sreg30 = s_iopSregs[30];
                            if (sreg30 == 0)
                            {
                                sreg30 = 0x80; // file3.irx status
                                s_iopSregs[30] = sreg30;
                            }
                            std::memcpy(sregMem + 30 * 4, &sreg30, 4);
                            ps2xSafeLog( "[sceSifSendCmd:HANDSHAKE] IOP handshake acknowledged: sreg[31]=1 sreg[30]=0x%x\n", sreg30);
                            std::fflush(stderr);
                        }
                    }
                }
            }
        }

        // Return non-zero on success.
        setReturnS32(ctx, 1);
    }

    void sceRpcGetPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t queuePtr = getRegU32(ctx, 4);
        setReturnS32(ctx, static_cast<int32_t>(queuePtr));
    }
}
