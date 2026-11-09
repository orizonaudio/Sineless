#include "engine/FramePacing.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
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

// Once L is known, D steps by exactly one refresh per callback: frames placed at D move by whole
// refreshes however late the worker makes them after its request, and none is placed later than
// it is made.
TEST_P(DisplayClock, StepsByExactRefreshes)
{
    const double rate = GetParam();
    Link link{.rate = rate};
    FramePacing pacing(rate);
    callRange(pacing, link, 0, FramePacing::kLeadCallbacks + 5, 0.0003);
    std::optional<std::uint64_t> last;
    for (std::uint64_t index = FramePacing::kLeadCallbacks + 5; index < 400; ++index) {
        link.call(pacing, index, 0.0003 * static_cast<double>((index * 31) % 7) / 7.0);
        // The worker reads a millisecond or two after the request.
        const std::uint64_t read = link.engineNowAfter(index, pacing.requestDelaySeconds() + 0.002);
        const std::uint64_t value = pacing.clock(read);
        if (last) {
            EXPECT_NEAR(static_cast<double>(value - *last), 1e9 / rate, 2.0) << index;
        }
        EXPECT_LE(value, read) << index;
        last = value;
    }
    EXPECT_EQ(pacing.earlyCallbacks(), 0U);
}

// The frame a callback asks for is drawn by the next callback, so it is placed for when it is
// made, just before that callback, not at the asking callback: the picture is not a refresh older
// than it need be. From the first measured period on, and across a move to another display.
TEST_P(DisplayClock, PlacesEachFrameWhenItIsMade)
{
    const double rate = GetParam();
    Link link{.rate = rate};
    FramePacing pacing(rate);
    const auto check = [&](std::uint64_t from, std::uint64_t to) {
        for (std::uint64_t index = from; index < to; ++index) {
            link.call(pacing, index);
            if (index < from + FramePacing::kLeadCallbacks + 2) {
                continue; // L is being measured.
            }
            const std::uint64_t made =
                link.engineNowAfter(index, pacing.requestDelaySeconds() + 0.0005);
            const std::uint64_t drawnAt = link.engineNowAfter(index + 1, 0.0);
            const auto placed = static_cast<double>(pacing.clock(made));
            // Placed within the margin it is made in before the callback that draws it.
            EXPECT_GE(placed, static_cast<double>(drawnAt) -
                                  (FramePacing::kHandOffMarginSeconds + 0.001) * 1e9)
                << index;
            EXPECT_LE(placed, static_cast<double>(made)) << index;
        }
    };
    check(0, 200);
    pacing.setScreenRate(rate);
    link.start = link.target(200);
    check(0, 200);
}

// A callback that runs late still asks for its frame in time for the next one: the request is
// timed from when the callback was due, so a late one waits less, and the frame is made before
// the next callback draws, not after it (a refresh with no new frame).
TEST_P(DisplayClock, AsksInTimeAfterALateCallback)
{
    const double rate = GetParam();
    Link link{.rate = rate};
    FramePacing pacing(rate);
    callRange(pacing, link, 0, FramePacing::kLeadCallbacks + 20);
    for (const double late : {0.0, 0.002, 0.004, 0.008}) {
        std::uint64_t index = FramePacing::kLeadCallbacks + 20;
        const double calledAt = link.target(index) - link.lead + late;
        const auto decision = link.call(pacing, index, late);
        const double nextCallback = link.target(index + 1) - link.lead;
        if (decision.requestFrame) {
            // At that moment, or at once when the callback ran past it.
            EXPECT_LE(calledAt + decision.requestAfterSeconds,
                      std::max(calledAt, nextCallback - FramePacing::kHandOffMarginSeconds) + 1e-6)
                << late * 1000 << " ms late";
        }
        callRange(pacing, link, index + 1, index + 3);
        link.start += 3.0 / rate; // The same indices again, a few refreshes on.
    }
}

INSTANTIATE_TEST_SUITE_P(Rates, DisplayClock, ::testing::Values(60.0, 120.0, 144.0, 240.0));

// ----- The hand-off margin, from how long frames take to make -----

// A display link whose worker makes each frame `making(index)` seconds after it is asked for, and
// reports that time to the pacing as the editor's frame flow does. Records, for each frame asked
// for, when it was asked for and ready, and the clock the worker read making it.
struct Asked
{
    std::uint64_t index;
    double askedAt; // Link clock, seconds.
    double readyAt;
    double drawnAt; // The callback that draws it.
    std::uint64_t clock;
};

std::vector<Asked> askAndMake(FramePacing& pacing, Link& link, std::uint64_t count,
                              const std::function<double(std::uint64_t)>& making)
{
    std::vector<Asked> asked;
    // Each frame's time is reported when it is ready: one ready after the next callback is
    // reported then, just before the frame that callback asks for.
    std::vector<std::pair<double, double>> reports; // When, and the time it took.
    for (std::uint64_t index = 0; index < count; ++index) {
        const double calledAt = link.target(index) - link.lead;
        std::erase_if(reports, [&](const auto& report) {
            if (report.first <= calledAt) {
                pacing.frameMade(static_cast<std::uint64_t>(std::llround(report.second * 1e9)));
                return true;
            }
            return false;
        });
        const auto decision = link.call(pacing, index);
        if (!decision.requestFrame) {
            continue;
        }
        const double askedAt = calledAt + decision.requestAfterSeconds;
        const double seconds = making(index);
        // The worker reads the clock as it starts making the frame, half a millisecond on.
        const std::uint64_t clock = pacing.clock(link.engineAt(askedAt + 0.0005));
        reports.emplace_back(askedAt + seconds, seconds);
        asked.push_back({.index = index,
                         .askedAt = askedAt,
                         .readyAt = askedAt + seconds,
                         .drawnAt = link.target(index + 1) - link.lead,
                         .clock = clock});
    }
    return asked;
}

// Once a second of making times is known and the margin has reached them: L measured, 60 times,
// and the margin's rise from the floor (well under a further second).
constexpr std::uint64_t kSettledCallbacks = FramePacing::kLeadCallbacks + 2 * 240;

struct MakingCase
{
    double rate;
    double makingMs;
};

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const MakingCase& c, std::ostream* out)
{
    *out << c.rate << " Hz, " << c.makingMs << " ms";
}

class HandOff : public ::testing::TestWithParam<MakingCase>
{
};

// Each refresh finds the frame it asked for, however long the frame takes to make, as long as it
// takes less than the time between the request and that refresh: a busy view is asked for
// earlier, instead of its frames missing their refresh (and the display repeating one) as with a
// fixed margin. A frame that takes longer than a callback period is asked for at the callback,
// no later than before.
TEST_P(HandOff, FindsEachFrameItAskedFor)
{
    const MakingCase c = GetParam();
    Link link{.rate = c.rate};
    FramePacing pacing(c.rate);
    const auto asked = askAndMake(pacing, link, kSettledCallbacks + 240,
                                  [&](auto) { return c.makingMs / 1000.0; });
    std::size_t checked = 0;
    for (const Asked& frame : asked) {
        if (frame.index < kSettledCallbacks) {
            continue;
        }
        ++checked;
        if (c.makingMs / 1000.0 >= 1.0 / c.rate) {
            EXPECT_DOUBLE_EQ(frame.askedAt, link.target(frame.index) - link.lead)
                << "asked for at the callback, at " << frame.index;
        } else {
            EXPECT_LE(frame.readyAt, frame.drawnAt) << "missed its refresh, at " << frame.index;
        }
    }
    EXPECT_GT(checked, 100U);
}

INSTANTIATE_TEST_SUITE_P(RatesAndLoads, HandOff,
                         ::testing::Values(MakingCase{60.0, 0.5}, MakingCase{60.0, 3.0},
                                           MakingCase{60.0, 6.0}, MakingCase{60.0, 10.0},
                                           MakingCase{120.0, 0.5}, MakingCase{120.0, 3.0},
                                           MakingCase{120.0, 6.0}, MakingCase{120.0, 10.0}));

// A light view keeps the gain: its frame is asked for within the floor of the refresh that draws
// it, so the picture is that fresh, again within two seconds of the last slow frame and after a
// heavy spell. While the slow frame is among the last 60, frames are asked for early enough that
// another as slow is found (the slow one missed its refresh, and was reported after it).
TEST(HandOffLight, IsAskedForJustBeforeItsRefresh)
{
    for (const double rate : {60.0, 120.0}) {
        SCOPED_TRACE(rate);
        Link link{.rate = rate};
        FramePacing pacing(rate);
        const std::uint64_t slowAt = kSettledCallbacks + 101;
        const auto twoSeconds = 2 * static_cast<std::uint64_t>(rate);
        const std::uint64_t heavyFrom = slowAt + twoSeconds + 480;
        const std::uint64_t heavyTo = heavyFrom + 240;
        // The first frame asked for from slowAt on is slow, and so is the first from half a second
        // later, while the first is still among the last 60: by then the margin covers it.
        const std::uint64_t againAt = slowAt + static_cast<std::uint64_t>(rate / 2.0);
        std::optional<std::uint64_t> firstSlow;
        std::optional<std::uint64_t> secondSlow;
        const auto asked = askAndMake(pacing, link, heavyTo + kSettledCallbacks + 240, [&](auto i) {
            if (i >= heavyFrom && i < heavyTo) {
                return 0.006;
            }
            if (i >= slowAt && !firstSlow) {
                firstSlow = i;
                return 0.006;
            }
            if (i >= againAt && !secondSlow) {
                secondSlow = i;
                return 0.006;
            }
            return 0.0005;
        });
        ASSERT_TRUE(firstSlow && secondSlow);
        for (const Asked& frame : asked) {
            if (frame.index == *secondSlow) {
                EXPECT_LE(frame.readyAt, frame.drawnAt) << "a slow frame like the one before";
            }
        }
        for (const Asked& frame : asked) {
            const bool settled =
                (frame.index >= kSettledCallbacks && frame.index < slowAt) ||
                (frame.index >= *secondSlow + twoSeconds && frame.index < heavyFrom) ||
                frame.index >= heavyTo + kSettledCallbacks;
            if (settled) {
                EXPECT_GE(frame.askedAt, frame.drawnAt - FramePacing::kHandOffMarginSeconds - 1e-9)
                    << "asked for early, at " << frame.index;
            }
        }
    }
}

// While the margin moves to follow the making time, frames are still placed a whole number of
// refreshes apart, within QS-14's 1 ms: the playhead's steps stay even with uneven making times
// and across a switch between a light view and a heavy one. A margin that jumped with each
// change in recent making times moved the placement by as much (2-4 ms).
TEST(HandOffMotion, PlacesFramesEvenlyWhileTheMarginMoves)
{
    struct Load
    {
        std::string name;
        std::function<double(std::uint64_t)> making;
    };
    const std::vector<Load> loads{
        {.name = "5 ms, every tenth 9 ms",
         .making = [](std::uint64_t i) { return i % 10 == 3 ? 0.009 : 0.005; }},
        {.name = "4.2-5 ms, 3 % 2-6 ms slower",
         .making =
             [](std::uint64_t i) {
                 const double base = 0.0042 + 0.0008 * static_cast<double>((i * 7919) % 97) / 96.0;
                 return (i * 104729) % 100 < 3
                            ? base + 0.002 + 0.004 * static_cast<double>(i % 5) / 4.0
                            : base;
             }},
        {.name = "light, heavy, light",
         .making = [](std::uint64_t i) { return (i / 300) % 2 == 1 ? 0.008 : 0.0005; }},
    };
    for (const double rate : {60.0, 120.0}) {
        for (const Load& load : loads) {
            SCOPED_TRACE(std::to_string(rate) + " Hz, " + load.name);
            Link link{.rate = rate};
            FramePacing pacing(rate);
            const auto asked = askAndMake(pacing, link, 1500, load.making);
            for (std::size_t k = 1; k < asked.size(); ++k) {
                if (asked[k - 1].index < FramePacing::kLeadCallbacks + 2) {
                    continue; // L is being measured.
                }
                const auto refreshes = static_cast<double>(asked[k].index - asked[k - 1].index);
                const double step = static_cast<double>(asked[k].clock - asked[k - 1].clock) / 1e9;
                EXPECT_NEAR(step, refreshes / rate, 0.001) << "at " << asked[k].index;
            }
            // Each frame is found by its refresh: with uneven times once a second of them is
            // known (the slow ones too, which a margin from most frames left late), and a second
            // after the switch to the heavy view. Frames longer than a callback period are not.
            for (const Asked& frame : asked) {
                if (frame.readyAt - frame.askedAt >= 1.0 / rate) {
                    continue;
                }
                const bool switching = load.name == "light, heavy, light";
                const bool settled = switching ? (frame.index / 300) % 2 == 1 &&
                                                     static_cast<double>(frame.index % 300) >= rate
                                               : frame.index >= kSettledCallbacks;
                if (settled) {
                    EXPECT_LE(frame.readyAt, frame.drawnAt)
                        << "missed its refresh at " << frame.index;
                }
            }
        }
    }
}

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
    // On time: the target less L, moved on to when its frame is asked for.
    link.call(pacing, index + 1);
    const double delay = pacing.requestDelaySeconds();
    EXPECT_NEAR(static_cast<double>(pacing.clock(link.engineNowAfter(index + 1, delay + 0.001))),
                static_cast<double>(link.engineAt(link.target(index + 1) - link.lead + delay)),
                2.0);
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
            const double delay = pacing.requestDelaySeconds();
            EXPECT_NEAR(
                static_cast<double>(pacing.clock(moved.engineNowAfter(index, delay + 0.001))),
                static_cast<double>(moved.engineAt(moved.target(index) - move.toLead + delay)), 2.0)
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
        const double delay = pacing.requestDelaySeconds();
        EXPECT_NEAR(
            static_cast<double>(pacing.clock(link.engineNowAfter(39, delay + 0.001)) - offset),
            static_cast<double>(link.engineAt(link.target(39) - link.lead + delay) - offset), 2.0);
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
        // Read once the frame's moment has passed, as the worker does.
        const std::uint64_t held =
            pacing.clock(link.engineAt(last + pacing.requestDelaySeconds() + 0.0001));
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
    const double delay = pacing.requestDelaySeconds();
    const auto value = static_cast<double>(pacing.clock(link.engineNowAfter(61, delay + 0.001)));
    EXPECT_NEAR(value, static_cast<double>(link.engineAt(link.target(61) - link.lead + delay)),
                2.0);
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
