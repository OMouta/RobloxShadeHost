#include "../src/frame_statistics.h"

#include <cmath>
#include <cstdio>

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s\n", message);
    return condition;
}

bool Near(double actual, double expected)
{
    return std::abs(actual - expected) < 0.001;
}
} // namespace

int main()
{
    using namespace std::chrono;
    const FrameStatistics::Clock::time_point start{};
    bool ok = true;
    FrameStatistics stats;
    stats.Reset(start, 100);
    for (int64_t frame = 0; frame < 60; ++frame)
        stats.RecordPresent(frame, 2);
    stats.Update(start + milliseconds(500), 160);
    ok &= Check(!stats.ready, "waits for a full sample before showing FPS");
    stats.Update(start + seconds(1), 220);
    ok &= Check(Near(stats.captureFps, 120) && Near(stats.programFps, 60) && Near(stats.freshFps, 60),
                "input and output rates use the same interval");
    ok &= Check(Near(stats.LostFps(), 60) && Near(stats.LossPercent(), 50), "shows capture-to-output loss");
    ok &= Check(Near(stats.processingMs, 2), "averages processing time per presentation");
    ok &= Check(Near(stats.peakProcessingMs, 2), "records the peak processing time");
    ok &= Check(stats.HistorySize() == 1 && Near(stats.HistoryAt(0).captureFps, 120),
                "records completed samples for the graph");

    for (int frame = 0; frame < 60; ++frame)
        stats.RecordPresent(59, 4);
    stats.Update(start + seconds(2), 220);
    ok &= Check(Near(stats.captureFps, 0) && Near(stats.programFps, 60) && Near(stats.freshFps, 0),
                "redrawing the same frame does not inflate fresh output FPS");
    ok &= Check(Near(stats.LossPercent(), 0) && Near(stats.processingMs, 4), "idle capture does not divide by zero");
    ok &= Check(Near(stats.RepeatedFps(), 60), "reports repeated frames separately");

    for (int64_t frame = 60; frame < 120; ++frame)
    {
        stats.RecordPresent(frame, 2);
        stats.RecordPresent(frame, 4);
    }
    stats.Update(start + seconds(4), 280);
    ok &= Check(Near(stats.captureFps, 30) && Near(stats.programFps, 60) && Near(stats.freshFps, 30),
                "uses actual elapsed time and separates duplicate presentations");
    ok &= Check(Near(stats.processingMs, 3) && Near(stats.LostFps(), 0), "duplicates do not hide loss or alter processing averages");
    ok &= Check(Near(stats.peakProcessingMs, 4), "peak processing time is measured within each sample");
    ok &= Check(stats.HistoryAt(2).at == start + seconds(4), "graph samples retain their actual timestamps");

    stats.Reset(start + seconds(10), 1000);
    ok &= Check(!stats.ready && stats.captureFps == 0 && stats.programFps == 0 && stats.freshFps == 0,
                "switching games or hiding the overlay clears old statistics");
    ok &= Check(stats.HistorySize() == 0 && stats.peakProcessingMs == 0, "reset clears graph history and processing peaks");
    stats.RecordPresent(0, 5);
    stats.Update(start + seconds(11), 1001);
    ok &= Check(Near(stats.captureFps, 1) && Near(stats.freshFps, 1), "reset clears the previous frame timestamp and counters");
    stats.Update(start + seconds(12), 1001);
    ok &= Check(stats.captureFps == 0 && stats.programFps == 0 && stats.freshFps == 0 && stats.processingMs == 0 && stats.peakProcessingMs == 0,
                "empty samples clear stale rates");

    stats.Reset(start, 0);
    for (int second = 1; second <= 65; ++second)
    {
        stats.RecordPresent(second, second);
        stats.Update(start + seconds(second), static_cast<uint64_t>(second));
    }
    ok &= Check(stats.HistorySize() == 60, "graph history has a fixed memory bound");
    ok &= Check(stats.HistoryAt(0).at == start + seconds(6) && stats.HistoryAt(59).at == start + seconds(65),
                "graph history stays in chronological order after wrapping");
    ok &= Check(Near(stats.peakProcessingMs, 65), "old processing peaks do not leak into later samples");
    return ok ? 0 : 1;
}
