#include "SampleRates.h"
#include "SimulatedHost.h"
#include "engine/BlockClock.h"
#include "engine/Motion.h"
#include "protocol/Protocol.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace orizon::engine {
namespace {

using testing::Arrivals;
using testing::HostPattern;

// The bounds of even motion: per frame, the edge moves within 1 ms of where steady motion takes
// it at the 99th percentile, and within 2 ms always.
constexpr double kAdvanceErrorP99 = 1.0;
constexpr double kAdvanceErrorMax = 2.0;
// Where a host's longest gap between deliveries is at most this share of the largest delay, the
// loop's ripple stays within the bounds; above it, bursts of large blocks reach 2.5 ms.
constexpr double kEligibleGapShare = 0.6;
// How long the edge settles after the clock locks: it holds once until the delay is behind it.
constexpr double kSettleSeconds = 1.0;

std::vector<EdgeDraw> settled(std::vector<EdgeDraw> draws, double lockedAt)
{
    std::erase_if(draws,
                  [&](const EdgeDraw& draw) { return draw.seconds < lockedAt + kSettleSeconds; });
    return draws;
}

struct Played
{
    std::vector<EdgeDraw> paced;
    std::vector<EdgeDraw> newest;
    double lockedAt = 1e9;
};

Played play(const HostPattern& pattern)
{
    PacedEdge edge;
    Played played;
    played.paced = testing::simulateHost(
        pattern, [&](const auto& stamp) { edge.observe(stamp, pattern.rate); },
        [&](std::uint64_t nanos, std::uint64_t newest) {
            const auto shown =
                static_cast<double>(edge.edge(nanos, newest, protocol::kPacingDelayCapSeconds));
            if (edge.paced() && played.lockedAt > 1e8) {
                played.lockedAt = static_cast<double>(nanos) / 1e9;
            }
            return shown;
        });
    for (const EdgeDraw& draw : played.paced) {
        played.newest.push_back(
            {.seconds = draw.seconds, .edge = draw.newest, .newest = draw.newest});
    }
    return played;
}

class EvenMotion : public ::testing::TestWithParam<HostPattern>
{
};

// The paced edge never runs past the audio or backwards, on any simulated host. Where the host's
// deliveries fit the largest delay, it moves evenly, frame by frame, and trails the audio by no
// more than that delay. Where blocks last 5 ms or more, the newest sample, as the edge stood
// before, does not: the measure tells the two apart.
TEST_P(EvenMotion, MovesTheEdgeEvenly)
{
    const HostPattern& pattern = GetParam();
    const Played played = play(pattern);
    const Motion all = measureMotion(played.paced, pattern.rate);
    EXPECT_FALSE(all.backwards);
    EXPECT_FALSE(all.ahead);
    if (pattern.largestGapSeconds() <= kEligibleGapShare * protocol::kPacingDelayCapSeconds) {
        EXPECT_LE(all.lagMax, protocol::kPacingDelayCapSeconds * 1000.0 + 2.0);
        ASSERT_LT(played.lockedAt, 3.0) << "the clock locks within two seconds";
        const Motion paced = measureMotion(settled(played.paced, played.lockedAt), pattern.rate);
        EXPECT_LE(paced.advanceErrorP99, kAdvanceErrorP99);
        EXPECT_LE(paced.advanceErrorMax, kAdvanceErrorMax);
    }
    if (pattern.periodSeconds() >= 0.005) {
        const Motion newest = measureMotion(settled(played.newest, played.lockedAt), pattern.rate);
        EXPECT_TRUE(newest.advanceErrorP99 > kAdvanceErrorP99 ||
                    newest.advanceErrorMax > kAdvanceErrorMax)
            << "the newest sample moves evenly too: the measure cannot tell";
    }
}

// The measure tells each way an edge can go wrong, at every rate: drawn at 60 Hz, 10 ms behind
// the newest audio and moving steadily, nothing is wrong; one frame that moves back, one past the
// newest audio, and one that stands still are each reported, and only as what they are.
TEST(MeasureMotion, ReportsEachWayAnEdgeGoesWrong)
{
    constexpr double kFrame = 1.0 / 60.0;
    for (const double rate : kSampleRates) {
        SCOPED_TRACE(rate);
        const auto steady = [&] {
            std::vector<EdgeDraw> draws;
            for (int index = 0; index < 120; ++index) {
                const double seconds = 1.0 + index * kFrame;
                draws.push_back({.seconds = seconds,
                                 .edge = (seconds - 0.01) * rate,
                                 .newest = seconds * rate});
            }
            return draws;
        };
        const Motion even = measureMotion(steady(), rate);
        EXPECT_NEAR(even.advanceErrorMax, 0.0, 1e-6);
        EXPECT_NEAR(even.lagMax, 10.0, 1e-6);
        EXPECT_FALSE(even.backwards);
        EXPECT_FALSE(even.ahead);

        auto back = steady();
        back[60].edge = back[59].edge - 1.0;
        const Motion backwards = measureMotion(back, rate);
        EXPECT_TRUE(backwards.backwards);
        EXPECT_FALSE(backwards.ahead);

        auto past = steady();
        past[60].edge = past[60].newest + 1.0;
        const Motion ahead = measureMotion(past, rate);
        EXPECT_TRUE(ahead.ahead);
        EXPECT_FALSE(ahead.backwards);

        // Frame 60 stands still, frame 61 moves two frames' worth: a stutter of a frame each way.
        auto jump = steady();
        jump[60].edge = jump[59].edge;
        const Motion stutter = measureMotion(jump, rate);
        EXPECT_NEAR(stutter.advanceErrorMax, kFrame * 1000.0, 1e-6);
        EXPECT_FALSE(stutter.backwards);
        EXPECT_FALSE(stutter.ahead);
    }
}

std::vector<HostPattern> patterns()
{
    std::vector<HostPattern> result;
    for (const double rate : kSampleRates) {
        for (std::uint64_t block = 32; block <= 4096; block *= 2) {
            for (const Arrivals arrivals :
                 {Arrivals::kSteady, Arrivals::kJittered, Arrivals::kSplit, Arrivals::kBurst}) {
                // Jitter of a quarter period, for periods up to 12 ms.
                if (arrivals == Arrivals::kJittered && static_cast<double>(block) / rate > 0.012) {
                    continue;
                }
                for (const double draws : {30.0, 60.0, 120.0}) {
                    result.push_back(
                        {.block = block, .rate = rate, .arrivals = arrivals, .drawHertz = draws});
                }
            }
        }
    }
    return result;
}

INSTANTIATE_TEST_SUITE_P(SimulatedHosts, EvenMotion, ::testing::ValuesIn(patterns()),
                         [](const ::testing::TestParamInfo<HostPattern>& info) {
                             const HostPattern& p = info.param;
                             return std::to_string(p.block) + "At" +
                                    std::to_string(static_cast<int>(p.rate)) +
                                    std::string(testing::arrivalsName(p.arrivals)) +
                                    std::to_string(static_cast<int>(p.drawHertz)) + "Hz";
                         });

} // namespace
} // namespace orizon::engine
