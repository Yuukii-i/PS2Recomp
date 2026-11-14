#include "ps2_host_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

extern void ps2xSafeLog(const char *fmt, ...);

namespace ps2x::iop
{
    // PERF-AU2: Local declarations for ps2xIOP SPU2 mixer bridge (header-rule safe).
    uint32_t mixSpu2AudioBlock(int16_t *outStereo, uint32_t frameCount);
    void getSpu2MixStats(uint32_t &outActiveVoices, uint64_t &outKeyOns, int32_t &outPeak);
}

namespace ps2x::audio
{
    namespace
    {
        constexpr uint32_t kSampleRate = 48000u;
        constexpr uint32_t kBlockFrames = 256u;
        constexpr uint32_t kRingCapacityFrames = 4096u; // 85.3 ms capacity (power of 2)
        constexpr uint32_t kRingMask = kRingCapacityFrames - 1u;
        constexpr uint32_t kTargetFillFrames = 2560u;   // 53.3 ms target latency (40-80 ms)

        bool isSg2LleEnabled() noexcept
        {
            const char *env = std::getenv("PS2X_SG2_LLE");
            return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
        }

        bool isAudioOutMuted() noexcept
        {
            const char *env = std::getenv("PS2X_AUDIO_OUT");
            return env && std::strcmp(env, "0") == 0;
        }

        struct Spu2AudioState
        {
            std::array<int16_t, kRingCapacityFrames * 2u> ring{};
            std::atomic<uint32_t> writePos{0u};
            std::atomic<uint32_t> readPos{0u};

            std::atomic<bool> running{false};
            std::atomic<bool> muted{false};
            std::atomic<bool> streamActive{false};
            std::thread mixerThread;

            std::atomic<uint64_t> framesProduced{0u};
            std::atomic<uint64_t> framesConsumed{0u};
            std::atomic<uint64_t> underruns{0u};

            AudioStream stream{};
            bool hasStream = false;
        };

        Spu2AudioState g_spu2Audio;

        uint32_t ringReadableFrames() noexcept
        {
            const uint32_t w = g_spu2Audio.writePos.load(std::memory_order_acquire);
            const uint32_t r = g_spu2Audio.readPos.load(std::memory_order_acquire);
            return (w - r) & kRingMask;
        }

        uint32_t ringWritableFrames() noexcept
        {
            return (kRingCapacityFrames - 1u) - ringReadableFrames();
        }

        void pushFramesToRing(const int16_t *srcStereo, uint32_t frames) noexcept
        {
            uint32_t w = g_spu2Audio.writePos.load(std::memory_order_relaxed);
            const uint32_t first = std::min(frames, kRingCapacityFrames - w);
            std::memcpy(&g_spu2Audio.ring[w * 2u], srcStereo, static_cast<size_t>(first) * 2u * sizeof(int16_t));
            if (frames > first)
            {
                std::memcpy(&g_spu2Audio.ring[0], srcStereo + first * 2u,
                            static_cast<size_t>(frames - first) * 2u * sizeof(int16_t));
            }
            g_spu2Audio.writePos.store((w + frames) & kRingMask, std::memory_order_release);
            g_spu2Audio.framesProduced.fetch_add(frames, std::memory_order_relaxed);
        }

        void popFramesFromRing(int16_t *dstStereo, uint32_t frames) noexcept
        {
            const uint32_t avail = ringReadableFrames();
            const uint32_t toCopy = std::min(frames, avail);
            if (toCopy > 0u)
            {
                uint32_t r = g_spu2Audio.readPos.load(std::memory_order_relaxed);
                const uint32_t first = std::min(toCopy, kRingCapacityFrames - r);
                if (dstStereo)
                {
                    std::memcpy(dstStereo, &g_spu2Audio.ring[r * 2u], static_cast<size_t>(first) * 2u * sizeof(int16_t));
                    if (toCopy > first)
                    {
                        std::memcpy(dstStereo + first * 2u, &g_spu2Audio.ring[0],
                                    static_cast<size_t>(toCopy - first) * 2u * sizeof(int16_t));
                    }
                }
                g_spu2Audio.readPos.store((r + toCopy) & kRingMask, std::memory_order_release);
                g_spu2Audio.framesConsumed.fetch_add(toCopy, std::memory_order_relaxed);
            }

            if (toCopy < frames)
            {
                if (dstStereo)
                {
                    std::memset(dstStereo + toCopy * 2u, 0,
                                static_cast<size_t>(frames - toCopy) * 2u * sizeof(int16_t));
                }
                g_spu2Audio.underruns.fetch_add(1u, std::memory_order_relaxed);
            }
        }

        void raylibAudioCallback(void *bufferData, unsigned int frames)
        {
            auto *out = static_cast<int16_t *>(bufferData);
            if (!out || frames == 0u)
                return;

            if (!g_spu2Audio.running.load(std::memory_order_acquire))
            {
                std::memset(out, 0, static_cast<size_t>(frames) * 2u * sizeof(int16_t));
                return;
            }

            popFramesFromRing(out, frames);

            if (g_spu2Audio.muted.load(std::memory_order_relaxed))
            {
                std::memset(out, 0, static_cast<size_t>(frames) * 2u * sizeof(int16_t));
            }
        }

        void maybeLogDiagnostics(std::chrono::steady_clock::time_point &lastLog, uint64_t &lastKeyOns)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog).count();
            if (elapsedMs < 5000)
                return;

            lastLog = now;
            uint32_t activeVoices = 0u;
            uint64_t totalKeyOns = 0u;
            int32_t peak = 0;
            ps2x::iop::getSpu2MixStats(activeVoices, totalKeyOns, peak);

            const double sec = static_cast<double>(elapsedMs) / 1000.0;
            const double keyOnsPerSec = static_cast<double>(totalKeyOns - lastKeyOns) / (sec > 0.0 ? sec : 1.0);
            lastKeyOns = totalKeyOns;

            const uint64_t produced = g_spu2Audio.framesProduced.load(std::memory_order_relaxed);
            const uint64_t consumed = g_spu2Audio.framesConsumed.load(std::memory_order_relaxed);
            const uint64_t underruns = g_spu2Audio.underruns.load(std::memory_order_relaxed);
            const double peakPct = (static_cast<double>(peak) / 32767.0) * 100.0;

            ::ps2xSafeLog(
                "[spu2] activeVoices=%u keyOns/s=%.1f produced=%llu consumed=%llu underruns=%llu peak=%d (%.1f%%)\n",
                activeVoices,
                keyOnsPerSec,
                static_cast<unsigned long long>(produced),
                static_cast<unsigned long long>(consumed),
                static_cast<unsigned long long>(underruns),
                peak,
                peakPct);
        }

        void mixerThreadMain()
        {
            std::array<int16_t, kBlockFrames * 2u> block{};
            auto lastLog = std::chrono::steady_clock::now();
            auto lastPace = std::chrono::steady_clock::now();
            uint64_t lastKeyOns = 0u;

            while (g_spu2Audio.running.load(std::memory_order_acquire))
            {
                if (!g_spu2Audio.streamActive.load(std::memory_order_relaxed))
                {
                    // Headless / no-audio-device fallback: pace at 48 kHz so synthesis and diagnostics still run.
                    const auto now = std::chrono::steady_clock::now();
                    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - lastPace).count();
                    const uint32_t dueFrames = static_cast<uint32_t>((static_cast<uint64_t>(std::max<int64_t>(0, us)) * kSampleRate) / 1000000ULL);
                    if (dueFrames >= kBlockFrames)
                    {
                        lastPace = now;
                        const uint32_t avail = ringReadableFrames();
                        if (avail >= kBlockFrames)
                            popFramesFromRing(nullptr, kBlockFrames);
                    }
                }

                while (g_spu2Audio.running.load(std::memory_order_acquire) &&
                       ringReadableFrames() < kTargetFillFrames &&
                       ringWritableFrames() >= kBlockFrames)
                {
                    ps2x::iop::mixSpu2AudioBlock(block.data(), kBlockFrames);
                    pushFramesToRing(block.data(), kBlockFrames);
                }

                maybeLogDiagnostics(lastLog, lastKeyOns);
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        }
    }

    // PERF-AU2: Start SPU2 mixer thread and raylib AudioStream when PS2X_SG2_LLE=1.
    void startSpu2Output()
    {
        if (!isSg2LleEnabled())
            return;
        if (g_spu2Audio.running.load(std::memory_order_acquire))
            return;

        g_spu2Audio.writePos.store(0u, std::memory_order_relaxed);
        g_spu2Audio.readPos.store(0u, std::memory_order_relaxed);
        g_spu2Audio.framesProduced.store(0u, std::memory_order_relaxed);
        g_spu2Audio.framesConsumed.store(0u, std::memory_order_relaxed);
        g_spu2Audio.underruns.store(0u, std::memory_order_relaxed);
        g_spu2Audio.muted.store(isAudioOutMuted(), std::memory_order_relaxed);

        // Pre-fill target latency before starting playback to avoid initial underruns.
        std::array<int16_t, kBlockFrames * 2u> prefill{};
        for (uint32_t f = 0u; f < kTargetFillFrames; f += kBlockFrames)
        {
            ps2x::iop::mixSpu2AudioBlock(prefill.data(), kBlockFrames);
            pushFramesToRing(prefill.data(), kBlockFrames);
        }

        g_spu2Audio.hasStream = false;
        g_spu2Audio.streamActive.store(false, std::memory_order_relaxed);

#if !defined(PLATFORM_VITA)
        if (IsAudioDeviceReady())
        {
            g_spu2Audio.stream = LoadAudioStream(kSampleRate, 16u, 2u);
            if (IsAudioStreamValid(g_spu2Audio.stream))
            {
                g_spu2Audio.hasStream = true;
                g_spu2Audio.streamActive.store(true, std::memory_order_relaxed);
                SetAudioStreamCallback(g_spu2Audio.stream, raylibAudioCallback);
                PlayAudioStream(g_spu2Audio.stream);
            }
        }
#endif

        g_spu2Audio.running.store(true, std::memory_order_release);
        g_spu2Audio.mixerThread = std::thread(mixerThreadMain);

        ::ps2xSafeLog("[spu2] mixer started (48kHz stereo, raylib AudioStream=%s, muted=%d)\n",
                      g_spu2Audio.hasStream ? "on" : "off",
                      g_spu2Audio.muted.load(std::memory_order_relaxed) ? 1 : 0);
    }

    void stopSpu2Output()
    {
        if (!g_spu2Audio.running.exchange(false, std::memory_order_acq_rel))
            return;

        if (g_spu2Audio.mixerThread.joinable())
            g_spu2Audio.mixerThread.join();

#if !defined(PLATFORM_VITA)
        if (g_spu2Audio.hasStream)
        {
            StopAudioStream(g_spu2Audio.stream);
            UnloadAudioStream(g_spu2Audio.stream);
            g_spu2Audio.hasStream = false;
            g_spu2Audio.stream = {};
        }
#endif
        g_spu2Audio.streamActive.store(false, std::memory_order_relaxed);
    }
}
