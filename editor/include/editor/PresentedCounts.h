#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace orizon::editor {

// What reached the screen, from Metal's presented times (MTLDrawable presentedTime, host seconds,
// 0.0 when the drawable was dropped): frames presented, drawables dropped, and refreshes that
// showed no new frame, counted from the interval between consecutive presented times in display
// periods (half up; one is steady). A dropped drawable is no time: the interval runs on from the
// last frame shown. The first frame, or any before the display's period is known, starts an
// interval but judges none. Only interval rounding is done, so the period's small wander does not
// accumulate. Fed from Metal's presented handlers; read from any thread.
class PresentedCounts
{
public:
    struct Totals
    {
        std::uint64_t presented = 0;
        std::uint64_t dropped = 0;
        std::uint64_t refreshesWithoutNewFrame = 0;
    };

    // `periodSeconds`: the display's refresh period when the drawable was drawn; 0 if unknown.
    void add(double presentedTime, double periodSeconds);
    // The link paused (hidden, occluded): the next frame starts an interval rather than counting
    // the pause as refreshes without a new frame.
    void restart() { last.store(0.0); }
    [[nodiscard]] Totals totals() const;

private:
    std::atomic<std::uint64_t> presented{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> withoutNewFrame{0};
    std::atomic<double> last{0.0};
};

// How long pictures take from the display link's callback that drew them to the screen (Metal's
// presented time less the callback's, in seconds): the median of the last kPictures presented, so
// one picture shown late does not move it and a change of a whole refresh does. NaN until a
// picture is presented, and again after a restart. Fed from Metal's presented handlers; read from
// any thread (never the audio thread: it locks).
class PresentedDelay
{
public:
    // The pictures the median is taken over: a second of them at 60 Hz, long enough to ride over
    // a hiccup and short enough to follow the display within a test's or a benchmark's wait.
    static constexpr std::size_t kPictures = 60;

    // A presented picture's delay. A dropped drawable has none and is not added.
    void add(double seconds);
    // The link paused (hidden, occluded): the pictures before it say nothing of those after.
    void restart();
    [[nodiscard]] double medianSeconds() const;

private:
    mutable std::mutex mutex;
    std::array<double, kPictures> delays{};
    std::size_t count = 0;
    std::size_t next = 0;
};

} // namespace orizon::editor
