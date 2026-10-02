#pragma once

#include <cstdint>

// Holds frames to a rate. Frames follow a schedule of one per interval rather than the time since the last one,
// so the rate is met on average although frames come at uneven times.
struct FrameLimit
{
    // Whether a frame may be shown at time, which only moves forward, with at least interval between frames on
    // average. Both are in the same unit.
    bool Allow(int64_t time, int64_t interval)
    {
        // Up to half an interval early counts as on time. A game running at the limit keeps every frame that way,
        // although its frames arrive a little early as often as late.
        if (time < next - interval / 2)
            return false;
        // A frame more than half an interval late starts the schedule again, so the time lost is not made up for
        // with a burst.
        next = (time > next + interval / 2 ? time : next) + interval;
        return true;
    }

private:
    int64_t next = 0;
};
