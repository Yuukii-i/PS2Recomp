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
#include <intrin.h>
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
    std::deque<std::vector<uint8_t>> q;
    size_t queuedBytes = 0;
    uint64_t enqueuedPkts = 0;  // PERF-JF: packets handed to the worker / finished by it (ordering ticket for the async present latch)
    uint64_t processedPkts = 0;
    bool busy = false;
    bool started = false;
    std::function<void(const uint8_t *, uint32_t)> fn;
};

constexpr size_t kGsAsyncMaxBytes = 8u * 1024u * 1024u;

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

void gsAsyncWorker()
{
    GsAsyncState &st = gsAsync();
    std::unique_lock<std::mutex> lock(st.m);
    for (;;)
    {
        st.cvWork.wait(lock, [&] { return !st.q.empty(); });
        std::vector<uint8_t> pkt = std::move(st.q.front());
        st.q.pop_front();
        st.busy = true;
        lock.unlock();
        st.fn(pkt.data(), static_cast<uint32_t>(pkt.size()));
        lock.lock();
        st.queuedBytes -= pkt.size();
        ++st.processedPkts;
        st.busy = false;
        st.cvIdle.notify_all();
    }
}
} // namespace

// Wait until every queued GIF packet has been processed (no-op when async is off).
extern "C" void ps2_gs_async_fence()
{
    if (!gsAsyncEnabled())
        return;
    GsAsyncState &st = gsAsync();
    std::unique_lock<std::mutex> lock(st.m);
    if (!st.started)
        return;
    const uint64_t kfT0 = __rdtsc(); // PROBE-KF
    st.cvIdle.wait(lock, [&] { return st.q.empty() && !st.busy; });
    g_waitTicks[2].fetch_add(__rdtsc() - kfT0, std::memory_order_relaxed);
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
    GsAsyncState &st = gsAsync();
    std::unique_lock<std::mutex> lock(st.m);
    st.cvIdle.wait(lock, [&] { return st.processedPkts >= ticket; });
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
    probe_ae::onBhRecordQueueDepth(static_cast<uint32_t>(m_queue.size()));
    probe_ae::onBhRecordQueueDepth(static_cast<uint32_t>(m_queue.size()));
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
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

    if (gsAsyncEnabled())
    {
        GsAsyncState &st = gsAsync();
        std::unique_lock<std::mutex> lock(st.m);
        if (!st.started)
        {
            st.started = true;
            st.fn = m_processFn;
            std::thread(gsAsyncWorker).detach();
        }
        for (size_t i = 0; i < m_queue.size(); ++i)
        {
            auto &pkt = m_queue[i];
            if (pkt.data.empty())
                continue;
            const uint64_t kfT1 = __rdtsc(); // PROBE-KF
            st.cvIdle.wait(lock, [&] { return st.queuedBytes < kGsAsyncMaxBytes; }); // backpressure
            g_waitTicks[3].fetch_add(__rdtsc() - kfT1, std::memory_order_relaxed);
            st.queuedBytes += pkt.data.size();
            ++st.enqueuedPkts;
            st.q.push_back(std::move(pkt.data));
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
