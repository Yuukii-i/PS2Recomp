#include <cstdint>
#include <cstdlib>
void ps2xSafeLog(const char *fmt, ...);
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
namespace ps2_stubs {
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
}
class PS2Runtime;
namespace ps2_syscalls {
    void ps2xUpdateDbcPadBuffers(uint8_t *rdram, PS2Runtime *runtime);
}
#include "runtime/ee_scheduler.h"
#include "probe_ae.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"
#include "Stubs/CD.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;
}

thread_local uint32_t g_currentExecutingFunction = 0u;

namespace
{

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }

    static std::unordered_map<int, EeAlarm> s_diagAlarms;
    static std::unordered_map<int, EeIrqHandler> s_diagIntcHandlers;
    static std::unordered_map<int, EeIrqHandler> s_diagDmacHandlers;
    static std::unordered_map<int, uint64_t> s_diagIntcFireCounts;
    static std::unordered_map<int, uint64_t> s_diagDmacFireCounts;
    static bool isSchedTraceEnabled()
    {
        static int s_enabled = -1;
        if (s_enabled == -1)
        {
            const char *env = std::getenv("PS2X_SCHED_TRACE");
            s_enabled = (env && std::strcmp(env, "1") == 0) ? 1 : 0;
        }
        return s_enabled == 1;
    }

    static const char *threadStatusName(EeThreadStatus s)
    {
        switch (s)
        {
        case EeThreadStatus::Running: return "RUNNING";
        case EeThreadStatus::Ready: return "READY";
        case EeThreadStatus::Waiting: return "WAITING";
        case EeThreadStatus::WaitingSuspended: return "WAIT_SUSP";
        case EeThreadStatus::Suspended: return "SUSPENDED";
        case EeThreadStatus::Dormant: return "DORMANT";
        default: return "UNKNOWN";
        }
    }

    

}

std::string ps2xGetIntcSummary()
    {
        std::string s;
        char b[64];
        for (const auto &pair : s_diagIntcHandlers)
        {
            uint64_t fires = s_diagIntcFireCounts[pair.first];
            std::snprintf(b, sizeof(b), " cause%u(#%d)=%llu", pair.second.cause, pair.first, (unsigned long long)fires);
            s += b;
        }
        return s;
    }

static EeScheduler *s_activeScheduler = nullptr;
static PS2Runtime *s_activeRuntime = nullptr;
static std::atomic<uint32_t> s_diagEnabledIntcMask{0xFFFFFFFFu};

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
    s_activeScheduler = this;
    s_activeRuntime = &m_runtime;
    s_diagEnabledIntcMask.store(0xFFFFFFFFu, std::memory_order_relaxed);
}

EeScheduler::~EeScheduler()
{
    if (s_activeScheduler == this) s_activeScheduler = nullptr;
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    s_activeScheduler = this;
    s_activeRuntime = &m_runtime;
    s_diagEnabledIntcMask.store(0xFFFFFFFFu, std::memory_order_relaxed);
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    s_diagAlarms.clear();
    s_diagIntcHandlers.clear();
    s_diagDmacHandlers.clear();
    s_diagIntcFireCounts.clear();
    s_diagDmacFireCounts.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    probe_ae::recordMainGuestThread();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                if (invocation.kind == GuestInvocationKind::Interrupt) {
                    uint32_t cause = getRegU32(&invocation.context, 4);
                    probe_ae::onCeVblankDelivered(cause);
                    probe_ae::onDc3IrqDelivered(cause, invocation.context.pc);
                }
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        probe_ae::onDc2ThreadDispatch(running->id, running->entry);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (invocation.kind == GuestInvocationKind::Interrupt) {
                uint32_t cause = getRegU32(&invocation.context, 4);
                probe_ae::onCeVblankDelivered(cause);
                probe_ae::onDc3IrqDelivered(cause, invocation.context.pc);
            }
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        static uint32_t s_lastDispatchedPc[32] = {0};
        static uint32_t s_lastDispatchIdx = 0;

        if (!m_runtime.hasFunction(context.pc))
        {
            fprintf(stderr, "[abort] EeScheduler: missing function at pc=0x%08x, ra=0x%08x, sp=0x%08x, threadId=%d, invocations=%zu\n",
                    context.pc,
                    (uint32_t)_mm_extract_epi32(context.r[31], 0),
                    (uint32_t)_mm_extract_epi32(context.r[29], 0),
                    running ? running->id : -999,
                    running ? running->invocations.size() : 0);
            fprintf(stderr, "[abort] Last dispatched PCs by EeScheduler:\n");
            for (int i = 0; i < 32; ++i) {
                uint32_t p = s_lastDispatchedPc[(s_lastDispatchIdx + i) % 32];
                if (p) fprintf(stderr, "  [%d] 0x%08x\n", i, p);
            }
            fflush(stderr);
            m_runtime.reportMissingFunction(m_rdram,
                                            &context,
                                            context.pc,
                                            context.pc,
                                            PS2Runtime::GuestBranchKind::DirectJump,
                                            "EE scheduler");
            std::abort();
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        try
        {
            if (context.pc >= 0x00120000u && context.pc < 0x0013F800u)
            {
                static uint32_t s_range120kHitCount = 0;
                s_range120kHitCount++;
                static bool s_reported120k = false;
                if (!s_reported120k)
                {
                    s_reported120k = true;
                    fprintf(stderr, "[region-0x120000] First execution in 0x120000..0x13F800 at pc=0x%08x (total hits: %u)\n",
                            context.pc, s_range120kHitCount);
                    fflush(stderr);
                }
                if (ps2_stubs::g_hasReadInto120000.load(std::memory_order_acquire))
                {
                    uint32_t count = ++ps2_stubs::g_postRead120000ExecCount;
                    static bool s_reportedPostRead = false;
                    if (!s_reportedPostRead)
                    {
                        s_reportedPostRead = true;
                        fprintf(stderr, "[region-0x120000:post-read] First execution in 0x120000..0x13F800 post-sceCdRead at pc=0x%08x (count=%u)\n",
                                context.pc, count);
                        fflush(stderr);
                    }
                }
            }

            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            const uint32_t irqCause = m_insideInterrupt ? getRegU32(&context, 4) : 0u;
            m_guestExecuting.store(true, std::memory_order_release);
            const uint32_t entryPc = context.pc;
            if (m_insideInterrupt || (!running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::SifCommand)) {
                probe_ae::logIntcHandlerDispatch(entryPc);
            }
            const uint32_t prevExecutingFunction = g_currentExecutingFunction;
            g_currentExecutingFunction = entryPc;
            s_lastDispatchedPc[s_lastDispatchIdx++ % 32] = entryPc;
            if (running && running->id == 1 && probe_ae::isEnabled())
            {
                probe_ae::recordMainThreadDispatch(entryPc);
            }
            auto tFunc0 = std::chrono::steady_clock::now();
            function(m_rdram, &context, &m_runtime);
            auto tFunc1 = std::chrono::steady_clock::now();
            if (m_insideInterrupt)
            {
                uint64_t irqUs = std::chrono::duration_cast<std::chrono::microseconds>(tFunc1 - tFunc0).count();
                probe_ae::onBhIntcHandler(irqCause, irqUs);
                if (probe_ae::isEnabled())
                {
                    uint32_t ret = getRegU32(&context, 2);
                    probe_ae::logIntcHandlerReturn(entryPc, ret);
                }
            }
            g_currentExecutingFunction = prevExecutingFunction;
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    uint32_t pc = g_currentExecutingFunction;
    if (m_currentThreadId != 0)
    {
        const GuestThread *running = currentThread();
        if (running != nullptr)
        {
            pc = running->activeContext().pc;
        }
    }
    if ((pc >= 0x003D7000u && pc <= 0x003E5000u) ||
        (g_currentExecutingFunction >= 0x003D7000u && g_currentExecutingFunction <= 0x003E5000u))
    {
        static thread_local uint32_t s_throttleCount = 0;
        if ((++s_throttleCount & 0x3FFu) != 0)
        {
            return false;
        }
    }

    if (m_currentThreadId == kMainThreadId && (pc == 0x0010384Cu || (g_currentExecutingFunction == 0x001037E8u && pc >= 0x00103960u)))
    {
        accountCycles(kVBlankPeriodCycles / 4);
        processPendingEvents();

        return false;
    }

    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    if (isSchedTraceEnabled())
    {
        const GuestThread *self = currentThread();
        uint32_t pc = self ? self->activeContext().pc : 0;
        uint32_t ra = self ? getRegU32(&self->activeContext(), 31) : 0;
        ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=CreateThread thid=%d entry=0x%08x stack=0x%08x stackSize=%u prio=%d\n",
                    (unsigned long long)m_vsyncTick, pc, ra, id, params.entry, params.stack, params.stackSize, params.priority);
    }
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    if (isSchedTraceEnabled())
    {
        uint32_t pc = caller.pc;
        uint32_t ra = getRegU32(&caller, 31);
        ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=StartThread target=%d state=DORMANT->READY wakeups=0 intSafe=%d\n",
                    (unsigned long long)m_vsyncTick, pc, ra, id, interruptSafe ? 1 : 0);
    }
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    if (isSchedTraceEnabled())
    {
        const uint32_t pc = exiting->activeContext().pc;
        const uint32_t ra = getRegU32(&exiting->activeContext(), 31);
        ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=ExitThread thid=%d state=%s->DORMANT\n",
                    (unsigned long long)m_vsyncTick, pc, ra, id, threadStatusName(exiting->status));
    }
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    const uint32_t pc = self->activeContext().pc;
    const uint32_t ra = getRegU32(&self->activeContext(), 31);
    const EeThreadStatus statusBefore = self->status;
    const uint32_t wakeBefore = self->wakeupCount;
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        if (isSchedTraceEnabled())
        {
            ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=SleepThread(ConsumedWakeup) thid=%d state=%s->%s wakeups=%u->%u\n",
                        (unsigned long long)m_vsyncTick, pc, ra, self->id,
                        threadStatusName(statusBefore), threadStatusName(self->status),
                        wakeBefore, self->wakeupCount);
        }
        return;
    }
    if (isSchedTraceEnabled())
    {
        ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=SleepThread(Blocked) thid=%d state=%s->WAITING wakeups=%u->0\n",
                    (unsigned long long)m_vsyncTick, pc, ra, self->id,
                    threadStatusName(statusBefore), wakeBefore);
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    const GuestThread *self = currentThread();
    const uint32_t pc = self ? self->activeContext().pc : 0;
    const uint32_t ra = self ? getRegU32(&self->activeContext(), 31) : 0;
    const EeThreadStatus statusBefore = target->status;
    const uint32_t wakeBefore = target->wakeupCount;
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    if (isSchedTraceEnabled())
    {
        ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=%s target=%d state=%s->%s wakeups=%u->%u intSafe=%d\n",
                    (unsigned long long)m_vsyncTick, pc, ra,
                    interruptSafe ? "iWakeupThread" : "WakeupThread",
                    id, threadStatusName(statusBefore), threadStatusName(target->status),
                    wakeBefore, target->wakeupCount, interruptSafe ? 1 : 0);
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    if (isSchedTraceEnabled())
    {
        const GuestThread *self = currentThread();
        uint32_t pc = self ? self->activeContext().pc : 0;
        uint32_t ra = self ? getRegU32(&self->activeContext(), 31) : 0;
        ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=CancelWakeup target=%d wakeups=%d->0\n",
                    (unsigned long long)m_vsyncTick, pc, ra, id, old);
    }
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    static int s_lastRotPrio = -1;
    static int s_lastRotThid = -1;
    static uint64_t s_rotCount = 0;
    s_rotCount++;
    const int curThid = m_currentThreadId;
    if (isSchedTraceEnabled())
    {
        if (priority != s_lastRotPrio || curThid != s_lastRotThid || (s_rotCount % 300) == 0)
        {
            s_lastRotPrio = priority;
            s_lastRotThid = curThid;
            const GuestThread *self = currentThread();
            uint32_t pc = self ? self->activeContext().pc : 0;
            uint32_t ra = self ? getRegU32(&self->activeContext(), 31) : 0;
            ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=%s targetPrio=%d curThid=%d totalRotates=%llu\n",
                        (unsigned long long)m_vsyncTick, pc, ra,
                        interruptSafe ? "iRotateReadyQueue" : "RotateReadyQueue",
                        priority, curThid, (unsigned long long)s_rotCount);
        }
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    const int oldCount = object->count++;
    if (isSchedTraceEnabled())
    {
        const GuestThread *self = currentThread();
        uint32_t pc = self ? self->activeContext().pc : 0;
        uint32_t ra = self ? getRegU32(&self->activeContext(), 31) : 0;
        ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=%s semId=%d count=%d->%d\n",
                    (unsigned long long)m_vsyncTick, pc, ra, interruptSafe ? "iSignalSema" : "SignalSema", id, oldCount, object->count);
    }
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    const uint32_t prevBits = flag->bits;
    flag->bits |= bits;
    if (isSchedTraceEnabled())
    {
        const GuestThread *self = currentThread();
        uint32_t pc = self ? self->activeContext().pc : 0;
        uint32_t ra = self ? getRegU32(&self->activeContext(), 31) : 0;
        ps2xSafeLog("[sched:trace] tick=%llu callerPc=0x%08x ra=0x%08x op=%s evfId=%d bits=0x%08x flagBits=0x%08x->0x%08x\n",
                    (unsigned long long)m_vsyncTick, pc, ra, interruptSafe ? "iSetEventFlag" : "SetEventFlag", id, bits, prevBits, flag->bits);
    }
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    s_diagAlarms[id] = EeAlarm{id, ticks, handler, argument, gp, sp};
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    s_diagAlarms.erase(id);
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    EeIrqHandler irqHandler{id,
                            cause,
                            handler,
                            argument,
                            gp,
                            sp,
                            true,
                            append ? ++tail : --head};
    handlers.emplace(id, irqHandler);
    if (dmac) s_diagDmacHandlers[id] = irqHandler;
    else s_diagIntcHandlers[id] = irqHandler;
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    if (dmac) s_diagDmacHandlers.erase(id);
    else s_diagIntcHandlers.erase(id);
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    if (dmac)
    {
        auto dit = s_diagDmacHandlers.find(id);
        if (dit != s_diagDmacHandlers.end()) dit->second.enabled = enabled;
    }
    else
    {
        auto iit = s_diagIntcHandlers.find(id);
        if (iit != s_diagIntcHandlers.end()) iit->second.enabled = enabled;
    }
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
        if (!dmac) {
            s_diagEnabledIntcMask.store(mask, std::memory_order_relaxed);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
            if (dmac) s_diagDmacFireCounts[id]++;
            else s_diagIntcFireCounts[id]++;
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    // Task CE: coalesce VBLANK interrupts (PS2X_VTIME); the pending flag is only set when an invocation is really queued
    if (!dmac && (cause == 2u || cause == 3u) && !matching.empty())
    {
        if (cause == 3u) probe_ae::onDc2VBlankProduced();
        const bool accepted = probe_ae::onCeVblankProduced(cause);
        probe_ae::onDc3IrqProduced(cause, accepted);
        if (!accepted)
        {
            return;
        }
    }
    if (!dmac && cause == 3u && probe_ae::isEnabled())
    {
        std::vector<uint32_t> addrs;
        for (const auto &h : matching) addrs.push_back(h.handler);
        probe_ae::logCause3Dispatch(addrs);
    }
    if (!dmac && (cause == 3u || cause == 5u) && probe_ae::isEnabled())
    {
        probe_ae::recordBvIntcDelivery(cause);
    }
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        probe_ae::onDc2ThreadWake(waiter->id);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

static uint64_t s_bvMainWaitStartUs = 0;

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->id == kMainThreadId && probe_ae::isEnabled()) {
        s_bvMainWaitStartUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.id == kMainThreadId && probe_ae::isEnabled()) {
        uint64_t startUs = s_bvMainWaitStartUs;
        if (startUs != 0) {
            uint64_t nowUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            double dur_ms = (nowUs > startUs) ? (double)(nowUs - startUs) / 1000.0 : 0.0;
            s_bvMainWaitStartUs = 0;
            int kind = (int)item.wait.reason;
            int id = 0;
            if (auto *sw = std::get_if<EeSemaphoreWait>(&item.wait.payload)) {
                id = sw->id;
            } else if (auto *ew = std::get_if<EeEventFlagWait>(&item.wait.payload)) {
                id = ew->id;
            }
            uint32_t ra = getRegU32(&item.activeContext(), 31);
            probe_ae::recordBvMainWait(kind, id, ra, dur_ms);
        }
    }
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        {
            std::unique_lock lock(m_eventMutex);
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle; });
            if (firstFuture == m_deadlines.begin())
            {
                updateNextDeadline();
                return;
            }
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                const auto now = std::chrono::steady_clock::now();
                const double lateMs = std::chrono::duration<double, std::milli>(now - scheduled.hostDeadline).count();
                probe_ae::onDc2VBlankStart(lateMs);
                auto nextHost = scheduled.hostDeadline + kVBlankPeriod;
                if (nextHost < now || nextHost > now + kVBlankPeriod * 2)
                {
                    nextHost = now + kVBlankPeriod;
                }
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              nextHost - kVBlankPeriod + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              nextHost,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);

        // GameThread pad injection
        {
            struct PadInj {
                std::string button;
                uint16_t mask = 0;
                uint64_t startMs = 0;
                uint64_t durMs = 0;
                bool active = false;
            };
            static std::vector<PadInj> s_injections;
            static bool s_parsed = false;
            static auto s_bootTime = std::chrono::steady_clock::now();
            if (!s_parsed) {
                s_parsed = true;
                const char *env = std::getenv("PS2X_INJECT_PAD");
                if (env && env[0] != '\0') {
                    std::string str(env);
                    size_t pos = 0;
                    while (pos < str.size()) {
                        size_t comma = str.find(',', pos);
                        std::string item = str.substr(pos, (comma == std::string::npos) ? std::string::npos : (comma - pos));
                        pos = (comma == std::string::npos) ? str.size() : (comma + 1);
                        size_t c1 = item.find(':');
                        size_t c2 = (c1 != std::string::npos) ? item.find(':', c1 + 1) : std::string::npos;
                        if (c1 != std::string::npos && c2 != std::string::npos) {
                            PadInj inj;
                            inj.button = item.substr(0, c1);
                            try {
                                inj.startMs = std::stoull(item.substr(c1 + 1, c2 - c1 - 1));
                                inj.durMs = std::stoull(item.substr(c2 + 1));
                            } catch (...) { continue; }
                            if (inj.button == "START") inj.mask = 1u << 3;
                            else if (inj.button == "CROSS") inj.mask = 1u << 14;
                            else if (inj.button == "CIRCLE") inj.mask = 1u << 13;
                            else if (inj.button == "TRIANGLE") inj.mask = 1u << 12;
                            else if (inj.button == "SQUARE") inj.mask = 1u << 15;
                            if (inj.mask != 0) s_injections.push_back(inj);
                        }
                    }
                }
            }

            uint16_t activeMask = 0;
            if (!s_injections.empty()) {
                const uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_bootTime).count();
                for (auto &inj : s_injections) {
                    bool shouldBeActive = (nowMs >= inj.startMs && nowMs < (inj.startMs + inj.durMs));
                    if (shouldBeActive != inj.active) {
                        inj.active = shouldBeActive;
                        ps2xSafeLog("[pad:inject] %s %s at %llums (vsync=%llu)\n",
                                    inj.button.c_str(), shouldBeActive ? "DOWN" : "UP",
                                    (unsigned long long)nowMs, (unsigned long long)m_vsyncTick);
                        std::fflush(stderr);
                    }
                    if (inj.active) activeMask |= inj.mask;
                }
            } else {
                extern std::atomic<uint16_t> g_hostPadButtons;
                static int s_autoPad = -1;
                if (s_autoPad == -1) {
                    const char *env = std::getenv("PS2X_AUTO_PAD");
                    s_autoPad = (env && (env[0] == '1' || env[0] == 't')) ? 1 : 0;
                }
                if (s_autoPad && m_vsyncTick >= 5) {
                    const uint64_t phase = (m_vsyncTick / 10) % 3;
                    const bool press = ((m_vsyncTick % 10) < 5);
                    if (press) {
                        if (phase == 0) activeMask |= (1u << 14); // CROSS
                        else if (phase == 1) activeMask |= (1u << 13); // CIRCLE
                        else activeMask |= (1u << 3); // START
                    }
                    static uint64_t s_lastLogVsync = 0;
                    if (isVerboseLog() && m_vsyncTick != s_lastLogVsync && (m_vsyncTick % 60) == 0) {
                        s_lastLogVsync = m_vsyncTick;
                        ps2xSafeLog("[pad:inject:auto] vsync=%llu phase=%llu press=%d btn=0x%04x\n",
                                    (unsigned long long)m_vsyncTick, (unsigned long long)phase, press ? 1 : 0, activeMask);
                        std::fflush(stderr);
                    }
                } else {
                    activeMask = g_hostPadButtons.load(std::memory_order_acquire);
                }
            }

            if (activeMask != 0) {
                ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~activeMask), 0x80, 0x80, 0x80, 0x80);
            } else {
                ps2_stubs::setPadOverrideState(0xFFFFu, 0x80, 0x80, 0x80, 0x80);
            }

            ps2_syscalls::ps2xUpdateDbcPadBuffers(m_rdram, &m_runtime);
        }
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        s_diagAlarms.erase(static_cast<int>(event.id));
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        auto tWait0 = std::chrono::steady_clock::now();
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        auto tWait1 = std::chrono::steady_clock::now();
        uint64_t wUs = std::chrono::duration_cast<std::chrono::microseconds>(tWait1 - tWait0).count();
        probe_ae::onBhSchedWait(wUs);
        probe_ae::onBhSleepSiteHit(1, wUs);
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    auto tWaitUntil0 = std::chrono::steady_clock::now();
    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    auto tWaitUntil1 = std::chrono::steady_clock::now();
    uint64_t wUntilUs = std::chrono::duration_cast<std::chrono::microseconds>(tWaitUntil1 - tWaitUntil0).count();
    probe_ae::onBhSchedWait(wUntilUs);
    probe_ae::onBhSleepSiteHit(2, wUntilUs);
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}

void dumpEeDiagnostics(const EeScheduler *scheduler, uint32_t stuckPc)
{
    if (!scheduler) return;
    extern std::mutex &ps2xLogMutex();
    std::lock_guard<std::mutex> lock(ps2xLogMutex());
    std::fprintf(stderr, "\n=== EE DIAGNOSTICS DUMP (Stuck at PC 0x%08X) ===\n", stuckPc);

    EeKernelSnapshot snap = scheduler->snapshot();
    std::fprintf(stderr, "--- Threads (running=%d, total=%zu) ---\n", snap.runningThreadId, snap.threads.size());
    for (const auto &th : snap.threads)
    {
        const char *statusStr = "UNKNOWN";
        switch (th.status)
        {
        case EeThreadStatus::Running: statusStr = "RUNNING"; break;
        case EeThreadStatus::Ready: statusStr = "READY"; break;
        case EeThreadStatus::Waiting: statusStr = "WAITING"; break;
        case EeThreadStatus::WaitingSuspended: statusStr = "WAITING_SUSPENDED"; break;
        case EeThreadStatus::Suspended: statusStr = "SUSPENDED"; break;
        case EeThreadStatus::Dormant: statusStr = "DORMANT"; break;
        }
        const char *waitStr = "None";
        switch (th.waitReason)
        {
        case EeWaitReason::None: waitStr = "None"; break;
        case EeWaitReason::Sleep: waitStr = "Sleep"; break;
        case EeWaitReason::Semaphore: waitStr = "Semaphore"; break;
        case EeWaitReason::EventFlag: waitStr = "EventFlag"; break;
        case EeWaitReason::VSync: waitStr = "VSync"; break;
        case EeWaitReason::External: waitStr = "External"; break;
        case EeWaitReason::Mpeg: waitStr = "Mpeg"; break;
        }
        std::fprintf(stderr, "  Thread id=%d prio=%d(init=%d) status=%s pc=0x%08X ra=0x%08X sp=0x%08X wait=%s (id=%d) invocations=%u\n",
                     th.id, th.currentPriority, th.initialPriority, statusStr, th.pc, th.ra, th.sp, waitStr, th.waitId, th.invocationDepth);
    }

    std::fprintf(stderr, "--- Semaphores (total=%zu) ---\n", snap.semaphores.size());
    for (const auto &s : snap.semaphores)
    {
        const EeSemaphore *sem = scheduler->semaphore(s.id);
        std::fprintf(stderr, "  Sema id=%d count=%d max=%d waiters=%u [", s.id, s.count, s.maxCount, s.waiters);
        if (sem)
        {
            for (size_t i = 0; i < sem->waiters.size(); ++i)
            {
                std::fprintf(stderr, "%s%d", (i > 0 ? ", " : ""), sem->waiters[i]);
            }
        }
        std::fprintf(stderr, "]\n");
    }

    std::fprintf(stderr, "--- Event Flags (total=%zu) ---\n", snap.eventFlags.size());
    for (const auto &ef : snap.eventFlags)
    {
        const EeEventFlag *flag = scheduler->eventFlag(ef.id);
        std::fprintf(stderr, "  EventFlag id=%d bits=0x%08X init=0x%08X waiters=%u [", ef.id, ef.bits, ef.initBits, ef.waiters);
        if (flag)
        {
            for (size_t i = 0; i < flag->waiters.size(); ++i)
            {
                std::fprintf(stderr, "%s%d", (i > 0 ? ", " : ""), flag->waiters[i]);
            }
        }
        std::fprintf(stderr, "]\n");
    }

    std::fprintf(stderr, "--- Alarms (total=%zu) ---\n", s_diagAlarms.size());
    for (const auto &pair : s_diagAlarms)
    {
        std::fprintf(stderr, "  Alarm id=%d ticks=%u handler=0x%08X arg=0x%08X\n",
                     pair.first, pair.second.ticks, pair.second.handler, pair.second.argument);
    }

    std::fprintf(stderr, "--- INTC Handlers (total=%zu) ---\n", s_diagIntcHandlers.size());
    for (const auto &pair : s_diagIntcHandlers)
    {
        uint64_t fires = s_diagIntcFireCounts[pair.first];
        std::fprintf(stderr, "  INTC id=%d cause=%u handler=0x%08X enabled=%d fires=%llu\n",
                     pair.first, pair.second.cause, pair.second.handler, pair.second.enabled, (unsigned long long)fires);
    }

    std::fprintf(stderr, "--- DMAC Handlers (total=%zu) ---\n", s_diagDmacHandlers.size());
    for (const auto &pair : s_diagDmacHandlers)
    {
        uint64_t fires = s_diagDmacFireCounts[pair.first];
        std::fprintf(stderr, "  DMAC id=%d cause=%u handler=0x%08X enabled=%d fires=%llu\n",
                     pair.first, pair.second.cause, pair.second.handler, pair.second.enabled, (unsigned long long)fires);
    }
    std::fprintf(stderr, "===============================================\n\n");
    std::fflush(stderr);
}

namespace probe_ae {
    
    void dumpTaskDkInfo() {
        if (!s_activeScheduler) return;
        EeKernelSnapshot snap = s_activeScheduler->snapshot();

        // 1. Threads
        for (const auto &thSnap : snap.threads) {
            const GuestThread *th = s_activeScheduler->thread(thSnap.id);
            const char *stateStr = "unknown";
            switch (thSnap.status) {
                case EeThreadStatus::Running: stateStr = "RUNNING"; break;
                case EeThreadStatus::Ready: stateStr = "READY"; break;
                case EeThreadStatus::Waiting: stateStr = "WAIT"; break;
                case EeThreadStatus::Suspended: stateStr = "SUSPEND"; break;
                case EeThreadStatus::WaitingSuspended: stateStr = "WAIT_SUSP"; break;
                case EeThreadStatus::Dormant: stateStr = "DORMANT"; break;
            }

            char waitBuf[128] = "none";
            if (thSnap.status == EeThreadStatus::Waiting || thSnap.status == EeThreadStatus::WaitingSuspended) {
                switch (thSnap.waitReason) {
                    case EeWaitReason::Sleep: std::snprintf(waitBuf, sizeof(waitBuf), "sleep"); break;
                    case EeWaitReason::Semaphore:
                        std::snprintf(waitBuf, sizeof(waitBuf), "sema_%d", thSnap.waitId);
                        break;
                    case EeWaitReason::EventFlag:
                        if (th) {
                            if (auto *w = std::get_if<EeEventFlagWait>(&th->wait.payload))
                                std::snprintf(waitBuf, sizeof(waitBuf), "ef_%d:0x%X:%u", w->id, w->bits, w->mode);
                            else
                                std::snprintf(waitBuf, sizeof(waitBuf), "ef_%d", thSnap.waitId);
                        } else {
                            std::snprintf(waitBuf, sizeof(waitBuf), "ef_%d", thSnap.waitId);
                        }
                        break;
                    case EeWaitReason::VSync:
                        if (th) {
                            if (auto *w = std::get_if<EeVSyncWait>(&th->wait.payload))
                                std::snprintf(waitBuf, sizeof(waitBuf), "vsync_%llu", (unsigned long long)w->afterTick);
                            else
                                std::snprintf(waitBuf, sizeof(waitBuf), "vsync");
                        } else {
                            std::snprintf(waitBuf, sizeof(waitBuf), "vsync");
                        }
                        break;
                    default:
                        std::snprintf(waitBuf, sizeof(waitBuf), "other_%u", (unsigned)thSnap.waitReason);
                        break;
                }
            }

            uint32_t pc = thSnap.pc;
            uint32_t ra = thSnap.ra;
            uint32_t wakeups = th ? th->wakeupCount : 0;
            std::printf("[probe] dc7 th id=%d entry=0x%08X prio=%d status=%s wait=%s pc=0x%08X ra=0x%08X wakeups=%u\n",
                         thSnap.id, thSnap.entry, thSnap.currentPriority, stateStr, waitBuf, pc, ra, wakeups);
        }

        // 2. Semaphores
        for (const auto &semSnap : snap.semaphores) {
            std::string waitersStr;
            const EeSemaphore *sem = s_activeScheduler->semaphore(semSnap.id);
            if (sem) {
                for (size_t i = 0; i < sem->waiters.size(); ++i) {
                    if (i > 0) waitersStr += ",";
                    waitersStr += std::to_string(sem->waiters[i]);
                }
            }
            std::printf("[probe] dc7 sema id=%d count=%d max=%d waiters=%s\n",
                         semSnap.id, semSnap.count, semSnap.maxCount, waitersStr.c_str());
        }

        // 3. Event flags
        for (const auto &efSnap : snap.eventFlags) {
            std::string waitersStr;
            const EeEventFlag *ef = s_activeScheduler->eventFlag(efSnap.id);
            if (ef) {
                for (size_t i = 0; i < ef->waiters.size(); ++i) {
                    int waiterId = ef->waiters[i];
                    if (i > 0) waitersStr += " ";
                    waitersStr += std::to_string(waiterId);
                    const GuestThread *th = s_activeScheduler->thread(waiterId);
                    if (th) {
                        if (auto *w = std::get_if<EeEventFlagWait>(&th->wait.payload)) {
                            char buf[64];
                            std::snprintf(buf, sizeof(buf), ":0x%X:%u", w->bits, w->mode);
                            waitersStr += buf;
                        }
                    }
                }
            }
            std::printf("[probe] dc7 ef id=%d bits=0x%08X waiters=%s\n",
                         efSnap.id, efSnap.bits, waitersStr.c_str());
        }
    }
} // namespace probe_ae

extern "C" void ps2_dumpDkSchedulerState() {
    probe_ae::dumpTaskDkInfo();
}
namespace probe_ae {

    void dumpTaskASInfo(uint32_t sec, bool isFirstStuckSec) {
        if (!isEnabled() || !s_activeScheduler) return;

        EeKernelSnapshot snap = s_activeScheduler->snapshot();

        // 1. Thread state dump (cap 12 threads)
        uint32_t threadLinesPrinted = 0;
        for (const auto &thSnap : snap.threads) {
            if (threadLinesPrinted >= 12) break;
            const GuestThread *th = s_activeScheduler->thread(thSnap.id);

            const char *stateStr = "unknown";
            switch (thSnap.status) {
                case EeThreadStatus::Running: stateStr = "run"; break;
                case EeThreadStatus::Ready: stateStr = "ready"; break;
                case EeThreadStatus::Waiting: stateStr = "wait"; break;
                case EeThreadStatus::Suspended: stateStr = "suspend"; break;
                case EeThreadStatus::WaitingSuspended: stateStr = "waitsuspend"; break;
                case EeThreadStatus::Dormant: stateStr = "dormant"; break;
            }

            const char *waitTypeStr = "none";
            int waitVal = 0;
            switch (thSnap.waitReason) {
                case EeWaitReason::None: waitTypeStr = "none"; break;
                case EeWaitReason::Sleep: waitTypeStr = "sleep"; break;
                case EeWaitReason::Semaphore: {
                    waitTypeStr = "sema";
                    const EeSemaphore *sem = s_activeScheduler->semaphore(thSnap.waitId);
                    if (sem) waitVal = sem->count;
                    break;
                }
                case EeWaitReason::EventFlag: {
                    waitTypeStr = "event";
                    const EeEventFlag *evf = s_activeScheduler->eventFlag(thSnap.waitId);
                    if (evf) waitVal = static_cast<int>(evf->bits);
                    break;
                }
                case EeWaitReason::VSync: waitTypeStr = "vsync"; break;
                case EeWaitReason::External: waitTypeStr = "ext"; break;
                case EeWaitReason::Mpeg: waitTypeStr = "mpeg"; break;
            }

            uint32_t a0 = 0, v0 = 0;
            if (th) {
                const R5900Context &ctx = th->activeContext();
                a0 = getRegU32(&ctx, 4);
                v0 = getRegU32(&ctx, 2);
            }

            std::printf("[probe] as stuck t=%u thread=%d state=%s waitType=%s waitId=%d waitVal=%d prio=%d entry=0x%08X pc=0x%08X ra=0x%08X sp=0x%08X a0=0x%08X v0=0x%08X\n",
                        sec, thSnap.id, stateStr, waitTypeStr, thSnap.waitId, waitVal, thSnap.currentPriority, thSnap.entry, thSnap.pc, thSnap.ra, thSnap.sp, a0, v0);
            threadLinesPrinted++;
        }

        // 2. Main thread function ring
        std::string ringStr = getRecentMainDispatches();
        std::printf("[probe] as ring main=[%s]\n", ringStr.c_str());

        // 3. One-time semaphore dump if first stuck sec (cap 10 semas)
        if (isFirstStuckSec) {
            uint32_t semLinesPrinted = 0;
            for (const auto &semSnap : snap.semaphores) {
                if (semLinesPrinted >= 10) break;
                if (semSnap.waiters == 0) continue;
                const EeSemaphore *sem = s_activeScheduler->semaphore(semSnap.id);

                std::string waitersStr;
                if (sem) {
                    for (size_t i = 0; i < sem->waiters.size(); ++i) {
                        char wb[16];
                        std::snprintf(wb, sizeof(wb), "%s%d", (i > 0 ? "," : ""), sem->waiters[i]);
                        waitersStr += wb;
                    }
                }

                uint32_t sigRa = 0, waitRa = 0;
                getSemaLastRas(semSnap.id, &sigRa, &waitRa);

                std::printf("[probe] as sema id=%d count=%d max=%d waiters=[%s] lastSignalRa=0x%08X lastWaitRa=0x%08X\n",
                            semSnap.id, semSnap.count, semSnap.maxCount, waitersStr.c_str(), sigRa, waitRa);
                semLinesPrinted++;
            }
        }
        std::fflush(stdout);
    }

    void dumpTaskATInfo(uint32_t sec, const uint8_t *rdram) {
        if (!isEnabled() || !s_activeScheduler) return;
        if (!rdram) rdram = s_activeScheduler->rdram();
        if (!rdram) return;

        const GuestThread *th = s_activeScheduler->thread(1); // main thread
        if (!th) return;
        const R5900Context &ctx = th->activeContext();

        uint32_t sp = getRegU32(&ctx, 29);
        uint32_t pc = ctx.pc;
        uint32_t ra = getRegU32(&ctx, 31);
        uint32_t gp = getRegU32(&ctx, 28);
        uint32_t fp = getRegU32(&ctx, 30);
        uint32_t v0 = getRegU32(&ctx, 2);
        uint32_t v1 = getRegU32(&ctx, 3);
        uint32_t a0 = getRegU32(&ctx, 4);
        uint32_t a1 = getRegU32(&ctx, 5);
        uint32_t a2 = getRegU32(&ctx, 6);
        uint32_t a3 = getRegU32(&ctx, 7);
        uint32_t t0 = getRegU32(&ctx, 8);
        uint32_t t1 = getRegU32(&ctx, 9);
        uint32_t t2 = getRegU32(&ctx, 10);
        uint32_t t3 = getRegU32(&ctx, 11);
        uint32_t t4 = getRegU32(&ctx, 12);
        uint32_t t5 = getRegU32(&ctx, 13);
        uint32_t t6 = getRegU32(&ctx, 14);
        uint32_t t7 = getRegU32(&ctx, 15);
        uint32_t s0 = getRegU32(&ctx, 16);
        uint32_t s1 = getRegU32(&ctx, 17);
        uint32_t s2 = getRegU32(&ctx, 18);
        uint32_t s3 = getRegU32(&ctx, 19);
        uint32_t s4 = getRegU32(&ctx, 20);
        uint32_t s5 = getRegU32(&ctx, 21);
        uint32_t s6 = getRegU32(&ctx, 22);
        uint32_t s7 = getRegU32(&ctx, 23);

        std::printf("[probe] at trigger t=%u stuck_main pc=0x%08X sp=0x%08X\n", sec, pc, sp);

        // 1. Stack walk (scan 0x600 bytes upward, cap 30 lines)
        uint32_t retLinesPrinted = 0;
        for (uint32_t off = 0; off <= 0x600; off += 4) {
            if (retLinesPrinted >= 30) break;
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
                        // JAL target
                        uint32_t callee = ((w - 4) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
                        std::printf("[probe] at ret w=0x%08X at sp+0x%X callee=0x%08X\n", w, off, callee);
                        retLinesPrinted++;
                    } else if ((insn & 0xFC00003Fu) == 0x00000009u) {
                        // JALR
                        std::printf("[probe] at ret w=0x%08X at sp+0x%X callee=jalr\n", w, off);
                        retLinesPrinted++;
                    }
                }
            }
        }

        // 2. Dump registers
        std::printf("[probe] at regs pc=0x%08X ra=0x%08X sp=0x%08X gp=0x%08X fp=0x%08X "
                    "v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X "
                    "t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X t4=0x%08X t5=0x%08X t6=0x%08X t7=0x%08X "
                    "s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X s5=0x%08X s6=0x%08X s7=0x%08X\n",
                    pc, ra, sp, gp, fp,
                    v0, v1, a0, a1, a2, a3,
                    t0, t1, t2, t3, t4, t5, t6, t7,
                    s0, s1, s2, s3, s4, s5, s6, s7);

        // 3. Hexdump 64 bytes at s1, s3, s5, a0
        auto dumpMem = [&](const char *name, uint32_t addr) {
            if (addr < 0x00100000u || addr >= 0x02000000u) return;
            if ((addr & PS2_RAM_MASK) + 64 > PS2_RAM_SIZE) return;
            const uint8_t *p = rdram + (addr & PS2_RAM_MASK);

            char hexBuf[129] = {0};
            for (int i = 0; i < 64; ++i) {
                std::snprintf(hexBuf + i * 2, 3, "%02x", p[i]);
            }
            std::printf("[probe] at mem %s=0x%08X : %s\n", name, addr, hexBuf);

            int printableCount = 0;
            std::string asciiStr;
            for (int i = 0; i < 64; ++i) {
                uint8_t c = p[i];
                if (c >= 32 && c <= 126) {
                    asciiStr += static_cast<char>(c);
                    printableCount++;
                } else if (c == 0) {
                    asciiStr += "\\0";
                } else {
                    asciiStr += '.';
                }
            }
            if (printableCount >= 8) {
                std::printf("[probe] at str %s=0x%08X : '%s'\n", name, addr, asciiStr.c_str());
            }
        };

        dumpMem("s1", s1);
        dumpMem("s3", s3);
        dumpMem("s5", s5);
        dumpMem("a0", a0);

        // 4. Words at s1, s3, and 0x725968
        auto readWord = [&](uint32_t a) -> uint32_t {
            if ((a & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
                uint32_t val = 0;
                std::memcpy(&val, rdram + (a & PS2_RAM_MASK), 4);
                return val;
            }
            return 0;
        };

        uint32_t w_s1_148 = readWord(s1 + 0x148);
        uint32_t w_s1_130 = readWord(s1 + 0x130);
        uint32_t w_s1_134 = readWord(s1 + 0x134);
        uint32_t w_s1_138 = readWord(s1 + 0x138);
        uint32_t w_s1_13c = readWord(s1 + 0x13C);
        uint32_t w_s3_130 = readWord(s3 + 0x130);
        uint32_t w_s3_40  = readWord(s3 + 0x40);

        std::printf("[probe] at words s1_148=0x%08X s1_130=0x%08X s1_134=0x%08X s1_138=0x%08X s1_13c=0x%08X s3_130=0x%08X s3_40=0x%08X\n",
                    w_s1_148, w_s1_130, w_s1_134, w_s1_138, w_s1_13c, w_s3_130, w_s3_40);

        std::string jtStr;
        for (int i = 0; i < 16; ++i) {
            uint32_t entry = readWord(0x725968u + i * 4);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s[#%d]=0x%08X", (i > 0 ? " " : ""), i, entry);
            jtStr += buf;
        }
        std::printf("[probe] at jt 0x725968: %s\n", jtStr.c_str());
        std::fflush(stdout);
    }

    void dumpTaskAVInfo(uint32_t sec, const uint8_t *rdram) {
        if (!isEnabled() || !s_activeScheduler || !s_activeRuntime) return;
        if (!rdram) rdram = s_activeScheduler->rdram();
        if (!rdram) return;

        uint32_t val_649de4 = 0;
        int32_t val_649de0 = 0;
        if ((0x649DE4u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
            std::memcpy(&val_649de4, rdram + (0x649DE4u & PS2_RAM_MASK), 4);
        }
        if ((0x649DE0u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
            std::memcpy(&val_649de0, rdram + (0x649DE0u & PS2_RAM_MASK), 4);
        }

        uint32_t f_state = 0;
        if ((0x772188u & PS2_RAM_MASK) + 4 <= PS2_RAM_SIZE) {
            std::memcpy(&f_state, rdram + (0x772188u & PS2_RAM_MASK), 4);
        }

        static bool s_seenFState9 = false;
        static uint32_t s_seenFState9Sec = 0;
        if (f_state == 9 && !s_seenFState9) {
            s_seenFState9 = true;
            s_seenFState9Sec = sec;
        }

        static uint32_t s_last649de4 = 0xFFFFFFFFu;
        static uint32_t s_unchangedStreak = 0;
        if (val_649de4 == s_last649de4) {
            s_unchangedStreak++;
        } else {
            s_last649de4 = val_649de4;
            s_unchangedStreak = 0;
        }

        bool afterState9 = s_seenFState9 && (sec >= s_seenFState9Sec);
        bool unchanged2s = (s_unchangedStreak >= 2);

        PS2Memory &mem = s_activeRuntime->memory();
        auto readIO = [&](uint32_t regAddr) -> uint32_t {
            auto it = mem.m_ioRegisters.find(regAddr);
            return (it != mem.m_ioRegisters.end()) ? it->second : 0u;
        };

        uint32_t chcr = readIO(0x10009000u);
        uint32_t tadr = readIO(0x10009030u);
        uint32_t qwc  = readIO(0x10009020u);
        uint32_t asr0 = readIO(0x10009040u);
        uint32_t str_bit = (chcr & 0x100u) != 0 ? 1 : 0;
        uint32_t tagid = (chcr >> 28) & 0x7u;

        static bool s_dma1StallReported = false;
        static uint32_t s_prevChcr = 0;
        if (str_bit != 0 && (s_prevChcr & 0x100u) != 0) {
            if (!s_dma1StallReported) {
                s_dma1StallReported = true;
                std::printf("[probe] av dma1_stall tadr=0x%08X qwc=0x%X tagid=%u\n", tadr, qwc, tagid);
                std::fflush(stdout);
            }
        }
        s_prevChcr = chcr;

        uint32_t kicks_start = cnt_vif1_kicks_started.exchange(0, std::memory_order_relaxed);
        uint32_t kicks_end   = cnt_vif1_kicks_finished.exchange(0, std::memory_order_relaxed);
        uint32_t vif_irq     = cnt_vif1_irq.exchange(0, std::memory_order_relaxed);
        uint32_t cause5      = cnt_cause5_raises.exchange(0, std::memory_order_relaxed);
        uint32_t loop_404230 = cnt_404230_loop.exchange(0, std::memory_order_relaxed);

        uint32_t h2843d4_total = 0;
        std::string markHist = getAndResetVifMarkHist();
        std::string h2843d4Hist = getAndResetH2843D4Hist(h2843d4_total);

        if (afterState9 || unchanged2s) {
            uint32_t stat = mem.vif1_regs.stat;
            uint32_t mark = mem.vif1_regs.mark;
            uint32_t code = mem.vif1_regs.code;
            uint32_t imr = readIO(0x1000F010u);
            uint32_t intc_mask = s_diagEnabledIntcMask.load(std::memory_order_relaxed);

            std::printf("[probe] av sec=%u f_state=%u flag_649de4=%u sema_649de0=%d "
                        "kicks_start=%u kicks_end=%u vif_irq=%u cause5=%u h2843d4=%u loop_404230=%u "
                        "mark_hist=[%s] h2843d4_hist=[%s] "
                        "stat=0x%08X mark=0x%04X code=0x%08X "
                        "chcr=0x%08X tadr=0x%08X qwc=0x%X asr0=0x%08X str=%u "
                        "imr=0x%08X intc_mask=0x%08X\n",
                        sec, f_state, val_649de4, val_649de0,
                        kicks_start, kicks_end, vif_irq, cause5, h2843d4_total, loop_404230,
                        markHist.c_str(), h2843d4Hist.c_str(),
                        stat, mark, code,
                        chcr, tadr, qwc, asr0, str_bit,
                        imr, intc_mask);
            std::fflush(stdout);
        }
    }
}
