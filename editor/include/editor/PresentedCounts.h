#pragma once

#include <atomic>
#include <cstdint>

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

} // namespace orizon::editor
