#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

struct FrameStatistics
{
    using Clock = std::chrono::steady_clock;

    struct Sample
    {
        Clock::time_point at;
        double captureFps;
        double programFps;
        double freshFps;
    };

    bool ready = false;
    double captureFps = 0;
    double programFps = 0;
    double freshFps = 0;
    double processingMs = 0;
    double peakProcessingMs = 0;

    void Reset(Clock::time_point now, uint64_t capturedFrames)
    {
        *this = {};
        sampleStart = now;
        captureStart = capturedFrames;
    }

    void RecordPresent(int64_t frameTimestamp, double elapsedMs)
    {
        ++presents;
        if (!lastFrame || frameTimestamp != *lastFrame)
            ++freshFrames;
        lastFrame = frameTimestamp;
        processingTotalMs += elapsedMs;
        processingPeakMs = std::max(processingPeakMs, elapsedMs);
    }

    void Update(Clock::time_point now, uint64_t capturedFrames)
    {
        const double seconds = std::chrono::duration<double>(now - sampleStart).count();
        if (seconds < 1)
            return;
        captureFps = (capturedFrames - captureStart) / seconds;
        programFps = presents / seconds;
        freshFps = freshFrames / seconds;
        processingMs = presents ? processingTotalMs / presents : 0;
        peakProcessingMs = processingPeakMs;
        history[historyNext] = { now, captureFps, programFps, freshFps };
        historyNext = (historyNext + 1) % history.size();
        historyCount = std::min(historyCount + 1, history.size());
        ready = true;
        sampleStart = now;
        captureStart = capturedFrames;
        presents = freshFrames = 0;
        processingTotalMs = 0;
        processingPeakMs = 0;
    }

    double LostFps() const
    {
        return std::max(0.0, captureFps - freshFps);
    }

    double LossPercent() const
    {
        return captureFps > 0 ? LostFps() / captureFps * 100 : 0;
    }

    double RepeatedFps() const
    {
        return std::max(0.0, programFps - freshFps);
    }

    size_t HistorySize() const
    {
        return historyCount;
    }

    // Samples run from oldest to newest, including after the buffer wraps.
    const Sample& HistoryAt(size_t index) const
    {
        return history[(historyNext + history.size() - historyCount + index) % history.size()];
    }

private:
    Clock::time_point sampleStart{};
    uint64_t captureStart = 0;
    uint64_t presents = 0;
    uint64_t freshFrames = 0;
    double processingTotalMs = 0;
    double processingPeakMs = 0;
    std::optional<int64_t> lastFrame;
    std::array<Sample, 60> history{};
    size_t historyNext = 0;
    size_t historyCount = 0;
};
