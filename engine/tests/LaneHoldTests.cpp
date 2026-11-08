#include "engine/Engine.h"
#include "engine/LaneHold.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace orizon::engine {
namespace {

// A lane held on freeze is one channel of its track, the lane channel, so that a Side lane is the
// Side of what was captured and not derived again from a copy of itself.
TEST(LaneHold, HoldsTheChannelItWasCapturedWith)
{
    Engine engine;
    engine.prepare(48000.0);
    std::vector<float> left(1000, 0.5f);
    std::vector<float> right(1000, 0.25f);
    std::array<const float*, 2> channels{left.data(), right.data()};
    engine.captureRing().write(channels.data(), 2, 1000);

    std::vector<float> scratch;
    for (const auto [view, expected] : {std::pair{protocol::ChannelView::kLeft, 0.5f},
                                        std::pair{protocol::ChannelView::kRight, 0.25f},
                                        std::pair{protocol::ChannelView::kMid, 0.375f},
                                        std::pair{protocol::ChannelView::kSide, 0.125f}}) {
        SCOPED_TRACE(static_cast<int>(view));
        LaneHold hold;
        ASSERT_TRUE(hold.capture(engine, view, 400, scratch));
        EXPECT_EQ(hold.channels(), 1);
        EXPECT_EQ(hold.endIndex(), 1000u);
        EXPECT_EQ(hold.beginIndex(), 600u);
        std::vector<float> samples(400);
        ASSERT_TRUE(hold.read(0, 600, samples));
        EXPECT_EQ(samples, std::vector<float>(400, expected));
        EXPECT_FALSE(hold.read(1, 600, samples)) << "one channel: no second to read";
    }
}

} // namespace
} // namespace orizon::engine
