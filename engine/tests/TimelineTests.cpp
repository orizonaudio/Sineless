#include "AllocationCounter.h"
#include "SampleRates.h"
#include "engine/Timeline.h"

#include <array>
#include <atomic>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

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
    if (!testing::kCountsAllocations) {
        GTEST_SKIP() << "ThreadSanitizer's build counts no allocations (AllocationCounter.h)";
    }
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

// The positions recorded from `from` on, oldest first (Timeline::positionsSince).
std::vector<MusicalStamp> positionsSince(const Timeline& timeline, std::uint64_t from = 0)
{
    std::array<MusicalStamp, Timeline::kPositionChanges> records{};
    const std::size_t count = timeline.positionsSince(from, records);
    return {records.begin(), records.begin() + static_cast<std::ptrdiff_t>(count)};
}

// Where a recorded position places sample `index`: by the last record at or before it.
double placedQuarterNotes(const Timeline& timeline, std::uint64_t index, double rate)
{
    const auto records = positionsSince(timeline, index);
    return records.empty() ? std::numeric_limits<double>::quiet_NaN()
                           : Timeline::quarterNotesAt(records.front(), index, rate);
}

struct RecordCase
{
    double rate;
    double tempo;
};

class PositionRecords : public ::testing::TestWithParam<RecordCase>
{
};

// The audio thread records a position only where it does not continue the last one recorded:
// steady play with a host whose positions wander by a fraction of a sample records only its first
// block, at every rate and tempo; a position 1.1 samples off is recorded, one 0.9 samples off is
// not; a jump, a tempo step and a metre change are; free-running after a stop is not.
TEST_P(PositionRecords, RecordOnlyWhatDoesNotContinue)
{
    const auto [rate, tempo] = GetParam();
    constexpr int kSamples = 128;
    Timeline timeline;
    double offset = 0.0; // Samples the host's positions are shifted by.
    double base = 0.0;   // The quarter note of host sample 0.
    std::uint64_t index = 0;
    const auto play = [&](int blocks, double atTempo, int numerator = 4) {
        for (int block = 0; block < blocks; ++block) {
            const double jitter = 0.3 * std::sin(static_cast<double>(index) * 0.01);
            HostTimeline host = playingAt(base + (static_cast<double>(index) + offset + jitter) *
                                                     atTempo / (60.0 * rate),
                                          atTempo);
            host.numerator = numerator;
            timeline.advance(host, kSamples, rate);
            index += kSamples;
        }
    };
    play(3000, tempo);
    EXPECT_EQ(positionsSince(timeline).size(), 1U) << "steady play";
    offset += 1.1;
    play(10, tempo);
    EXPECT_EQ(positionsSince(timeline).size(), 2U) << "1.1 samples off";
    offset += 0.9;
    play(10, tempo);
    EXPECT_EQ(positionsSince(timeline).size(), 2U) << "0.9 samples off";
    base -= 1.0; // A jump back a beat.
    play(10, tempo);
    EXPECT_EQ(positionsSince(timeline).size(), 3U) << "a jump";
    // A tempo step: the position goes on from where the old tempo put it.
    base = timeline.latest().quarterNotes + kSamples * tempo / (60.0 * rate) -
           (static_cast<double>(index) + offset) * tempo * 1.25 / (60.0 * rate);
    const std::uint64_t stepped = index;
    play(10, tempo * 1.25);
    EXPECT_EQ(positionsSince(timeline).size(), 4U) << "a tempo step";
    EXPECT_EQ(positionsSince(timeline).back().sampleIndex, stepped) << "at its own block";
    // A tempo change at twice the tolerance, its position going on exactly: recorded at its block.
    const double smallStep = tempo * 1.25 * (1.0 + 2.0 * kSweepLengthTolerance);
    base = timeline.latest().quarterNotes + kSamples * tempo * 1.25 / (60.0 * rate) -
           (static_cast<double>(index) + offset) * smallStep / (60.0 * rate);
    const std::uint64_t nudged = index;
    play(10, smallStep);
    EXPECT_EQ(positionsSince(timeline).size(), 5U) << "a tempo change just over the tolerance";
    EXPECT_EQ(positionsSince(timeline).back().sampleIndex, nudged) << "at its own block";
    const std::uint64_t metre = index;
    play(10, smallStep, 3);
    EXPECT_EQ(positionsSince(timeline).size(), 6U) << "a metre change";
    EXPECT_EQ(positionsSince(timeline).back().sampleIndex, metre) << "at its own block";
    for (int block = 0; block < 100; ++block) {
        HostTimeline stopped = playingAt(7.0, smallStep);
        stopped.playing = false;
        stopped.numerator = 3;
        timeline.advance(stopped, kSamples, rate);
    }
    EXPECT_EQ(positionsSince(timeline).size(), 6U) << "free-running after a stop";
}

// A tempo change too small to start a new sweep (FR-9.4) is not recorded for its tempo, but the
// host's positions follow it, so each sample is still placed within a sample of where the host
// played it.
TEST_P(PositionRecords, PlaceEverySampleWithinASampleAcrossASmallTempoChange)
{
    const auto [rate, tempo] = GetParam();
    constexpr int kSamples = 128;
    const double later = tempo * (1.0 + 0.5 * kSweepLengthTolerance);
    Timeline timeline;
    std::vector<double> played;
    double position = 0.0;
    for (int block = 0; block < 4000; ++block) {
        const double atTempo = block < 100 ? tempo : later;
        timeline.advance(playingAt(position, atTempo), kSamples, rate);
        played.push_back(position);
        position += kSamples * atTempo / (60.0 * rate);
        // Eight blocks on, the first record's tempo still leads to within half a sample: a ramp
        // of such changes would otherwise record every block and lap the records in under a
        // second.
        if (block == 108) {
            EXPECT_EQ(positionsSince(timeline).size(), 1U) << "the small change is not recorded";
        }
    }
    for (std::size_t block = 0; block < played.size(); ++block) {
        const auto index = static_cast<std::uint64_t>(block) * kSamples;
        const double samplesOff =
            (placedQuarterNotes(timeline, index, rate) - played[block]) * 60.0 * rate / later;
        ASSERT_LE(std::abs(samplesOff), 1.0) << "block " << block;
    }
}

std::vector<RecordCase> recordCases()
{
    std::vector<RecordCase> cases;
    for (const double rate : kSampleRates) {
        for (const double tempo : {57.3, 120.0, 299.9}) {
            cases.push_back({.rate = rate, .tempo = tempo});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EveryRateAndTempo, PositionRecords, ::testing::ValuesIn(recordCases()),
                         [](const ::testing::TestParamInfo<RecordCase>& info) {
                             return std::to_string(static_cast<int>(info.param.rate)) + "Hz_" +
                                    std::to_string(static_cast<int>(info.param.tempo * 10)) +
                                    "dBpm";
                         });

// A position the host reports as not a number is recorded, not taken for a continuation.
TEST(Timeline, RecordsAPositionThatIsNotANumber)
{
    Timeline timeline;
    timeline.advance(playingAt(0.0), kBlock, kRate);
    timeline.advance(playingAt(std::numeric_limits<double>::quiet_NaN()), kBlock, kRate);
    EXPECT_EQ(positionsSince(timeline).size(), 2U);
}

// A timeline is anchored once it has played a block with a host position, and stays so through
// stops: its free-run then goes on from a position every instance shares. A position reported
// while stopped, which an instance takes on its first block, does not anchor it.
TEST(Timeline, IsAnchoredOnlyOnceItHasPlayedWithAPosition)
{
    Timeline timeline;
    HostTimeline none;
    timeline.advance(none, kBlock, kRate);
    EXPECT_FALSE(timeline.latest().anchored) << "no position";
    HostTimeline stopped = playingAt(8.0);
    stopped.playing = false;
    timeline.advance(stopped, kBlock, kRate);
    EXPECT_FALSE(timeline.latest().anchored) << "a position reported while stopped";
    // Played from where the free-run has reached: only the anchoring makes it a new record.
    const std::size_t before = positionsSince(timeline).size();
    const MusicalStamp running = timeline.latest();
    timeline.advance(
        playingAt(Timeline::quarterNotesAt(running, running.sampleIndex + kBlock, kRate)), kBlock,
        kRate);
    EXPECT_TRUE(timeline.latest().anchored) << "played with a position";
    EXPECT_EQ(positionsSince(timeline).size(), before + 1) << "the anchoring block is recorded";
    timeline.advance(stopped, kBlock, kRate);
    EXPECT_TRUE(timeline.latest().anchored) << "stopped after playing";
    EXPECT_TRUE(positionsSince(timeline).back().anchored);
}

// The ring keeps the newest kPositionChanges records; a sample before the oldest kept is placed by
// that oldest.
TEST(Timeline, KeepsTheNewestPositionChanges)
{
    Timeline timeline;
    constexpr int kChanges = static_cast<int>(Timeline::kPositionChanges) + 44;
    for (int change = 0; change < kChanges; ++change) {
        timeline.advance(playingAt(change % 2 == 0 ? 0.0 : 100.0), kBlock, kRate);
    }
    const auto records = positionsSince(timeline);
    ASSERT_EQ(records.size(), Timeline::kPositionChanges);
    EXPECT_EQ(records.front().sampleIndex, 44U * kBlock) << "the oldest kept";
    EXPECT_EQ(records.back().sampleIndex, static_cast<std::uint64_t>(kChanges - 1) * kBlock);
    EXPECT_EQ(positionsSince(timeline, 0).front().sampleIndex, 44U * kBlock);
}

// The positions from an index on start with the one in effect there: the record at that index,
// or the last before it, or the oldest kept for an index before them all.
TEST(Timeline, GivesThePositionsInEffectFromAnIndexOn)
{
    Timeline timeline;
    for (const double position : {0.0, 10.0, 20.0, 30.0}) { // Four records, a block apart.
        timeline.advance(playingAt(position), kBlock, kRate);
    }
    ASSERT_EQ(positionsSince(timeline).size(), 4U);
    EXPECT_EQ(positionsSince(timeline, std::uint64_t{2} * kBlock).front().sampleIndex,
              std::uint64_t{2} * kBlock)
        << "at one";
    EXPECT_EQ(positionsSince(timeline, std::uint64_t{2} * kBlock).size(), 2U);
    EXPECT_EQ(positionsSince(timeline, std::uint64_t{2} * kBlock + 7).front().sampleIndex,
              std::uint64_t{2} * kBlock)
        << "between two";
    EXPECT_EQ(positionsSince(timeline, std::uint64_t{10} * kBlock).size(), 1U)
        << "after the newest";
}

// A different sample rate (the processor prepared again) drops the positions recorded at the old
// one: they would extrapolate at the wrong rate.
TEST(Timeline, DropsThePositionsOfAnotherRate)
{
    Timeline timeline;
    timeline.advance(playingAt(0.0), kBlock, kRate);
    timeline.advance(playingAt(10.0), kBlock, kRate);
    ASSERT_EQ(positionsSince(timeline).size(), 2U);
    timeline.advance(playingAt(10.0), kBlock, 2.0 * kRate);
    const auto records = positionsSince(timeline);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records.front().sampleIndex, 2U * kBlock);
}

TEST(Timeline, RecordingAPositionDoesNotAllocate)
{
    if (!testing::kCountsAllocations) {
        GTEST_SKIP() << "ThreadSanitizer's build counts no allocations (AllocationCounter.h)";
    }
    Timeline timeline;
    std::size_t operations = 0;
    {
        testing::AllocationCounter counter;
        timeline.advance(playingAt(1.0), kBlock, kRate);
        timeline.advance(playingAt(9.0), kBlock, kRate); // A jump: recorded.
        operations = counter.allocations() + counter.deallocations();
    }
    EXPECT_EQ(operations, 0U);
}

// Readers never see a list mixed from two laps of the ring: while every block jumps (so every
// block is recorded, and the ring is lapped over and over), each list read is in order and each
// record whole (its position is its index / 1000, plus 0 or 1).
TEST(Timeline, ConcurrentReadsSeeConsistentPositionLists)
{
    constexpr int kReads = 200'000;
    Timeline timeline;
    std::atomic<int> reads{0};
    std::thread writer([&] {
        for (std::uint64_t block = 0; reads.load(std::memory_order_relaxed) < kReads; ++block) {
            const auto index = static_cast<double>(block * kBlock);
            timeline.advance(playingAt(index / 1000.0 + static_cast<double>(block % 2)), kBlock,
                             kRate);
        }
    });
    int inconsistent = 0;
    while (reads.load(std::memory_order_relaxed) < kReads) {
        const MusicalStamp newest = timeline.latest();
        const std::uint64_t from = newest.sampleIndex > std::uint64_t{50} * kBlock
                                       ? newest.sampleIndex - std::uint64_t{50} * kBlock
                                       : 0;
        const auto records = positionsSince(timeline, from);
        for (std::size_t i = 0; i < records.size(); ++i) {
            const double extra =
                records[i].quarterNotes - static_cast<double>(records[i].sampleIndex) / 1000.0;
            if ((std::abs(extra) > 1e-6 && std::abs(extra - 1.0) > 1e-6) ||
                (i > 0 && records[i].sampleIndex <= records[i - 1].sampleIndex)) {
                ++inconsistent;
            }
        }
        reads.fetch_add(1, std::memory_order_relaxed);
    }
    writer.join();
    EXPECT_EQ(inconsistent, 0);
}

} // namespace
} // namespace orizon::engine
