#include "AllocationCounter.h"
#include "engine/Timeline.h"

#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <string>
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

// A host may mark a tempo or time signature valid and still report 0 (or less) in it, as when a
// project is loading. Each such value is ignored and the last usable one kept, so the beat span and
// the sweep stay finite instead of dividing by zero.
TEST(Timeline, KeepsTheLastUsableTempoAndTimeSignature)
{
    struct Case
    {
        const char* name;
        void (*corrupt)(HostTimeline&);
    };
    const std::array cases{
        Case{.name = "tempo 0", .corrupt = [](HostTimeline& host) { host.tempo = 0.0; }},
        Case{.name = "negative tempo", .corrupt = [](HostTimeline& host) { host.tempo = -120.0; }},
        Case{.name = "numerator 0", .corrupt = [](HostTimeline& host) { host.numerator = 0; }},
        Case{.name = "denominator 0", .corrupt = [](HostTimeline& host) { host.denominator = 0; }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        Timeline timeline;
        timeline.advance(playingAt(8.0, 90.0), kBlock, kRate);
        HostTimeline host = playingAt(8.02, 90.0);
        c.corrupt(host);
        timeline.advance(host, kBlock, kRate);
        const MusicalStamp stamp = timeline.latest();
        EXPECT_DOUBLE_EQ(stamp.tempo, 90.0);
        EXPECT_EQ(stamp.numerator, 3);
        EXPECT_EQ(stamp.denominator, 4);
        EXPECT_DOUBLE_EQ(Timeline::barQuarterNotes(stamp), 3.0);
    }
}

TEST(Timeline, KnowsTheProjectTime)
{
    Timeline timeline;
    HostTimeline host = playingAt(8.0); // 4 s at 120 BPM.
    host.secondsValid = true;
    host.seconds = 4.0;
    timeline.advance(host, kBlock, kRate);
    MusicalStamp stamp = timeline.latest();
    EXPECT_DOUBLE_EQ(stamp.seconds, 4.0);
    EXPECT_DOUBLE_EQ(Timeline::secondsAt(stamp, 10.0), 5.0) << "two beats later at 2 beats/s";

    host.playing = false;
    for (int block = 0; block < 100; ++block) {
        timeline.advance(host, kBlock, kRate);
    }
    stamp = timeline.latest();
    EXPECT_NEAR(stamp.seconds, 5.0, 1e-9) << "stopped, it runs on with the clock";
}

// Before the host ever plays, the left edge's project time is where the host says it stands:
// a project located at 4 s shows 4 s, not the start of the project.
TEST(Timeline, StartsAtTheHostPositionBeforeTheFirstPlay)
{
    Timeline timeline;
    HostTimeline host = playingAt(8.0);
    host.playing = false;
    host.secondsValid = true;
    host.seconds = 4.0;
    timeline.advance(host, kBlock, kRate);
    const MusicalStamp stamp = timeline.latest();
    EXPECT_FALSE(stamp.playing);
    EXPECT_DOUBLE_EQ(stamp.quarterNotes, 8.0);
    EXPECT_DOUBLE_EQ(stamp.seconds, 4.0);
}

// A position the host has not flagged as valid is not taken, whatever it holds.
TEST(Timeline, StartsAtTheProjectStartWithoutAHostPosition)
{
    Timeline timeline;
    timeline.advance({.quarterNotes = 8.0, .seconds = 4.0}, kBlock, kRate);
    const MusicalStamp stamp = timeline.latest();
    EXPECT_DOUBLE_EQ(stamp.quarterNotes, 0.0);
    EXPECT_DOUBLE_EQ(stamp.seconds, 0.0);
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

struct Meter
{
    int numerator;
    int denominator;
    double barQuarterNotes;
};

class TimeSignature : public ::testing::TestWithParam<Meter>
{
};

// A bar lasts as long as the host's time signature says, so the beat grid and bar spans line up
// with the host's bars in compound and odd meters, not only in quarter-note ones.
TEST_P(TimeSignature, BarsLastAsLongAsTheHostsBars)
{
    const Meter meter = GetParam();
    Timeline timeline;
    HostTimeline host = playingAt(0.0);
    host.numerator = meter.numerator;
    host.denominator = meter.denominator;
    timeline.advance(host, kBlock, kRate);
    const MusicalStamp stamp = timeline.latest();
    EXPECT_EQ(stamp.numerator, meter.numerator);
    EXPECT_EQ(stamp.denominator, meter.denominator);
    EXPECT_DOUBLE_EQ(Timeline::barQuarterNotes(stamp), meter.barQuarterNotes);
}

INSTANTIATE_TEST_SUITE_P(Meters, TimeSignature,
                         ::testing::Values(Meter{4, 4, 4.0}, Meter{3, 4, 3.0}, Meter{6, 8, 3.0},
                                           Meter{7, 8, 3.5}, Meter{5, 16, 1.25}, Meter{2, 2, 4.0}),
                         [](const ::testing::TestParamInfo<Meter>& info) {
                             return std::to_string(info.param.numerator) + "_" +
                                    std::to_string(info.param.denominator);
                         });

// A host that stops sending a time signature, or sends one with a zero, leaves the bars as they
// were rather than making them endless.
TEST(Timeline, KeepsTheMeterWhenTheHostSendsNoValidOne)
{
    Timeline timeline;
    HostTimeline host = playingAt(0.0);
    host.numerator = 6;
    host.denominator = 8;
    timeline.advance(host, kBlock, kRate);

    host.timeSignatureValid = false;
    host.numerator = 4;
    host.denominator = 4;
    timeline.advance(host, kBlock, kRate);
    EXPECT_DOUBLE_EQ(Timeline::barQuarterNotes(timeline.latest()), 3.0) << "not sent";

    host.timeSignatureValid = true;
    host.denominator = 0;
    timeline.advance(host, kBlock, kRate);
    EXPECT_DOUBLE_EQ(Timeline::barQuarterNotes(timeline.latest()), 3.0) << "a denominator of 0";

    host.numerator = 0;
    host.denominator = 4;
    timeline.advance(host, kBlock, kRate);
    EXPECT_DOUBLE_EQ(Timeline::barQuarterNotes(timeline.latest()), 3.0) << "a numerator of 0";
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
