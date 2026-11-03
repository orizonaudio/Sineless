#include "engine/FramePacing.h"
#include "protocol/Protocol.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace orizon::engine {
namespace {

// ----- AD-12's rule, from the vectors the page's tests read too -----

struct TicksCase
{
    nlohmann::json entry;
};

std::vector<TicksCase> ticksCases()
{
    std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/ticks-per-frame.json");
    const nlohmann::json vectors = nlohmann::json::parse(file);
    std::vector<TicksCase> cases;
    for (const nlohmann::json& entry : vectors.at("cases")) {
        cases.push_back({entry});
    }
    return cases;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const TicksCase& ticks, std::ostream* out)
{
    *out << ticks.entry.dump();
}

class TicksPerFrame : public ::testing::TestWithParam<TicksCase>
{
};

// Every refresh up to 90 Hz and at most 90 frames a second on faster displays; near a line, n
// changes only once the rate is past it by the hysteresis.
TEST_P(TicksPerFrame, HoldsEachFrameForTheSameRefreshes)
{
    const nlohmann::json& entry = GetParam().entry;
    const double rate =
        entry.contains("rateHz")
            ? entry.at("rateHz").get<double>()
            : protocol::kFullRateUpToHz * entry.value("line", 1.0) * entry.value("factor", 1.0) *
                  (1.0 + entry.value("hysteresisShare", 0.0) * protocol::kLineHysteresis);
    const std::optional<int> previous =
        entry.contains("previous") ? std::optional(entry.at("previous").get<int>()) : std::nullopt;
    EXPECT_EQ(ticksPerFrame(rate, previous), entry.at("n").get<int>());
}

INSTANTIATE_TEST_SUITE_P(SharedVectors, TicksPerFrame, ::testing::ValuesIn(ticksCases()));

// ----- The display clock -----

// A display link: targets one refresh apart from `start`, each called back `lead` (plus jitter)
// before it, on a link clock offset from the engine's by an arbitrary amount.
struct Link
{
    double rate = 60.0;
    double lead = 0.05;
    double start = 1000.0;                      // Link clock, seconds.
    std::uint64_t engineOffset = 7'000'000'000; // Engine clock at link time 0, ns.
    std::uint64_t refresh = 0;

    [[nodiscard]] double target(std::uint64_t index) const
    {
        return start + static_cast<double>(index) / rate;
    }
    [[nodiscard]] std::uint64_t engineAt(double linkSeconds) const
    {
        return engineOffset + static_cast<std::uint64_t>(std::llround(linkSeconds * 1e9));
    }
    // Calls back for refresh `index`, `jitter` seconds from its usual time (positive: later).
    FramePacing::Decision call(FramePacing& pacing, std::uint64_t index, double jitter = 0.0)
    {
        const double now = target(index) - lead + jitter;
        refresh = index;
        return pacing.callback(target(index), now, engineAt(now));
    }
    [[nodiscard]] std::uint64_t engineNowAfter(std::uint64_t index, double extra = 0.001) const
    {
        return engineAt(target(index) - lead + extra);
    }
};

// Calls back refreshes [from, to) with leads that vary by up to `spread` (later, so smaller).
void callRange(FramePacing& pacing, Link& link, std::uint64_t from, std::uint64_t to,
               double spread = 0.0)
{
    for (std::uint64_t index = from; index < to; ++index) {
        const double jitter = spread * static_cast<double>((index * 7919) % 101) / 100.0;
        link.call(pacing, index, jitter);
    }
}

class DisplayClock : public ::testing::TestWithParam<double>
{
};

// Once L is known, D steps by exactly one refresh per callback, at or before the callback: frames
// placed at D move by whole refreshes however late the worker makes them.
TEST_P(DisplayClock, StepsByExactRefreshes)
{
    const double rate = GetParam();
    Link link{.rate = rate};
    FramePacing pacing(rate);
    callRange(pacing, link, 0, FramePacing::kLeadCallbacks + 5, 0.0003);
    std::optional<std::uint64_t> last;
    for (std::uint64_t index = FramePacing::kLeadCallbacks + 5; index < 400; ++index) {
        link.call(pacing, index, 0.0003 * static_cast<double>((index * 31) % 7) / 7.0);
        // The worker reads a millisecond or two later.
        const std::uint64_t value = pacing.clock(link.engineNowAfter(index, 0.002));
        if (last) {
            EXPECT_NEAR(static_cast<double>(value - *last), 1e9 / rate, 2.0) << index;
        }
        // Never ahead of its callback (to within the conversion's rounding).
        EXPECT_LE(value, link.engineNowAfter(index, 0.0) + 2) << index;
        last = value;
    }
    EXPECT_EQ(pacing.earlyCallbacks(), 0U);
}

INSTANTIATE_TEST_SUITE_P(Rates, DisplayClock, ::testing::Values(60.0, 120.0, 144.0, 240.0));

// Before L is known the clock follows the engine's time; when it becomes known, it moves by no
// more than the callbacks' lead spread, and never back.
TEST(DisplayClockStart, FollowsTheEngineUntilTheLeadIsKnownThenSwitchesSmoothly)
{
    const double spread = 0.0004;
    Link link;
    FramePacing pacing(60.0);
    std::uint64_t before = 0;
    for (std::uint64_t index = 0; index < FramePacing::kLeadCallbacks - 1; ++index) {
        link.call(pacing, index, spread * static_cast<double>(index % 3) / 2.0);
        const std::uint64_t now = link.engineNowAfter(index, 0.001);
        EXPECT_EQ(pacing.clock(now), now);
        before = now;
    }
    link.call(pacing, FramePacing::kLeadCallbacks - 1);
    const std::uint64_t after = pacing.clock(link.engineNowAfter(FramePacing::kLeadCallbacks - 1));
    EXPECT_GE(after, before);
    EXPECT_LE(static_cast<double>(link.engineNowAfter(FramePacing::kLeadCallbacks - 1) - after),
              1e9 * (spread + 0.0015));
}

// A callback earlier than L allowed places D now, not in the future; one just inside places it
// at the target less L.
TEST(DisplayClockStart, PlacesAnEarlyCallbackNowAndAnOnTimeOneAtTheTargetLessTheLead)
{
    Link link;
    FramePacing pacing(60.0);
    callRange(pacing, link, 0, FramePacing::kLeadCallbacks);
    const std::uint64_t index = FramePacing::kLeadCallbacks;
    // 0.2 ms early: beyond the lead measured.
    link.call(pacing, index, -0.0002);
    const std::uint64_t now = link.engineAt(link.target(index) - link.lead - 0.0002);
    EXPECT_EQ(pacing.clock(now), now);
    EXPECT_EQ(pacing.earlyCallbacks(), 1U);
    // On time: the target less L.
    link.call(pacing, index + 1);
    EXPECT_EQ(pacing.clock(link.engineNowAfter(index + 1, 0.003)),
              link.engineAt(link.target(index + 1) - link.lead));
}

// A display change measures L again; meanwhile and after, the clock never goes back.
TEST(DisplayClockChanges, NeverStepsBackAcrossADisplayChange)
{
    Link link;
    FramePacing pacing(60.0);
    callRange(pacing, link, 0, 60);
    std::uint64_t last = pacing.clock(link.engineNowAfter(59));
    pacing.setScreenRate(120.0);
    Link faster{.rate = 120.0, .lead = 0.03, .start = link.target(60)};
    for (std::uint64_t index = 0; index < 120; ++index) {
        faster.call(pacing, index);
        const std::uint64_t value = pacing.clock(faster.engineNowAfter(index));
        EXPECT_GE(value, last) << index;
        last = value;
    }
    EXPECT_EQ(pacing.ticks(), 2);
}

// A move to a display that calls back with another lead measures L again: once measured, D is the
// new display's target less its own lead, and no callback counts as early. Kept at the old lead,
// a move to a slower display with a longer lead found every callback early, placed D when called
// rather than by whole refreshes, and the frames juddered; a move the other way added the
// difference to every frame's delay.
TEST(DisplayClockChanges, MeasuresTheNewDisplaysLeadAfterAMove)
{
    struct Move
    {
        double fromRate, fromLead, toRate, toLead;
    };
    for (const Move move :
         {Move{.fromRate = 120.0, .fromLead = 0.03, .toRate = 60.0, .toLead = 0.05},
          Move{.fromRate = 60.0, .fromLead = 0.05, .toRate = 120.0, .toLead = 0.03}}) {
        Link link{.rate = move.fromRate, .lead = move.fromLead};
        FramePacing pacing(move.fromRate);
        callRange(pacing, link, 0, 60);
        pacing.setScreenRate(move.toRate);
        Link moved{.rate = move.toRate, .lead = move.toLead, .start = link.target(60)};
        callRange(pacing, moved, 0, FramePacing::kLeadCallbacks);
        const std::uint64_t earlyBefore = pacing.earlyCallbacks();
        for (std::uint64_t index = FramePacing::kLeadCallbacks; index < 120; ++index) {
            moved.call(pacing, index);
            EXPECT_EQ(pacing.clock(moved.engineNowAfter(index, 0.002)),
                      moved.engineAt(moved.target(index) - move.toLead))
                << move.fromRate << " to " << move.toRate << " Hz, refresh " << index;
        }
        EXPECT_EQ(pacing.earlyCallbacks(), earlyBefore)
            << move.fromRate << " to " << move.toRate << " Hz";
    }
}

// Paused (hidden, occluded) the clock follows the engine's time; the next callback continues from
// there, never back.
TEST(DisplayClockChanges, FollowsTheEngineWhilePausedAndContinuesForward)
{
    Link link;
    FramePacing pacing(60.0);
    callRange(pacing, link, 0, 60);
    const std::uint64_t atPause = pacing.clock(link.engineNowAfter(59));
    pacing.pause();
    const std::uint64_t later = link.engineAt(link.target(600));
    EXPECT_EQ(pacing.clock(later), later);
    link.call(pacing, 601);
    const std::uint64_t resumed = pacing.clock(link.engineNowAfter(601));
    EXPECT_GE(resumed, later);
    EXPECT_GE(resumed, atPause);
}

// Lost, the clock follows the engine's time for good.
TEST(DisplayClockChanges, FollowsTheEngineOnceLost)
{
    Link link;
    FramePacing pacing(60.0);
    callRange(pacing, link, 0, 60);
    pacing.lose();
    callRange(pacing, link, 60, 120);
    const std::uint64_t now = link.engineNowAfter(119, 0.004);
    EXPECT_EQ(pacing.clock(now), now);
}

// The two clocks share no origin: only differences cross, so any offset gives the same D.
TEST(DisplayClockChanges, PlacesTheSameWhateverTheClocksOffset)
{
    for (const std::uint64_t offset : {std::uint64_t{0}, std::uint64_t{5'000'000'000'000}}) {
        Link link{.engineOffset = offset};
        FramePacing pacing(60.0);
        callRange(pacing, link, 0, 40);
        EXPECT_EQ(pacing.clock(link.engineNowAfter(39)) - offset,
                  link.engineAt(link.target(39) - link.lead) - offset);
    }
}

// ----- The stall watchdog -----

struct StallCase
{
    double rate;
    int every; // The link calls back every this many refreshes.
};

class StallWatchdog : public ::testing::TestWithParam<StallCase>
{
};

// No callback for just under kStallCallbackPeriods callback periods keeps D; at it, the clock
// follows the engine's time so the sweeps keep advancing. The threshold never grows: not after a
// long pause, nor after a previous trip.
TEST_P(StallWatchdog, FollowsTheEngineOnlyAfterTheThreshold)
{
    const auto [rate, every] = GetParam();
    Link link{.rate = rate};
    FramePacing pacing(rate);
    std::uint64_t index = 0;
    const auto calls = [&](int count) {
        for (int call = 0; call < count; ++call, index += static_cast<std::uint64_t>(every)) {
            link.call(pacing, index);
        }
    };
    const double callbackPeriod = static_cast<double>(every) / rate;
    const auto checkThreshold = [&] {
        const double last = link.target(index - static_cast<std::uint64_t>(every)) - link.lead;
        const std::uint64_t held = pacing.clock(link.engineAt(last + 0.0001));
        const double under = (FramePacing::kStallCallbackPeriods - 0.5) * callbackPeriod;
        EXPECT_EQ(pacing.clock(link.engineAt(last + under)), held) << "under the threshold";
        const double over = (FramePacing::kStallCallbackPeriods + 0.1) * callbackPeriod;
        EXPECT_EQ(pacing.clock(link.engineAt(last + over)), link.engineAt(last + over))
            << "past the threshold";
    };
    calls(FramePacing::kLeadCallbacks + 20);
    checkThreshold();
    // A long pause, then callbacks again.
    pacing.pause();
    index += static_cast<std::uint64_t>(600 * rate);
    calls(20);
    checkThreshold();
    // After that trip, callbacks again.
    index += 10;
    calls(20);
    checkThreshold();
}

INSTANTIATE_TEST_SUITE_P(Rates, StallWatchdog,
                         ::testing::Values(StallCase{60.0, 1}, StallCase{120.0, 1},
                                           StallCase{240.0, 1}, StallCase{120.0, 2},
                                           StallCase{240.0, 3}));

// A single skipped callback is a step of two refreshes: no trip, and the rate stays the display's.
TEST(StallWatchdogSkip, TakesASkippedCallbackForTwoRefreshes)
{
    Link link;
    FramePacing pacing(60.0);
    callRange(pacing, link, 0, 60);
    link.call(pacing, 61); // 60 skipped.
    const std::uint64_t value = pacing.clock(link.engineNowAfter(61, 0.002));
    EXPECT_EQ(value, link.engineAt(link.target(61) - link.lead));
    callRange(pacing, link, 62, 80);
    EXPECT_NEAR(pacing.rateHertz(), 60.0, 0.01);
}

// ----- Which refreshes show a new frame -----

// At 60 Hz every refresh shows a new frame; at 120 Hz every second; a link the system slows to
// every n-th refresh still shows frames at the display's rate divided by n (one per callback).
TEST(FrameRequests, AskForEveryNthRefreshCountedInRefreshes)
{
    for (const auto [rate, every, expectedShare] :
         {std::tuple{60.0, 1, 1.0}, std::tuple{120.0, 1, 0.5}, std::tuple{120.0, 2, 1.0},
          std::tuple{240.0, 1, 1.0 / 3.0}}) {
        SCOPED_TRACE(rate);
        Link link{.rate = rate};
        FramePacing pacing(rate);
        int requests = 0;
        int callbacks = 0;
        for (std::uint64_t index = 0; index < 600; index += static_cast<std::uint64_t>(every)) {
            const bool asked = link.call(pacing, index).requestFrame;
            if (index >= 120) {
                requests += asked ? 1 : 0;
                ++callbacks;
            }
        }
        EXPECT_NEAR(static_cast<double>(requests) / callbacks, expectedShare, 0.02);
    }
}

// Moving from a 60 Hz to a 120 Hz display changes n once, from 1 to 2.
TEST(FrameRequests, ChangesNOnceWhenTheWindowMovesToAFasterDisplay)
{
    Link link;
    FramePacing pacing(60.0);
    callRange(pacing, link, 0, 60);
    EXPECT_EQ(pacing.ticks(), 1);
    pacing.setScreenRate(120.0);
    Link faster{.rate = 120.0, .start = link.target(60)};
    std::vector<int> seen;
    for (std::uint64_t index = 0; index < 240; ++index) {
        faster.call(pacing, index);
        if (seen.empty() || seen.back() != pacing.ticks()) {
            seen.push_back(pacing.ticks());
        }
    }
    EXPECT_EQ(seen, std::vector<int>{2});
}

// The display's period, as the presented counts divide by it: a refresh, not the step between
// callbacks, which at two refreshes a frame is two; unknown (0) until measured, and again after a
// move to another display until measured there.
TEST(FrameRequests, PublishesTheDisplaysPeriodNotTheCallbacks)
{
    FramePacing pacing(120.0);
    EXPECT_EQ(pacing.displayPeriodSeconds(), 0.0);
    Link everyOther{.rate = 60.0}; // Targets two 120 Hz refreshes apart.
    callRange(pacing, everyOther, 0, 60);
    EXPECT_NEAR(pacing.displayPeriodSeconds(), 1.0 / 120.0, 1e-9);
    pacing.setScreenRate(60.0);
    EXPECT_EQ(pacing.displayPeriodSeconds(), 0.0) << "the old display's period, after a move";
    Link slower{.rate = 60.0, .start = everyOther.target(60)};
    callRange(pacing, slower, 0, 60);
    EXPECT_NEAR(pacing.displayPeriodSeconds(), 1.0 / 60.0, 1e-9);
}

// The editor starts the clock at an assumed rate and the screen's own may never be given (a window
// with no screen yet): the rate is measured from the link's targets, and n follows it, so a fast
// display still shows at most AD-12's frames a second. Just past a line, n keeps its value while
// within the hysteresis, as when the rate is given.

TEST(FrameRequests, MeasuresTheRateFromTheLinksOwnRefreshes)
{
    const double pastLine = protocol::kFullRateUpToHz * (1.0 + 0.5 * protocol::kLineHysteresis);
    for (const double rate : {120.0, 144.0, 240.0, pastLine}) {
        Link link{.rate = rate};
        FramePacing pacing(60.0);
        int requests = 0;
        const std::uint64_t refreshes = 600;
        for (std::uint64_t index = 0; index < refreshes; ++index) {
            if (link.call(pacing, index).requestFrame && index >= refreshes / 2) {
                ++requests;
            }
        }
        const int expected = ticksPerFrame(rate, 1);
        EXPECT_EQ(pacing.ticks(), expected) << rate << " Hz";
        // Frames shown in the second half, once measured: one every n refreshes.
        EXPECT_NEAR(requests, static_cast<double>(refreshes) / 2.0 / expected, 1.0)
            << rate << " Hz";
    }
}

// ----- Threads -----

// The display link writes while the worker reads: only atomics are shared (the ThreadSanitizer
// builds of scripts/check-linux.sh check this), and the worker's clock never goes back.
TEST(FramePacingThreads, LetsTheWorkerReadWhileTheLinkCallsBack)
{
    Link link;
    FramePacing pacing(60.0);
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> engineNow{link.engineAt(link.start)};
    std::thread worker([&] {
        std::uint64_t last = 0;
        while (!done.load()) {
            const std::uint64_t value = pacing.clock(engineNow.load());
            EXPECT_GE(value, last);
            last = value;
        }
    });
    for (std::uint64_t index = 0; index < 2000; ++index) {
        link.call(pacing, index);
        engineNow.store(link.engineNowAfter(index));
        if (index == 1500) {
            pacing.lose();
        }
    }
    done.store(true);
    worker.join();
}

} // namespace
} // namespace orizon::engine
