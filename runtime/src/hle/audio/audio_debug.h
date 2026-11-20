#pragma once

// SIN_AUDIO_LOG=1: low-rate proof-of-output logging for the AX/DSP/AI HLE (header-only so the
// mix thread, the AI tick and the AX task code can all share it without a new link dependency).
//
// Output (stderr/console.log), all lines prefixed "[audio-dbg]":
//   voice START/STOP pb=.. rate=..Hz vol=L/R fmt=..   first kMaxVoiceLines transitions only
//   backend opened ...                                 when the SDL playback stream opens
//   1s: ...                                            once per second while the AI DMA runs
// A healthy run shows aiBlocks ~ 333/s, axFrames ~ 333/s, a non-zero voices count and peak > 0.

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace AudioDebug {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SIN_AUDIO_LOG");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

struct Counters {
    std::atomic<uint64_t> aiBlocks{0};      // 3 ms AI DMA blocks delivered to the backend
    std::atomic<uint64_t> aiCallbacks{0};   // guest AI DMA callbacks (__AXOutAiCallback) invoked
    std::atomic<uint64_t> axFrames{0};      // AX command lists executed by the DSP HLE
    std::atomic<uint64_t> voiceStarts{0};
    std::atomic<uint64_t> voiceStops{0};
    std::atomic<uint32_t> voicesThisFrame{0};
    std::atomic<uint32_t> voicesLastFrame{0};
    std::atomic<uint32_t> peak{0};          // max |sample| pushed since the last 1 s line
    std::atomic<uint64_t> nonSilentBlocks{0};
    std::atomic<uint64_t> backendOpens{0};
    std::atomic<uint64_t> sdlBlocks{0};
    std::atomic<uint64_t> sdlNonSilentBlocks{0};
    std::atomic<uint32_t> sdlPeak{0};
    std::atomic<uint32_t> lastRateHz{0};
    std::atomic<uint32_t> voiceLines{0};
    std::atomic<uint32_t> mails{0};
};

inline Counters& C() {
    static Counters counters;
    return counters;
}

constexpr uint32_t kMaxVoiceLines = 160;

inline void FrameBegin() { C().voicesThisFrame.store(0, std::memory_order_relaxed); }

inline void FrameEnd() {
    C().axFrames.fetch_add(1, std::memory_order_relaxed);
    C().voicesLastFrame.store(C().voicesThisFrame.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// Called once per parameter block per AX frame, before the voice is mixed.
inline void NoteVoice(uint32_t pbAddr, uint16_t running, uint32_t srcRatio, uint16_t volL, uint16_t volR,
                      uint16_t sampleFormat, uint16_t srcType) {
    static std::mutex mutex;
    static std::unordered_map<uint32_t, bool> state;
    const bool isRunning = running == 1;
    if (isRunning) {
        C().voicesThisFrame.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(mutex);
    bool& was = state[pbAddr];
    if (was == isRunning) {
        return;
    }
    was = isRunning;
    // Output rate of a voice = (SRC ratio 16.16) * 32 kHz.
    const double rateHz = static_cast<double>(srcRatio) / 65536.0 * 32000.0;
    (isRunning ? C().voiceStarts : C().voiceStops).fetch_add(1, std::memory_order_relaxed);
    if (C().voiceLines.fetch_add(1, std::memory_order_relaxed) < kMaxVoiceLines) {
        std::fprintf(stderr,
                     "[audio-dbg] voice %s pb=0x%08X rate=%.0fHz vol=%04X/%04X fmt=0x%04X src=%u\n",
                     isRunning ? "START" : "STOP ", pbAddr, rateHz, volL, volR, sampleFormat, srcType);
    }
}

// Big-endian interleaved 16-bit AI block: track the peak and whether it was silent.
inline void NoteBlock(const uint8_t* beSamples, uint32_t bytes, uint32_t sampleRate) {
    C().aiBlocks.fetch_add(1, std::memory_order_relaxed);
    C().lastRateHz.store(sampleRate, std::memory_order_relaxed);
    uint32_t peak = 0;
    for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        const int16_t s = static_cast<int16_t>((static_cast<uint16_t>(beSamples[i]) << 8) | beSamples[i + 1]);
        const uint32_t a = s < 0 ? static_cast<uint32_t>(-static_cast<int32_t>(s)) : static_cast<uint32_t>(s);
        if (a > peak) {
            peak = a;
        }
    }
    if (peak != 0) {
        C().nonSilentBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t cur = C().peak.load(std::memory_order_relaxed);
    while (peak > cur && !C().peak.compare_exchange_weak(cur, peak, std::memory_order_relaxed)) {
    }
}

inline void NoteBackendOpen(uint32_t sampleRate, uint32_t channels) {
    C().backendOpens.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[audio-dbg] backend opened sdl=S16LE rate=%uHz channels=%u\n",
                 sampleRate, channels);
    std::fflush(stderr);
}

// Called only after SDL_PutAudioStreamData accepted the PCM.  Queue overflow and SDL
// errors therefore cannot satisfy the software audio-output acceptance check.
inline void NoteSdlQueued(const int16_t* samples, size_t sampleCount) {
    uint32_t peak = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
        const int32_t sample = samples[i];
        const uint32_t magnitude = sample < 0
                                       ? static_cast<uint32_t>(-sample)
                                       : static_cast<uint32_t>(sample);
        peak = std::max(peak, magnitude);
    }
    C().sdlBlocks.fetch_add(1, std::memory_order_relaxed);
    if (peak != 0) {
        C().sdlNonSilentBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t current = C().sdlPeak.load(std::memory_order_relaxed);
    while (peak > current &&
           !C().sdlPeak.compare_exchange_weak(current, peak, std::memory_order_relaxed)) {
    }
}

// Cheap enough to call on every tick; prints at most once per second.
inline void MaybeReport(bool dmaEnabled, bool callbackSet) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static uint64_t lastBlocks = 0, lastCallbacks = 0, lastFrames = 0, lastNonSilent = 0;
    static uint64_t lastSdlBlocks = 0, lastSdlNonSilent = 0;
    const Clock::time_point now = Clock::now();
    if (now - last < std::chrono::seconds(1)) {
        return;
    }
    last = now;
    Counters& c = C();
    const uint64_t blocks = c.aiBlocks.load(), callbacks = c.aiCallbacks.load(), frames = c.axFrames.load(),
                   nonSilent = c.nonSilentBlocks.load(), sdlBlocks = c.sdlBlocks.load(),
                   sdlNonSilent = c.sdlNonSilentBlocks.load();
    std::fprintf(stderr,
                 "[audio-dbg] 1s: aiBlocks=+%llu axFrames=+%llu aiCallbacks=+%llu nonSilentBlocks=+%llu "
                 "sdlBlocks=+%llu sdlNonSilentBlocks=+%llu sdlPeak=%u backendOpens=%llu "
                 "voices=%u (started %llu stopped %llu) peak=%u rate=%uHz dma=%d cb=%d\n",
                 static_cast<unsigned long long>(blocks - lastBlocks),
                 static_cast<unsigned long long>(frames - lastFrames),
                 static_cast<unsigned long long>(callbacks - lastCallbacks),
                 static_cast<unsigned long long>(nonSilent - lastNonSilent),
                 static_cast<unsigned long long>(sdlBlocks - lastSdlBlocks),
                 static_cast<unsigned long long>(sdlNonSilent - lastSdlNonSilent),
                 c.sdlPeak.exchange(0), static_cast<unsigned long long>(c.backendOpens.load()),
                 c.voicesLastFrame.load(), static_cast<unsigned long long>(c.voiceStarts.load()),
                 static_cast<unsigned long long>(c.voiceStops.load()), c.peak.exchange(0), c.lastRateHz.load(),
                 dmaEnabled ? 1 : 0, callbackSet ? 1 : 0);
    std::fflush(stderr);
    lastBlocks = blocks;
    lastCallbacks = callbacks;
    lastFrames = frames;
    lastNonSilent = nonSilent;
    lastSdlBlocks = sdlBlocks;
    lastSdlNonSilent = sdlNonSilent;
}

} // namespace AudioDebug
