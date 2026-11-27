#include "ps2_host_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath> // PROBE-AU3D
#include <cstdint>
#include <cstdio> // PROBE-AU3D
#include <cstdlib>
#include <cstring>
#include <mutex> // FIX-AU4
#include <string> // PROBE-AU3D
#include <thread>

extern void ps2xSafeLog(const char *fmt, ...);

#if defined(PS2X_HAS_SDL3) // SDL-FE
extern bool ps2xSdlActive();
extern bool ps2xSdlAudioStart(void (*fill)(void *buffer, unsigned frames));
extern void ps2xSdlAudioStop();
#else
inline bool ps2xSdlActive() { return false; }
inline bool ps2xSdlAudioStart(void (*)(void *, unsigned)) { return false; }
inline void ps2xSdlAudioStop() {}
#endif

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
        constexpr uint64_t kWavMaxBytes = 1024ULL * 1024ULL * 1024ULL; // PROBE-AU3D
        constexpr uint64_t kWavFlushFrames = 96000ULL;                 // PROBE-AU3D
        constexpr uint32_t kWavHeaderBytes = 44u;                      // PROBE-AU3D

        const std::string &getWavOutputPath() // PROBE-AU3D
        { // PROBE-AU3D
            static const std::string path = []() { // PROBE-AU3D
                const char *env = std::getenv("PS2X_AUDIO_WAV"); // PROBE-AU3D
                return (env && env[0] != '\0') ? std::string(env) : std::string(); // PROBE-AU3D
            }(); // PROBE-AU3D
            return path; // PROBE-AU3D
        } // PROBE-AU3D

        void writeWavHeader(std::FILE *fp) noexcept // PROBE-AU3D
        { // PROBE-AU3D
            const uint32_t riffSize = 36u; // PROBE-AU3D
            const uint32_t fmtSize = 16u; // PROBE-AU3D
            const uint16_t audioFmt = 1u; // PROBE-AU3D
            const uint16_t channels = 2u; // PROBE-AU3D
            const uint32_t sampleRate = kSampleRate; // PROBE-AU3D
            const uint32_t byteRate = kSampleRate * channels * sizeof(int16_t); // PROBE-AU3D
            const uint16_t blockAlign = channels * sizeof(int16_t); // PROBE-AU3D
            const uint16_t bitsPerSample = 16u; // PROBE-AU3D
            const uint32_t dataSize = 0u; // PROBE-AU3D
            std::fwrite("RIFF", 1, 4, fp); // PROBE-AU3D
            std::fwrite(&riffSize, 4, 1, fp); // PROBE-AU3D
            std::fwrite("WAVE", 1, 4, fp); // PROBE-AU3D
            std::fwrite("fmt ", 1, 4, fp); // PROBE-AU3D
            std::fwrite(&fmtSize, 4, 1, fp); // PROBE-AU3D
            std::fwrite(&audioFmt, 2, 1, fp); // PROBE-AU3D
            std::fwrite(&channels, 2, 1, fp); // PROBE-AU3D
            std::fwrite(&sampleRate, 4, 1, fp); // PROBE-AU3D
            std::fwrite(&byteRate, 4, 1, fp); // PROBE-AU3D
            std::fwrite(&blockAlign, 2, 1, fp); // PROBE-AU3D
            std::fwrite(&bitsPerSample, 2, 1, fp); // PROBE-AU3D
            std::fwrite("data", 1, 4, fp); // PROBE-AU3D
            std::fwrite(&dataSize, 4, 1, fp); // PROBE-AU3D
            std::fflush(fp); // PROBE-AU3D
        } // PROBE-AU3D

        void patchWavSizes(std::FILE *fp, uint64_t dataBytes) noexcept // PROBE-AU3D
        { // PROBE-AU3D
            if (!fp) // PROBE-AU3D
                return; // PROBE-AU3D
            const uint32_t dataSize = static_cast<uint32_t>(std::min<uint64_t>(dataBytes, kWavMaxBytes - kWavHeaderBytes)); // PROBE-AU3D
            const uint32_t riffSize = 36u + dataSize; // PROBE-AU3D
            std::fseek(fp, 4, SEEK_SET); // PROBE-AU3D
            std::fwrite(&riffSize, 4, 1, fp); // PROBE-AU3D
            std::fseek(fp, 40, SEEK_SET); // PROBE-AU3D
            std::fwrite(&dataSize, 4, 1, fp); // PROBE-AU3D
            std::fseek(fp, 0, SEEK_END); // PROBE-AU3D
            std::fflush(fp); // PROBE-AU3D
        } // PROBE-AU3D

        struct LoudnessStats // PROBE-AU3D
        { // PROBE-AU3D
            double sumSq = 0.0; // PROBE-AU3D
            uint64_t sampleCount = 0u; // PROBE-AU3D
            int32_t peak = 0; // PROBE-AU3D
            uint64_t blocks = 0u; // PROBE-AU3D
            uint64_t audibleBlocks = 0u; // PROBE-AU3D
        }; // PROBE-AU3D

        bool isSg2LleEnabled() noexcept
        {
            const char *env = std::getenv("PS2X_SG2_LLE");
            return !env || env[0] == '\0' || std::strcmp(env, "0") != 0; // FIX-AU10
        }

        bool isAudioOutMuted() noexcept
        {
            const char *env = std::getenv("PS2X_AUDIO_OUT");
            return env && std::strcmp(env, "0") == 0;
        }

        bool isSpu2WallclockMode() noexcept // FIX-AU5
        {
            static const bool s_wallclock = []() {
                const char *env = std::getenv("PS2X_SPU2_GUESTCLOCK");
                return !(env && std::strcmp(env, "1") == 0);
            }();
            return s_wallclock;
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

            std::FILE *wavFile = nullptr;      // PROBE-AU3D
            uint64_t wavDataBytes = 0u;        // PROBE-AU3D
            uint64_t wavFramesSinceFlush = 0u; // PROBE-AU3D

            std::mutex diagMutex;              // FIX-AU4
            LoudnessStats loudness{};          // FIX-AU4
            uint64_t vblankTick = 0u;          // FIX-AU4
        };

        Spu2AudioState g_spu2Audio;
        std::atomic<uint64_t> s_au6FramesMixed{0}; // PROBE-AU6

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

        void recordMixedBlock(const int16_t *samples, uint32_t frameCount) noexcept // FIX-AU4
        {
            s_au6FramesMixed.fetch_add(frameCount, std::memory_order_relaxed); // PROBE-AU6
            if (g_spu2Audio.wavFile != nullptr) // PROBE-AU3D
            { // PROBE-AU3D
                const size_t blockBytes = static_cast<size_t>(frameCount) * 2u * sizeof(int16_t); // PROBE-AU3D
                if (kWavHeaderBytes + g_spu2Audio.wavDataBytes + blockBytes <= kWavMaxBytes) // PROBE-AU3D
                { // PROBE-AU3D
                    std::fwrite(samples, 1, blockBytes, g_spu2Audio.wavFile); // PROBE-AU3D
                    g_spu2Audio.wavDataBytes += blockBytes; // PROBE-AU3D
                    g_spu2Audio.wavFramesSinceFlush += frameCount; // PROBE-AU3D
                    if (g_spu2Audio.wavFramesSinceFlush >= kWavFlushFrames) // PROBE-AU3D
                    { // PROBE-AU3D
                        patchWavSizes(g_spu2Audio.wavFile, g_spu2Audio.wavDataBytes); // PROBE-AU3D
                        g_spu2Audio.wavFramesSinceFlush = 0u; // PROBE-AU3D
                    } // PROBE-AU3D
                } // PROBE-AU3D
            } // PROBE-AU3D

            int32_t blockPeak = 0; // PROBE-AU3D
            double blockSumSq = 0.0; // FIX-AU4
            const uint32_t sampleCount = frameCount * 2u; // FIX-AU4
            for (uint32_t i = 0u; i < sampleCount; ++i) // PROBE-AU3D
            { // PROBE-AU3D
                const int32_t s = static_cast<int32_t>(samples[i]); // PROBE-AU3D
                const int32_t absS = (s < 0) ? -s : s; // PROBE-AU3D
                if (absS > blockPeak) // PROBE-AU3D
                    blockPeak = absS; // PROBE-AU3D
                blockSumSq += static_cast<double>(s) * static_cast<double>(s); // PROBE-AU3D
            } // PROBE-AU3D

            std::lock_guard<std::mutex> lock(g_spu2Audio.diagMutex); // FIX-AU4
            g_spu2Audio.loudness.sumSq += blockSumSq; // PROBE-AU3D
            g_spu2Audio.loudness.sampleCount += sampleCount; // PROBE-AU3D
            if (blockPeak > g_spu2Audio.loudness.peak) // PROBE-AU3D
                g_spu2Audio.loudness.peak = blockPeak; // PROBE-AU3D
            ++g_spu2Audio.loudness.blocks; // PROBE-AU3D
            if (blockPeak > 64) // PROBE-AU3D
                ++g_spu2Audio.loudness.audibleBlocks; // PROBE-AU3D
        }

        void maybeLogDiagnostics(std::chrono::steady_clock::time_point &lastLog,
                                 const std::chrono::steady_clock::time_point &mixerStart, // PROBE-AU3D
                                 uint64_t &lastKeyOns) // FIX-AU4
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
            const uint64_t keyOns5s = totalKeyOns - lastKeyOns; // PROBE-AU3D
            const double keyOnsPerSec = static_cast<double>(keyOns5s) / (sec > 0.0 ? sec : 1.0); // PROBE-AU3D
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

            LoudnessStats loudness{}; // FIX-AU4
            { // FIX-AU4
                std::lock_guard<std::mutex> lock(g_spu2Audio.diagMutex); // FIX-AU4
                loudness = g_spu2Audio.loudness; // FIX-AU4
                g_spu2Audio.loudness = LoudnessStats{}; // PROBE-AU3D
            } // FIX-AU4

            const double tSec = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(now - mixerStart).count()) / 1000.0; // PROBE-AU3D
            const double rms = (loudness.sampleCount > 0u) ? std::sqrt(loudness.sumSq / static_cast<double>(loudness.sampleCount)) : 0.0; // PROBE-AU3D
            const double rmsDb = (rms > 0.0) ? (20.0 * std::log10(rms / 32768.0)) : -120.0; // PROBE-AU3D
            const double peakDb = (loudness.peak > 0) ? (20.0 * std::log10(static_cast<double>(loudness.peak) / 32768.0)) : -120.0; // PROBE-AU3D
            const double audiblePct = (loudness.blocks > 0u) ? (static_cast<double>(loudness.audibleBlocks) * 100.0 / static_cast<double>(loudness.blocks)) : 0.0; // PROBE-AU3D
            ::ps2xSafeLog( // PROBE-AU3D
                "[spu2rms] t=%.1f rmsDb=%.1f peakDb=%.1f audiblePct=%.1f active=%u keyOns5s=%llu\n", // PROBE-AU3D
                tSec, // PROBE-AU3D
                rmsDb, // PROBE-AU3D
                peakDb, // PROBE-AU3D
                audiblePct, // PROBE-AU3D
                activeVoices, // PROBE-AU3D
                static_cast<unsigned long long>(keyOns5s)); // PROBE-AU3D
        }

        void mixerThreadMain()
        {
            std::array<int16_t, kBlockFrames * 2u> block{};
            const auto mixerStart = std::chrono::steady_clock::now(); // PROBE-AU3D
            auto lastLog = mixerStart; // PROBE-AU3D
            auto lastPace = mixerStart; // PROBE-AU3D
            uint64_t lastKeyOns = 0u;
            const bool wallclockMode = isSpu2WallclockMode(); // FIX-AU4

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

                if (wallclockMode) // FIX-AU4
                {
                    while (g_spu2Audio.running.load(std::memory_order_acquire) &&
                           ringReadableFrames() < kTargetFillFrames &&
                           ringWritableFrames() >= kBlockFrames)
                    {
                        ps2x::iop::mixSpu2AudioBlock(block.data(), kBlockFrames);
                        pushFramesToRing(block.data(), kBlockFrames);
                        recordMixedBlock(block.data(), kBlockFrames); // FIX-AU4
                    }
                }

                maybeLogDiagnostics(lastLog, mixerStart, lastKeyOns); // PROBE-AU3D
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        }
    }

    // PERF-AU2: Start SPU2 mixer thread and raylib AudioStream unless PS2X_SG2_LLE=0. // FIX-AU10
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
        g_spu2Audio.vblankTick = 0u; // FIX-AU4
        { // FIX-AU4
            std::lock_guard<std::mutex> lock(g_spu2Audio.diagMutex); // FIX-AU4
            g_spu2Audio.loudness = LoudnessStats{}; // FIX-AU4
        } // FIX-AU4

        // Pre-fill target latency before starting playback to avoid initial underruns.
        std::array<int16_t, kBlockFrames * 2u> prefill{};
        for (uint32_t f = 0u; f < kTargetFillFrames; f += kBlockFrames)
        {
            ps2x::iop::mixSpu2AudioBlock(prefill.data(), kBlockFrames);
            pushFramesToRing(prefill.data(), kBlockFrames);
        }

        g_spu2Audio.hasStream = false;
        g_spu2Audio.streamActive.store(false, std::memory_order_relaxed);

        g_spu2Audio.wavFile = nullptr;      // PROBE-AU3D
        g_spu2Audio.wavDataBytes = 0u;        // PROBE-AU3D
        g_spu2Audio.wavFramesSinceFlush = 0u; // PROBE-AU3D
        const std::string &wavPath = getWavOutputPath(); // PROBE-AU3D
        if (!wavPath.empty()) // PROBE-AU3D
        { // PROBE-AU3D
            g_spu2Audio.wavFile = std::fopen(wavPath.c_str(), "wb"); // PROBE-AU3D
            if (g_spu2Audio.wavFile != nullptr) // PROBE-AU3D
                writeWavHeader(g_spu2Audio.wavFile); // PROBE-AU3D
        } // PROBE-AU3D

#if !defined(PLATFORM_VITA)
        if (ps2xSdlActive()) // SDL-FE
        {
            if (ps2xSdlAudioStart(raylibAudioCallback))
            {
                g_spu2Audio.hasStream = true;
                g_spu2Audio.streamActive.store(true, std::memory_order_relaxed);
            }
        }
        else if (IsAudioDeviceReady())
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

        ::ps2xSafeLog("[spu2] mixer started (48kHz stereo, %s=%s, muted=%d)\n", // SDL-FE
                      ps2xSdlActive() ? "SDL audio" : "raylib AudioStream",
                      g_spu2Audio.hasStream ? "on" : "off",
                      g_spu2Audio.muted.load(std::memory_order_relaxed) ? 1 : 0);
    }

    void stopSpu2Output()
    {
        if (!g_spu2Audio.running.exchange(false, std::memory_order_acq_rel))
            return;

        if (g_spu2Audio.mixerThread.joinable())
            g_spu2Audio.mixerThread.join();

        if (g_spu2Audio.wavFile != nullptr) // PROBE-AU3D
        { // PROBE-AU3D
            patchWavSizes(g_spu2Audio.wavFile, g_spu2Audio.wavDataBytes); // PROBE-AU3D
            std::fclose(g_spu2Audio.wavFile); // PROBE-AU3D
            g_spu2Audio.wavFile = nullptr; // PROBE-AU3D
        } // PROBE-AU3D

#if !defined(PLATFORM_VITA)
        if (ps2xSdlActive()) // SDL-FE
        {
            if (g_spu2Audio.hasStream)
            {
                ps2xSdlAudioStop();
                g_spu2Audio.hasStream = false;
            }
        }
        else if (g_spu2Audio.hasStream)
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

void ps2xSpu2GuestVblank() // FIX-AU4
{
    using namespace ps2x::audio;
    static const bool s_enabled = isSg2LleEnabled() && !isSpu2WallclockMode();
    if (!s_enabled)
        return;
    if (!g_spu2Audio.running.load(std::memory_order_acquire))
        return;

    const uint64_t t = g_spu2Audio.vblankTick++;
    const uint64_t nextTotal = ((t + 1ULL) * 48000ULL * 1001ULL) / 60000ULL;
    const uint64_t curTotal = (t * 48000ULL * 1001ULL) / 60000ULL;
    uint32_t remaining = static_cast<uint32_t>(nextTotal - curTotal);

    static std::array<int16_t, kBlockFrames * 2u> s_block{};
    while (remaining > 0u)
    {
        const uint32_t chunk = std::min(remaining, kBlockFrames);
        ps2x::iop::mixSpu2AudioBlock(s_block.data(), chunk);
        recordMixedBlock(s_block.data(), chunk);
        const uint32_t writable = ringWritableFrames();
        const uint32_t toPush = std::min(chunk, writable);
        if (toPush > 0u)
            pushFramesToRing(s_block.data(), toPush);
        remaining -= chunk;
    }
}

uint64_t ps2xAu6FramesMixed() // PROBE-AU6
{ // PROBE-AU6
    return ps2x::audio::s_au6FramesMixed.load(std::memory_order_relaxed); // PROBE-AU6
} // PROBE-AU6

uint64_t ps2xAu6FramesConsumed() // PROBE-AU6
{ // PROBE-AU6
    return ps2x::audio::g_spu2Audio.framesConsumed.load(std::memory_order_relaxed); // PROBE-AU6
} // PROBE-AU6
