#include "runtime/gs/ps2_gif_arbiter.h"
#include "probe_ae.h"
#include "probe_ae.h"
#include "probe_ae.h"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>
#include "ThreadNaming.h" // PERF-MB
#include <intrin.h>
extern void ps2xSafeLog(const char *fmt, ...);
extern std::atomic<uint64_t> g_waitTicks[8]; // PROBE-KF, defined in ps2_memory.cpp

// PERF-GK: optional asynchronous GS (PS2X_GS_ASYNC=1). drain() hands the sorted packets to a worker thread that runs the
// process function (GS rasterization) so the EE/VU1 thread no longer pays for drawing. File-static state only (no header change).
namespace
{
struct GsAsyncState
{
    std::mutex m;
    std::condition_variable cvWork;
    std::condition_variable cvIdle;
    // PERF-GN: wake-ups (NtAlertThreadByThreadId, ~5% of the GS thread) only when somebody can be waiting; both counters are guarded by `m`
    uint32_t idleWaiters = 0;
    uint32_t ticketWaiters = 0; // PERF-GH: waiters that need a wake-up after every packet (present thread waiting for its ticket)
    bool workerWaiting = false;
    std::deque<std::vector<uint8_t>> q;
    size_t queuedBytes = 0;
    uint64_t enqueuedPkts = 0;  // PERF-JF: packets handed to the worker / finished by it (ordering ticket for the async present latch)
    uint64_t processedPkts = 0;
    bool busy = false;
    bool started = false;
    std::function<void(const uint8_t *, uint32_t)> fn;
};

// PERF-GQ: the GS thread is only ~70% busy but the 8 MB queue filled during texture-upload bursts and blocked the VIF1/VU1 worker (and through the VIF1
// fence the EE) for ~25% of the time (gs_backpressure in the [wait] line). The queue is FIFO either way, so a deeper one changes no ordering.
const size_t kGsAsyncMaxBytes = []() {
    const char *e = std::getenv("PS2X_GS_QUEUE_MB");
    const unsigned long mb = e ? std::strtoul(e, nullptr, 10) : 8ul;
    return static_cast<size_t>(mb == 0ul ? 8ul : mb) * 1024u * 1024u;
}();

// PERF-GP: recycled packet buffers (the per-XGKICK vector allocation + free was ~1.7% of the VU1 worker)
struct PacketPool // leaked on purpose like GsAsyncState: the detached GS worker may outlive static destruction
{
    std::mutex m;
    std::vector<std::vector<uint8_t>> b[22]; // PERF-CT: power-of-two capacity buckets (256 B .. 1 MB); the VIF1 worker holds thousands of packets per frame
    size_t bytes = 0;
};
PacketPool &packetPool()
{
    static PacketPool *p = new PacketPool();
    return *p;
}
inline int packetBucket(size_t n) // smallest b >= 8 with 2^b >= n
{
    int b = 8;
    while ((static_cast<size_t>(1) << b) < n)
        ++b;
    return b;
}
std::vector<uint8_t> takePacketBuf(size_t need)
{
    const int b = packetBucket(need);
    if (b > 20)
        return {};
    // PERF-XP: the producer (VIF1 worker) took the pool mutex once per XGKICK while the GS thread recycles under it. Refill a thread-local stash of up to
    // 16 buffers per lock instead; buffers are interchangeable, so nothing observable changes.
    thread_local std::vector<std::vector<uint8_t>> t_stash[22];
    auto &stash = t_stash[b];
    if (stash.empty())
    {
        PacketPool &pool = packetPool();
        std::lock_guard<std::mutex> lock(pool.m);
        auto &v = pool.b[b];
        for (int i = 0; i < 16 && !v.empty(); ++i)
        {
            pool.bytes -= v.back().capacity();
            stash.push_back(std::move(v.back()));
            v.pop_back();
        }
    }
    if (!stash.empty())
    {
        std::vector<uint8_t> r = std::move(stash.back());
        stash.pop_back();
        return r;
    }
    std::vector<uint8_t> r;
    r.reserve(static_cast<size_t>(1) << b);
    return r;
}
void recyclePacketBuf(std::vector<uint8_t> &&v)
{
    const size_t cap = v.capacity();
    if (cap < 256u || cap > (1u << 20))
        return;
    int b = 8;
    while ((static_cast<size_t>(2) << b) <= cap)
        ++b; // floor(log2(cap)): a buffer in bucket b holds at least 2^b bytes
    PacketPool &pool = packetPool();
    std::lock_guard<std::mutex> lock(pool.m);
    if (pool.bytes + cap <= (64u << 20))
    {
        pool.bytes += cap;
        pool.b[b].push_back(std::move(v));
    }
}

GsAsyncState &gsAsync()
{
    static GsAsyncState *s = new GsAsyncState(); // leaked on purpose: detached worker outlives static destruction
    return *s;
}

bool gsAsyncEnabled()
{
    static const bool on = []() { const char *v = std::getenv("PS2X_GS_ASYNC"); return v && v[0] == '1'; }();
    return on;
}

bool gsBatchPopEnabled()
{
    static const bool on = []() {
        const char *v = std::getenv("PS2X_GS_BATCHPOP");
        const bool enabled = v && (v[0] == '1' || (v[0] != '0' && std::strcmp(v, "off") != 0 && std::strcmp(v, "false") != 0 && v[0] != '\0'));
        if (enabled)
        {
            ::ps2xSafeLog("[gsbatch] ON max=64\n");
            std::fflush(stderr);
        }
        return enabled;
    }();
    return on;
}

thread_local std::vector<std::vector<uint8_t>> t_gsRecycleStash;

void flushRecycleStash()
{
    if (t_gsRecycleStash.empty())
        return;
    PacketPool &pool = packetPool();
    std::lock_guard<std::mutex> lock(pool.m);
    for (auto &v : t_gsRecycleStash)
    {
        const size_t cap = v.capacity();
        if (cap < 256u || cap > (1u << 20))
            continue;
        int b = 8;
        while ((static_cast<size_t>(2) << b) <= cap)
            ++b; // floor(log2(cap)): a buffer in bucket b holds at least 2^b bytes
        if (pool.bytes + cap <= (64u << 20))
        {
            pool.bytes += cap;
            pool.b[b].push_back(std::move(v));
        }
    }
    t_gsRecycleStash.clear();
}

// PERF-GB: while the calling thread is in batch mode (the VIF1 worker, around one job) async GIF packets are collected thread-locally and handed to the queue
// under ONE lock at the end of the job (was a lock + possible notify + pool lock per XGKICK). Order is unchanged: single producer, FIFO queue.
thread_local bool t_gsBatch = false;
thread_local std::vector<std::vector<uint8_t>> t_gsPending;
thread_local size_t t_gsPendingBytes = 0;
std::atomic<bool> s_gsAsyncStarted{false};
std::atomic<uint64_t> g_gsqBusy{0}, g_gsqIdle{0}, g_gsqBlocked{0}, g_gsqPeak{0}, g_gsqPkts{0}; // PROBE-GQ

void gsFlushPending()
{
    if (t_gsPending.empty())
        return;
    GsAsyncState &st = gsAsync();
    std::unique_lock<std::mutex> lock(st.m);
    for (auto &d : t_gsPending)
    {
        const uint64_t kfT1 = __rdtsc(); // PROBE-KF
        ++st.idleWaiters;
        st.cvIdle.wait(lock, [&] { return st.queuedBytes < kGsAsyncMaxBytes; }); // backpressure
        --st.idleWaiters;
        if (__rdtsc() - kfT1 > 20000ull) g_gsqBlocked.fetch_add(1, std::memory_order_relaxed); // PROBE-GQ: blocked for more than ~5 us
        g_waitTicks[3].fetch_add(__rdtsc() - kfT1, std::memory_order_relaxed);
        st.queuedBytes += d.size();
        if (st.queuedBytes > g_gsqPeak.load(std::memory_order_relaxed)) g_gsqPeak.store(st.queuedBytes, std::memory_order_relaxed); // PROBE-GQ
        ++st.enqueuedPkts;
        st.q.push_back(std::move(d));
    }
    if (st.workerWaiting)
        st.cvWork.notify_one();
    t_gsPending.clear();
    t_gsPendingBytes = 0;
}

void gsAsyncWorker()
{
    ThreadNaming::SetCurrentThreadName("GsWorker");
    // PROBE-GP: PS2X_PROF_GSWORKER=1 (with PS2X_PROF_SAMPLE=1) points the sampler's line/owner/budget output at this thread
    if (const char *pe = std::getenv("PS2X_PROF_GSWORKER"); pe && pe[0] == '1')
        probe_ae::recordMainGuestThread();
    GsAsyncState &st = gsAsync();
    std::unique_lock<std::mutex> lock(st.m);

    if (!gsBatchPopEnabled())
    {
        for (;;)
        {
            st.workerWaiting = true;
            const uint64_t gqI0 = __rdtsc(); // PROBE-GQ
            st.cvWork.wait(lock, [&] { return !st.q.empty(); });
            g_gsqIdle.fetch_add(__rdtsc() - gqI0, std::memory_order_relaxed);
            st.workerWaiting = false;
            std::vector<uint8_t> pkt = std::move(st.q.front());
            st.q.pop_front();
            st.busy = true;
            lock.unlock();
            const size_t pktSize = pkt.size();
            const uint64_t gqB0 = __rdtsc(); // PROBE-GQ
            st.fn(pkt.data(), static_cast<uint32_t>(pktSize));
            g_gsqBusy.fetch_add(__rdtsc() - gqB0, std::memory_order_relaxed);
            g_gsqPkts.fetch_add(1, std::memory_order_relaxed);
            recyclePacketBuf(std::move(pkt));
            lock.lock();
            st.queuedBytes -= pktSize;
            ++st.processedPkts;
            st.busy = false;
            // PERF-GH: a producer blocked on the byte limit only needs to hear about it once a good part of the queue is drained (it used to be woken after every
            // packet, pushed one packet and blocked again: a context switch per packet while the GS thread was the bottleneck). Fence and ticket waiters keep their wake-ups.
            if (st.idleWaiters != 0u && (st.ticketWaiters != 0u || st.q.empty() || st.queuedBytes <= kGsAsyncMaxBytes / 4u * 3u))
                st.cvIdle.notify_all();
        }
    }

    struct WorkerExitGuard
    {
        ~WorkerExitGuard()
        {
            flushRecycleStash();
        }
    } exitGuard;

    std::vector<std::vector<uint8_t>> batch;
    batch.reserve(64);

    for (;;)
    {
        st.workerWaiting = true;
        const uint64_t gqI0 = __rdtsc(); // PROBE-GQ
        if (st.q.empty() && !t_gsRecycleStash.empty())
        {
            flushRecycleStash();
        }
        st.cvWork.wait(lock, [&] { return !st.q.empty(); });
        const uint64_t idleTicks = __rdtsc() - gqI0;
        st.workerWaiting = false;

        batch.clear();
        size_t batchBytes = 0;
        while (!st.q.empty() && batch.size() < 64)
        {
            batchBytes += st.q.front().size();
            batch.push_back(std::move(st.q.front()));
            st.q.pop_front();
        }
        if (st.q.empty() && !t_gsRecycleStash.empty())
        {
            flushRecycleStash();
        }
        const uint64_t batchCount = static_cast<uint64_t>(batch.size());
        st.busy = true;
        lock.unlock();

        const uint64_t gqB0 = __rdtsc(); // PROBE-GQ
        for (size_t i = 0; i < batch.size(); ++i)
        {
            st.fn(batch[i].data(), static_cast<uint32_t>(batch[i].size()));
        }
        const uint64_t busyTicks = __rdtsc() - gqB0;

        for (size_t i = 0; i < batch.size(); ++i)
        {
            t_gsRecycleStash.push_back(std::move(batch[i]));
        }
        batch.clear();

        if (t_gsRecycleStash.size() >= 32)
        {
            flushRecycleStash();
        }

        g_gsqIdle.fetch_add(idleTicks, std::memory_order_relaxed);
        g_gsqBusy.fetch_add(busyTicks, std::memory_order_relaxed);
        g_gsqPkts.fetch_add(batchCount, std::memory_order_relaxed);

        lock.lock();
        st.queuedBytes -= batchBytes;
        st.processedPkts += batchCount;
        st.busy = false;
        if (st.idleWaiters != 0u && (st.ticketWaiters != 0u || st.q.empty() || st.queuedBytes <= kGsAsyncMaxBytes / 4u * 3u))
            st.cvIdle.notify_all();
    }
    flushRecycleStash();
}
} // namespace

// Wait until every queued GIF packet has been processed (no-op when async is off).
extern "C" void ps2_gs_async_fence()
{
    if (!gsAsyncEnabled())
        return;
    gsFlushPending(); // PERF-GB
    GsAsyncState &st = gsAsync();
    std::unique_lock<std::mutex> lock(st.m);
    if (!st.started)
        return;
    const uint64_t kfT0 = __rdtsc(); // PROBE-KF
    ++st.idleWaiters;
    st.cvIdle.wait(lock, [&] { return st.q.empty() && !st.busy; });
    --st.idleWaiters;
    g_waitTicks[2].fetch_add(__rdtsc() - kfT0, std::memory_order_relaxed);
}

extern "C" void ps2_gs_async_stats(uint64_t *out) // PROBE-GQ: busy, idle ticks, blocked count, peak bytes, packets (all reset on read)
{
    out[0] = g_gsqBusy.exchange(0);
    out[1] = g_gsqIdle.exchange(0);
    out[2] = g_gsqBlocked.exchange(0);
    out[3] = g_gsqPeak.exchange(0);
    out[4] = g_gsqPkts.exchange(0);
}

// PERF-GB: batch mode for the calling thread (VIF1 worker, around one job); turning it off flushes.
extern "C" void ps2_gs_async_batch(bool on)
{
    if (!gsAsyncEnabled())
        return;
    if (!on)
        gsFlushPending();
    t_gsBatch = on;
}

// PERF-JF: ticket = number of packets handed to the async worker so far (0 when async is off / not started).
extern "C" uint64_t ps2_gs_async_ticket()
{
    if (!gsAsyncEnabled())
        return 0;
    GsAsyncState &st = gsAsync();
    std::lock_guard<std::mutex> lock(st.m);
    return st.started ? st.enqueuedPkts : 0;
}

// Wait until the worker has finished the first `ticket` packets (the draws that were queued when the ticket was taken).
extern "C" void ps2_gs_async_wait_ticket(uint64_t ticket)
{
    if (!ticket)
        return;
    gsFlushPending(); // PERF-GB
    GsAsyncState &st = gsAsync();
    std::unique_lock<std::mutex> lock(st.m);
    ++st.idleWaiters;
    ++st.ticketWaiters;
    st.cvIdle.wait(lock, [&] { return st.processedPkts >= ticket; });
    --st.ticketWaiters;
    --st.idleWaiters;
}

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    probe_ae::onBhRecordQueueDepth(static_cast<uint32_t>(m_queue.size()));
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data = takePacketBuf(sizeBytes);
    pkt.data.assign(data, data + sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    if (m_queue.size() > 1u) // PERF-FN: a one-packet queue (the usual XGKICK case) needs no sort
    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    if (gsAsyncEnabled() && t_gsBatch && s_gsAsyncStarted.load(std::memory_order_acquire)) // PERF-GB
    {
        for (size_t i = 0; i < m_queue.size(); ++i)
        {
            auto &pkt = m_queue[i];
            if (pkt.data.empty())
                continue;
            t_gsPendingBytes += pkt.data.size();
            t_gsPending.push_back(std::move(pkt.data));
        }
        m_queue.clear();
        if (t_gsPendingBytes >= (1u << 20))
            gsFlushPending();
        return;
    }

    if (gsAsyncEnabled())
    {
        GsAsyncState &st = gsAsync();
        std::unique_lock<std::mutex> lock(st.m);
        if (!st.started)
        {
            st.started = true;
            st.fn = m_processFn;
            std::thread(gsAsyncWorker).detach();
            s_gsAsyncStarted.store(true, std::memory_order_release); // PERF-GB
        }
        for (size_t i = 0; i < m_queue.size(); ++i)
        {
            auto &pkt = m_queue[i];
            if (pkt.data.empty())
                continue;
            const uint64_t kfT1 = __rdtsc(); // PROBE-KF
            ++st.idleWaiters;
            st.cvIdle.wait(lock, [&] { return st.queuedBytes < kGsAsyncMaxBytes; }); // backpressure
            --st.idleWaiters;
            g_waitTicks[3].fetch_add(__rdtsc() - kfT1, std::memory_order_relaxed);
            st.queuedBytes += pkt.data.size();
            ++st.enqueuedPkts;
            st.q.push_back(std::move(pkt.data));
            if (st.workerWaiting)
                st.cvWork.notify_one();
        }
        m_queue.clear();
        return;
    }

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
