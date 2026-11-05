#pragma once

// The host-event fuzzer's input, shared by the fuzzer (HostFuzzer.cpp) and the writer of its seeds
// (FuzzInputs.cpp): each step is a byte choosing a StepKind (its value modulo kStepKinds), then the
// bytes HostFuzzer.cpp's readSteps reads for that kind.

#include "SampleRates.h"
#include "engine/CaptureRing.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace orizon::fuzz::host {

// The rates hosts run at, and edges around them: below and above any host's, and either side of
// the highest the capture is sized for.
inline constexpr auto kRates = std::to_array<double>(
    {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0, 8000.0, 22050.0,
     768000.0, engine::CaptureRing::kHighestSampleRate - 1.0,
     engine::CaptureRing::kHighestSampleRate + 1.0});
static_assert(
    [] {
        for (std::size_t i = 0; i < engine::kSampleRates.size(); ++i) {
            if (kRates[i] != engine::kSampleRates[i]) {
                return false;
            }
        }
        return true;
    }(),
    "kRates starts with every rate hosts run at");

inline constexpr auto kTempi = std::to_array<double>({120.0, 20.0, 999.0, 60.0, 174.5, 120.0001});
inline constexpr auto kDenominators = std::to_array<int>({4, 8, 2, 16, 1});

enum class StepKind : std::uint8_t
{
    kPrepare,
    kRate,
    kChannels,
    kPlay,
    kTransport,
    kLeave,
    kJoin,
    kFreeze,
    kMessage,
    kWait,
    kAdvance,
    kFrame,
    kCount,
};

// Which tracks a host step applies to. One track alone makes the two tracks' audio differ.
enum class Tracks : std::uint8_t
{
    kBoth,
    kOwn,
    kOther,
};

enum class SignalKind : std::uint8_t
{
    kSine,
    kSquare,
    kDc,
    kSilence,
    kImpulse,
    kNonFinite,
    kCount,
};

inline constexpr auto kStepKinds = static_cast<std::size_t>(StepKind::kCount);
inline constexpr auto kSignalKinds = static_cast<std::size_t>(SignalKind::kCount);

} // namespace orizon::fuzz::host
