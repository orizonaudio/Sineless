#include "SimulatedHost.h"
#include "engine/BlockClock.h"
#include "engine/MonotonicClock.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <thread>
#include <vector>

namespace orizon::engine {
namespace {

constexpr std::uint64_t kSecond = 1'000'000'000;

// Steady blocks of `block` samples at `rate`, from 1 s, each on time.
struct SteadyHost
{
    std::uint64_t block = 512;
    double rate = 48000.0;
    std::uint64_t delivered = 0;
    std::uint64_t lateNanos = 0; // Added to the next delivery only.

    BlockStampValue next()
    {
        ++delivered;
        const auto due = static_cast<double>(delivered * block) / rate;
        const BlockStampValue stamp{.index = delivered * block,
                                    .nanos =
                                        kSecond + static_cast<std::uint64_t>(due * 1e9) + lateNanos,
                                    .paced = true};
        lateNanos = 0;
        return stamp;
    }
};

void feed(BlockClock& clock, SteadyHost& host, int blocks)
{
    for (int i = 0; i < blocks; ++i) {
        clock.observe(host.next(), host.rate);
    }
}

TEST(MonotonicClock, AdvancesInNanosecondsWithRealTime)
{
    const std::uint64_t before = monotonicNanos();
    const auto started = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t after = monotonicNanos();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    EXPECT_GE(after - before, 20'000'000U);
    EXPECT_NEAR(static_cast<double>(after - before), static_cast<double>(elapsed), 5e6)
        << "in step with steady_clock: the same clock, in nanoseconds";
}

// The analysis thread never reads a stamp mixed from two blocks while the audio thread publishes
// them: one block's index with another's time would look like a jump or a stall to the clock,
// which would start over and leave the edge unpaced. Here each block's time and flag follow from
// its index.
TEST(BlockStamp, ConcurrentReadsSeeWholeStamps)
{
    constexpr int kReads = 1'000'000;
    BlockStamp stamp;
    std::atomic<int> read{0};
    // The writer publishes until the reader has read enough, so the two always overlap.
    std::thread writer([&] {
        for (std::uint64_t block = 1; read.load(std::memory_order_relaxed) < kReads; ++block) {
            stamp.publish({.index = block * 512, .nanos = block * 7 + 3, .paced = block % 2 == 0});
        }
    });
    int mixed = 0;
    while (read.load(std::memory_order_relaxed) < kReads) {
        if (const auto latest = stamp.latest()) {
            const std::uint64_t block = latest->index / 512;
            if (latest->index % 512 != 0 || latest->nanos != block * 7 + 3 ||
                latest->paced != (block % 2 == 0)) {
                ++mixed;
            }
            read.fetch_add(1, std::memory_order_relaxed);
        }
    }
    writer.join();
    EXPECT_EQ(mixed, 0);
}

// A steady host locks the clock, which then says which sample is due at any time within a sample:
// small blocks at the highest rate, and the longest blocks, whose period no gap rule resets.
TEST(BlockClock, LocksOnASteadyHostAndEstimatesWithinASample)
{
    for (const auto [block, rate] :
         {std::pair{32ULL, 384000.0}, std::pair{512ULL, 48000.0}, std::pair{4096ULL, 44100.0}}) {
        SCOPED_TRACE(block);
        BlockClock clock;
        SteadyHost host{.block = block, .rate = rate};
        feed(clock, host, 4);
        EXPECT_FALSE(clock.locked()) << "a few blocks are not enough to settle";
        feed(clock, host, 60);
        ASSERT_TRUE(clock.locked());
        EXPECT_EQ(clock.restarts(), 0U);
        const std::uint64_t at =
            kSecond + static_cast<std::uint64_t>(
                          (static_cast<double>(host.delivered * block) + 100.5) / rate * 1e9);
        EXPECT_NEAR(clock.estimate(at), static_cast<double>(host.delivered * block) + 100.5, 1.0);
        EXPECT_NEAR(clock.lastLead(), static_cast<double>(block), 1.0)
            << "the line ran a block past the audio when the next came";
    }
}

TEST(BlockClock, IgnoresAStampWithNoNewSamples)
{
    BlockClock clock;
    SteadyHost host;
    feed(clock, host, 30);
    const BlockStampValue same{
        .index = host.delivered * host.block, .nanos = kSecond * 5, .paced = true};
    EXPECT_FALSE(clock.observe(same, host.rate));
    EXPECT_TRUE(clock.locked());
    EXPECT_EQ(clock.restarts(), 0U);
}

// A host that stops calling process, a stall or a jump, a rate change and unpaced (offline)
// processing each start the clock over, unlocked; it locks again on steady blocks after.
TEST(BlockClock, StartsOverOnAStallAJumpARateChangeAndOfflineBlocks)
{
    const auto lockedThen = [](BlockClock& clock, SteadyHost& host) {
        EXPECT_FALSE(clock.locked());
        feed(clock, host, 40);
        EXPECT_TRUE(clock.locked()) << "locks again";
    };
    {
        SCOPED_TRACE("stall");
        BlockClock clock;
        SteadyHost host;
        feed(clock, host, 30);
        host.lateNanos = 200'000'000;
        clock.observe(host.next(), host.rate);
        EXPECT_EQ(clock.restarts(), 1U);
        lockedThen(clock, host);
    }
    {
        SCOPED_TRACE("rate");
        BlockClock clock;
        SteadyHost host;
        feed(clock, host, 30);
        host.rate = 96000.0;
        clock.observe(host.next(), host.rate);
        EXPECT_EQ(clock.restarts(), 1U);
    }
    {
        SCOPED_TRACE("offline");
        BlockClock clock;
        SteadyHost host;
        feed(clock, host, 30);
        BlockStampValue offline = host.next();
        offline.paced = false;
        EXPECT_FALSE(clock.observe(offline, host.rate));
        EXPECT_FALSE(clock.locked());
        lockedThen(clock, host);
    }
    {
        SCOPED_TRACE("no rate");
        BlockClock clock;
        SteadyHost host;
        for (const double rate : {0.0, std::nan("")}) {
            clock.observe(host.next(), rate);
            EXPECT_FALSE(clock.locked());
        }
    }
}

// A late block: within the larger of two periods and 5 ms the loop absorbs it; beyond, it starts
// over. At 64 samples (1.3 ms, so 5 ms counts) and 2048 (46 ms, so the periods count; four
// periods, the gap rule, are further).
TEST(BlockClock, AbsorbsALateBlockUpToTheJumpThreshold)
{
    for (const auto [block, rate, threshold] :
         {std::tuple{64ULL, 48000.0, 0.005},
          std::tuple{2048ULL, 44100.0, 2.0 * 2048.0 / 44100.0}}) {
        SCOPED_TRACE(block);
        for (const double share : {0.9, 1.1}) {
            BlockClock clock;
            SteadyHost host{.block = block, .rate = rate};
            feed(clock, host, 30);
            host.lateNanos = static_cast<std::uint64_t>(share * threshold * 1e9);
            clock.observe(host.next(), rate);
            EXPECT_EQ(clock.restarts(), share > 1.0 ? 1U : 0U) << share;
        }
    }
}

// A gap just short of the stall rule keeps the clock; just past it starts over. The rule is the
// larger of 50 ms and four periods: 50 ms at 512 samples, four periods (372 ms) at 4096.
TEST(BlockClock, StartsOverOnlyAfterAGap)
{
    for (const auto [block, rate] : {std::pair{512ULL, 48000.0}, std::pair{4096ULL, 44100.0}}) {
        SCOPED_TRACE(block);
        const double period = static_cast<double>(block) / rate;
        const double gap = std::max(BlockClock::kGapSeconds, 4.0 * period);
        BlockClock clock;
        SteadyHost host{.block = block, .rate = rate};
        feed(clock, host, 30);
        const std::uint64_t last = host.next().nanos;
        clock.observe({.index = host.delivered * block, .nanos = last, .paced = true}, rate);
        EXPECT_FALSE(clock.stalled(last + static_cast<std::uint64_t>(0.95 * gap * 1e9)));
        EXPECT_TRUE(clock.stalled(last + static_cast<std::uint64_t>(1.05 * gap * 1e9)));
    }
}

// The paced edge: the newest sample until the clock locks, offline and when the host stalls;
// paced, never past the audio or backwards, and a delay that starts again after the clock does.
TEST(PacedEdge, IsTheNewestSampleUnlessTheClockIsLocked)
{
    PacedEdge edge;
    SteadyHost host;
    const double cap = protocol::kPacingDelayCapSeconds;
    BlockStampValue stamp = host.next();
    edge.observe(stamp, host.rate);
    EXPECT_EQ(edge.edge(stamp.nanos + 1'000'000, stamp.index, cap), stamp.index);
    EXPECT_FALSE(edge.paced()) << "not locked yet";
    for (int i = 0; i < 40; ++i) {
        stamp = host.next();
        edge.observe(stamp, host.rate);
    }
    const std::uint64_t paced = edge.edge(stamp.nanos + 5'000'000, stamp.index, cap);
    EXPECT_TRUE(edge.paced());
    EXPECT_LT(paced, stamp.index);
    EXPECT_GT(edge.delay(), 0.0);

    // The host stops: the newest sample, without going back.
    const std::uint64_t stalled = edge.edge(stamp.nanos + kSecond, stamp.index, cap);
    EXPECT_FALSE(edge.paced());
    EXPECT_EQ(stalled, stamp.index);

    // Offline.
    BlockStampValue offline = host.next();
    offline.paced = false;
    edge.observe(offline, host.rate);
    EXPECT_EQ(edge.edge(offline.nanos + 1'000'000, offline.index, cap), offline.index);
    EXPECT_FALSE(edge.paced());
}

// Plays `pattern` against a paced edge, through `seen` (which may delay stamps), returning its
// draws.
std::vector<EdgeDraw> playEdge(PacedEdge& edge, const testing::HostPattern& pattern, double cap,
                               const std::function<BlockStampValue(BlockStampValue)>& seen = {})
{
    return testing::simulateHost(
        pattern,
        [&](const std::optional<BlockStampValue>& stamp) {
            edge.observe(stamp && seen ? std::optional(seen(*stamp)) : stamp, pattern.rate);
        },
        [&](std::uint64_t nanos, std::uint64_t newest) {
            return static_cast<double>(edge.edge(nanos, newest, cap));
        });
}

// A block later than any before raises the delay gradually: the edge slows by a tenth for a
// moment (1.7 ms less in a 60 Hz frame) rather than stalling for the whole rise (6 ms here); it
// stays behind the audio and never goes back.
TEST(PacedEdge, RaisesTheDelayBySlowingNotStalling)
{
    PacedEdge edge;
    const testing::HostPattern pattern{.block = 512, .rate = 48000.0, .seconds = 8.0};
    // From 4 s, every block 6 ms late: deliveries later than any before.
    const auto draws =
        playEdge(edge, pattern, protocol::kPacingDelayCapSeconds, [](BlockStampValue stamp) {
            if (stamp.nanos > 4 * kSecond) {
                stamp.nanos += 6'000'000;
            }
            return stamp;
        });
    std::vector<EdgeDraw> settled;
    std::ranges::copy_if(draws, std::back_inserter(settled),
                         [](const EdgeDraw& draw) { return draw.seconds > 3.0; });
    const Motion motion = measureMotion(settled, pattern.rate);
    EXPECT_FALSE(motion.backwards);
    EXPECT_FALSE(motion.ahead);
    EXPECT_LE(motion.advanceErrorMax, 2.0) << "no frame stalls for the rise";
}

// Blocks further apart than the largest delay are not paced: the edge is the newest sample, as
// before, never past it.
TEST(PacedEdge, IsTheNewestSampleForBlocksBeyondTheCap)
{
    const double cap = 0.01; // Shorter than the 85 ms blocks below.
    PacedEdge edge;
    const testing::HostPattern pattern{.block = 4096, .rate = 48000.0, .seconds = 10.0};
    const auto draws = playEdge(edge, pattern, cap);
    EXPECT_FALSE(edge.paced());
    for (const EdgeDraw& draw : draws) {
        ASSERT_EQ(draw.edge, draw.newest) << draw.seconds;
    }
}

// After a long time without an edge (a freeze, a span too short to pace), the delay is what it
// was: the edge moves on paced, behind the audio, not at the newest sample.
TEST(PacedEdge, KeepsItsDelayThroughALongPause)
{
    PacedEdge edge;
    SteadyHost host;
    BlockStampValue stamp;
    for (int i = 0; i < 200; ++i) {
        stamp = host.next();
        edge.observe(stamp, host.rate);
        (void)edge.edge(stamp.nanos + 1'000'000, stamp.index, protocol::kPacingDelayCapSeconds);
    }
    const double delay = edge.delay();
    ASSERT_GT(delay, 0.0);
    // An hour of blocks, with no edge asked for.
    for (int i = 0; i < 337'500; ++i) {
        stamp = host.next();
        edge.observe(stamp, host.rate);
    }
    const std::uint64_t shown =
        edge.edge(stamp.nanos + 1'000'000, stamp.index, protocol::kPacingDelayCapSeconds);
    EXPECT_TRUE(edge.paced());
    EXPECT_LT(shown, stamp.index) << "behind the audio";
    EXPECT_NEAR(edge.delay(), delay, 0.01 * delay);
}

// A host that stops and resumes with smaller blocks (its buffer changed) has its delay measured
// again: the edge trails the audio by about a new block, not the old one.
TEST(PacedEdge, MeasuresItsDelayAgainAfterTheClockRestarts)
{
    PacedEdge edge;
    SteadyHost large{.block = 2048, .rate = 48000.0};
    BlockStampValue stamp;
    for (int i = 0; i < 60; ++i) {
        stamp = large.next();
        edge.observe(stamp, large.rate);
        (void)edge.edge(stamp.nanos + 1'000'000, stamp.index, protocol::kPacingDelayCapSeconds);
    }
    ASSERT_GT(edge.delay(), 2000.0) << "about a 2048-sample block";
    // A 200 ms stall, then 128-sample blocks from where the audio stood.
    const std::uint64_t resumed = stamp.nanos + 200'000'000;
    for (std::uint64_t block = 1; block <= 800; ++block) {
        stamp = {.index = large.delivered * 2048 + block * 128,
                 .nanos = resumed + static_cast<std::uint64_t>(static_cast<double>(block * 128) /
                                                               48000.0 * 1e9),
                 .paced = true};
        edge.observe(stamp, 48000.0);
        (void)edge.edge(stamp.nanos + 1'000'000, stamp.index, protocol::kPacingDelayCapSeconds);
    }
    EXPECT_TRUE(edge.paced());
    EXPECT_LT(edge.delay(), 400.0) << "about a 128-sample block and the margin";
}

} // namespace
} // namespace orizon::engine
