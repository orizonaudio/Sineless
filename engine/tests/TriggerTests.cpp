#include "engine/Trigger.h"

#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <random>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::TriggerMode;
using protocol::TriggerSlope;
using Action = TriggerDecision::Action;

constexpr double kPeriod = 100.0; // Samples per cycle of the test sine.

std::vector<float> sine(std::size_t count, double phase = 0.0)
{
    std::vector<float> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * (static_cast<double>(i) + phase) / kPeriod));
    }
    return samples;
}

std::vector<double> edges(const std::vector<float>& samples, const TriggerParameters& parameters,
                          std::uint64_t firstIndex = 0)
{
    std::vector<double> found;
    forEachEdge(samples, firstIndex, parameters, [&](double edge) { found.push_back(edge); });
    return found;
}

TEST(EdgeTrigger, FindsRisingZeroCrossingsWithSubSampleAccuracy)
{
    // Phase 0.3 puts the rising zero crossings 0.3 samples before each multiple of the period.
    const auto found = edges(sine(1000, 0.3), {.hysteresis = 0.01});
    ASSERT_GE(found.size(), 9u);
    for (double edge : found) {
        const double expected = std::round((edge + 0.3) / kPeriod) * kPeriod - 0.3;
        EXPECT_NEAR(edge, expected, 1e-3);
    }
}

TEST(EdgeTrigger, FindsFallingCrossingsAtTheLevel)
{
    const auto found =
        edges(sine(1000), {.level = 0.5, .hysteresis = 0.01, .slope = TriggerSlope::kFalling});
    ASSERT_FALSE(found.empty());
    for (double edge : found) {
        // sin(x) = 0.5 going down at x = 5π/6, i.e. 5/12 of a period.
        const double phase = std::fmod(edge, kPeriod) / kPeriod;
        EXPECT_NEAR(phase, 5.0 / 12.0, 1e-3);
    }
}

TEST(EdgeTrigger, ReportsAbsoluteIndices)
{
    const auto local = edges(sine(500), {.hysteresis = 0.01});
    const auto shifted = edges(sine(500), {.hysteresis = 0.01}, 1'000'000);
    ASSERT_EQ(local.size(), shifted.size());
    for (std::size_t i = 0; i < local.size(); ++i) {
        EXPECT_NEAR(shifted[i] - local[i], 1'000'000.0, 1e-6);
    }
}

TEST(EdgeTrigger, HysteresisRejectsNoiseAroundTheLevel)
{
    // A square wave with noise: without hysteresis the noise re-crosses the level.
    std::mt19937 random(5); // NOLINT(bugprone-random-generator-seed): reproducible
    std::uniform_real_distribution<float> noise(-0.05f, 0.05f);
    std::vector<float> samples(1000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = ((i / 50) % 2 == 0 ? -1.0f : 1.0f) * 0.02f + noise(random);
    }
    const auto noisy = edges(samples, {.hysteresis = 0.0});
    const auto clean = edges(samples, {.hysteresis = 0.1});
    EXPECT_GT(noisy.size(), 20u);
    EXPECT_TRUE(clean.empty()) << "noise never leaves the hysteresis band";
}

TEST(EdgeTrigger, IgnoresDcAndSilence)
{
    EXPECT_TRUE(edges(std::vector<float>(1000, 0.0f), {.hysteresis = 0.01}).empty());
    EXPECT_TRUE(edges(std::vector<float>(1000, 0.7f), {.hysteresis = 0.01}).empty());
}

class TriggerModes : public ::testing::Test
{
protected:
    static constexpr double kSpan = 200.0;
    static constexpr double kTimeout = 150.0;

    TriggerDecision decide(const std::vector<float>& samples, TriggerMode mode,
                           TriggerParameters parameters = {.hysteresis = 0.01},
                           std::uint64_t firstIndex = 0)
    {
        return state.decide(samples, firstIndex, kSpan, kTimeout, parameters, mode);
    }

    TriggerState state;
};

TEST_F(TriggerModes, FreeDrawsTheNewestSpanUntriggered)
{
    const auto decision = decide(sine(1000), TriggerMode::kFree);
    EXPECT_EQ(decision.action, Action::kDraw);
    EXPECT_FALSE(decision.triggered);
    EXPECT_EQ(decision.windowStart, 1000.0 - kSpan);
}

TEST_F(TriggerModes, AutoPlacesTheNewestUsableEdgeAtTenPercent)
{
    const auto decision = decide(sine(1000, 0.3), TriggerMode::kAuto);
    ASSERT_EQ(decision.action, Action::kDraw);
    EXPECT_TRUE(decision.triggered);
    const double trigger = decision.windowStart + kTriggerPosition * kSpan;
    // Newest edge with a full span after it: edges at 99.7, 199.7, ... and 1000 - 180 = 820.
    EXPECT_NEAR(trigger, 799.7, 1e-3);
}

TEST_F(TriggerModes, AutoFallsBackToUntriggeredWithoutEdges)
{
    const auto decision = decide(std::vector<float>(1000, 0.0f), TriggerMode::kAuto);
    EXPECT_EQ(decision.action, Action::kDraw);
    EXPECT_FALSE(decision.triggered);
}

TEST_F(TriggerModes, NormalHoldsWithoutEdges)
{
    EXPECT_EQ(decide(std::vector<float>(1000, 0.0f), TriggerMode::kNormal).action, Action::kHold);
}

TEST_F(TriggerModes, HoldsWhenNoNewEdgeHasArrived)
{
    const auto samples = sine(1000, 0.3);
    ASSERT_EQ(decide(samples, TriggerMode::kNormal).action, Action::kDraw);
    EXPECT_EQ(decide(samples, TriggerMode::kNormal).action, Action::kHold) << "same data again";
}

TEST_F(TriggerModes, SingleFreezesOnceUntilRearmed)
{
    const auto first = decide(sine(1000, 0.3), TriggerMode::kSingle);
    EXPECT_EQ(first.action, Action::kDraw);
    EXPECT_TRUE(first.freeze);

    // Newer data with new edges: nothing until re-armed.
    const auto later = sine(2000, 0.3);
    EXPECT_EQ(decide(later, TriggerMode::kSingle).action, Action::kHold);
    state.arm();
    const auto rearmed = decide(sine(3000, 0.3), TriggerMode::kSingle);
    EXPECT_EQ(rearmed.action, Action::kDraw);
    EXPECT_TRUE(rearmed.freeze);
}

TEST_F(TriggerModes, HoldOffSpacesSuccessiveTriggers)
{
    // Edges every 100 samples; a 250-sample hold-off accepts every third edge of the chain.
    const TriggerParameters parameters{.hysteresis = 0.01, .holdOffSamples = 250.0};
    const auto samples = sine(1000, 0.3);
    ASSERT_EQ(decide(samples, TriggerMode::kNormal, parameters).action, Action::kDraw);
    ASSERT_TRUE(state.lastTrigger().has_value());
    const double first = state.lastTrigger().value();

    // Continue the same signal 300 samples later: the chain moves on by a multiple of 300.
    std::vector<float> continued(samples.begin() + 300, samples.end());
    const auto next = sine(300, 1000.3);
    continued.insert(continued.end(), next.begin(), next.end());
    ASSERT_EQ(decide(continued, TriggerMode::kNormal, parameters, 300).action, Action::kDraw);
    ASSERT_TRUE(state.lastTrigger().has_value());
    const double gap = state.lastTrigger().value() - first;
    EXPECT_GE(gap, 250.0);
    EXPECT_NEAR(std::fmod(gap, 300.0), 0.0, 1e-3);
}

TEST(TriggerRegion, CoversTheSpanAndTimeoutWithinTheReadableRange)
{
    const auto [first, end] = TriggerState::searchRegion(0, 100'000, 200.0, 150.0);
    EXPECT_EQ(end, 100'000u);
    EXPECT_EQ(end - first, 401u);
    const auto [clampedFirst, clampedEnd] =
        TriggerState::searchRegion(99'900, 100'000, 200.0, 150.0);
    EXPECT_EQ(clampedFirst, 99'900u);
    EXPECT_EQ(clampedEnd, 100'000u);
}

} // namespace
} // namespace orizon::engine
