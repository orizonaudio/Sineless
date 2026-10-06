#include "AllocationCounter.h"
#include "engine/Timeline.h"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>

namespace orizon::engine {
namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 480; // 10 ms.

HostTimeline playingAt(double quarterNotes, double tempo = 120.0)
{
    return {.playing = true,
            .positionValid = true,
            .quarterNotes = quarterNotes,
            .tempoValid = true,
            .tempo = tempo,
            .timeSignatureValid = true,
            .numerator = 3,
            .denominator = 4};
}

TEST(Timeline, FollowsTheHostWhilePlaying)
{
    Timeline timeline;
    timeline.advance(playingAt(8.0), kBlock, kRate);
    timeline.advance(playingAt(8.02), kBlock, kRate);
    const MusicalStamp stamp = timeline.latest();
    EXPECT_EQ(stamp.sampleIndex, static_cast<std::uint64_t>(kBlock));
    EXPECT_DOUBLE_EQ(stamp.quarterNotes, 8.02);
    EXPECT_TRUE(stamp.playing);
    EXPECT_EQ(stamp.numerator, 3);
    EXPECT_DOUBLE_EQ(Timeline::barQuarterNotes(stamp), 3.0);
}

TEST(Timeline, KeepsRunningAtTheLastTempoWhileStopped)
{
    Timeline timeline;
    timeline.advance(playingAt(4.0, 120.0), kBlock, kRate); // 10 ms at 2 beats/s = 0.02 beats.
    HostTimeline stopped = playingAt(4.0, 120.0);
    stopped.playing = false;
    for (int block = 0; block < 100; ++block) {
        timeline.advance(stopped, kBlock, kRate);
    }
    const MusicalStamp stamp = timeline.latest();
    EXPECT_FALSE(stamp.playing);
    EXPECT_NEAR(stamp.quarterNotes, 4.0 + 100 * 0.02, 1e-9);
}

TEST(Timeline, SnapsBackToTheHostWhenPlaybackStarts)
{
    Timeline timeline;
    HostTimeline stopped = playingAt(0.0);
    stopped.playing = false;
    for (int block = 0; block < 10; ++block) {
        timeline.advance(stopped, kBlock, kRate);
    }
    timeline.advance(playingAt(16.0), kBlock, kRate);
    EXPECT_DOUBLE_EQ(timeline.latest().quarterNotes, 16.0);
}

TEST(Timeline, FreeRunsAt120BpmInFourFourWithoutHostInformation)
{
    Timeline timeline;
    for (int block = 0; block < 50; ++block) {
        timeline.advance({}, kBlock, kRate);
    }
    const MusicalStamp stamp = timeline.latest();
    EXPECT_DOUBLE_EQ(stamp.tempo, 120.0);
    EXPECT_DOUBLE_EQ(Timeline::barQuarterNotes(stamp), 4.0);
    EXPECT_NEAR(stamp.quarterNotes, 49 * 0.02, 1e-9);
}

TEST(Timeline, MapsBetweenSamplesAndQuarterNotes)
{
    const MusicalStamp stamp{.sampleIndex = 48000, .quarterNotes = 10.0, .tempo = 90.0};
    // 90 BPM: 1.5 beats per second.
    EXPECT_DOUBLE_EQ(Timeline::quarterNotesAt(stamp, 96000, kRate), 11.5);
    EXPECT_DOUBLE_EQ(Timeline::quarterNotesAt(stamp, 24000, kRate), 9.25);
    EXPECT_DOUBLE_EQ(Timeline::sampleIndexAt(stamp, 11.5, kRate), 96000.0);
}

TEST(Timeline, AdvanceDoesNotAllocate)
{
    Timeline timeline;
    std::size_t operations = 0;
    {
        testing::AllocationCounter counter;
        timeline.advance(playingAt(1.0), kBlock, kRate);
        operations = counter.allocations() + counter.deallocations();
    }
    EXPECT_EQ(operations, 0u);
}

// Readers never see a stamp mixed from two writes: here the position always equals
// sampleIndex / 1000 and the tempo always equals the position.
TEST(Timeline, ConcurrentReadsSeeConsistentStamps)
{
    Timeline timeline;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int block = 0; block < 200'000; ++block) {
            const double position = static_cast<double>(block) * kBlock / 1000.0;
            timeline.advance(playingAt(position, 1.0 + position), kBlock, kRate);
        }
        done = true;
    });
    int inconsistent = 0;
    while (!done) {
        const MusicalStamp stamp = timeline.latest();
        if (stamp.quarterNotes != static_cast<double>(stamp.sampleIndex) / 1000.0 ||
            (stamp.sampleIndex > 0 && stamp.tempo != 1.0 + stamp.quarterNotes)) {
            ++inconsistent;
        }
    }
    writer.join();
    EXPECT_EQ(inconsistent, 0);
}

} // namespace
} // namespace orizon::engine
