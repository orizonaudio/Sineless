#pragma once

// The benchmarks' scenarios, by a name that stays the same as long as the scenario does: results
// are kept and compared by it (scripts/engine-work.sh, and later the reference machines). Change
// what a name means only together with its name.

#include "protocol/Protocol.h"

#include <array>
#include <optional>
#include <string_view>

namespace orizon::bench {

struct Scenario
{
    std::string_view name;
    std::string_view description;
    protocol::DisplayMode mode = protocol::DisplayMode::kMulti;
    int tracks = 1;
    int channels = 2;
    protocol::TimeBase timeBase = protocol::TimeBase::kBeats;
    protocol::BeatSpan beatSpan = protocol::BeatSpan::kOneBar; // In Beats.
    double span = 1.0;                                         // Seconds, in Seconds.
    double rate = 48000.0;
    double tempo = 120.0;
    bool compareAll = false; // Compare: every other track checked, not only the first.
    bool previewFix = false;
    bool jitter = false; // Other tracks 0 to 3 ms late, moving (bench::Signals).
    // The part of the span on screen.
    double viewStart = 0.0;
    double viewEnd = 1.0;
    bool frozenPan = false; // Frozen, panning back and forth through the held audio.
    // Judged by CI's engine-work check (every scenario today); one set to false is measured and
    // reported only, as for a scenario whose counts are not yet known to be steady.
    bool gated = true;
};

inline constexpr std::array kScenarios{
    Scenario{.name = "compare-15-worst",
             .description = "The worst case: Compare, 15 tracks all checked, Preview fix, moving "
                            "matches, 16 bars at 60 BPM and 96 kHz",
             .mode = protocol::DisplayMode::kCompare,
             .tracks = 15,
             .beatSpan = protocol::BeatSpan::kSixteenBars,
             .rate = 96000.0,
             .tempo = 60.0,
             .compareAll = true,
             .previewFix = true,
             .jitter = true},
    Scenario{.name = "multi-15-sixteen-bars",
             .description = "Multi, 15 tracks, 16 bars at 60 BPM and 96 kHz",
             .tracks = 15,
             .beatSpan = protocol::BeatSpan::kSixteenBars,
             .rate = 96000.0,
             .tempo = 60.0},
    Scenario{.name = "multi-7-four-bars",
             .description = "Multi, 7 tracks, 4 bars at 120 BPM and 48 kHz",
             .tracks = 7,
             .beatSpan = protocol::BeatSpan::kFourBars},
    Scenario{.name = "compare-7-four-bars",
             .description = "Compare, 7 tracks, the first other checked, 4 bars at 120 BPM and "
                            "48 kHz",
             .mode = protocol::DisplayMode::kCompare,
             .tracks = 7,
             .beatSpan = protocol::BeatSpan::kFourBars},
    Scenario{.name = "compare-15-roll",
             .description = "Compare, 15 tracks all checked, rolling 4 s at 96 kHz",
             .mode = protocol::DisplayMode::kCompare,
             .tracks = 15,
             .timeBase = protocol::TimeBase::kSeconds,
             .span = 4.0,
             .rate = 96000.0,
             .compareAll = true},
    Scenario{.name = "single-one-bar",
             .description = "Single, all four views, 1 bar at 120 BPM and 48 kHz",
             .mode = protocol::DisplayMode::kSingle},
    Scenario{.name = "single-triggered",
             .description = "Single, all four views, triggered over 20 ms at 48 kHz",
             .mode = protocol::DisplayMode::kSingle,
             .timeBase = protocol::TimeBase::kSeconds,
             .span = 0.02},
    Scenario{
        .name = "multi-15-zoomed",
        .description = "Multi, 15 tracks, 16 bars at 60 BPM and 96 kHz, zoomed in 16 times",
        .tracks = 15,
        .beatSpan = protocol::BeatSpan::kSixteenBars,
        .rate = 96000.0,
        .tempo = 60.0,
        .viewStart = 0.5,
        .viewEnd = 0.5625,
    },
    Scenario{
        .name = "compare-15-frozen-pan",
        .description = "Compare, 15 tracks all checked, frozen over 1 s at 96 kHz, panning",
        .mode = protocol::DisplayMode::kCompare,
        .tracks = 15,
        .timeBase = protocol::TimeBase::kSeconds,
        .span = 1.0,
        .rate = 96000.0,
        .compareAll = true,
        .frozenPan = true,
    },
    Scenario{
        .name = "single-mono-one-bar",
        .description = "Single on a mono track, 1 bar at 120 BPM and 48 kHz",
        .mode = protocol::DisplayMode::kSingle,
        .channels = 1,
    },
};

inline std::optional<Scenario> scenarioNamed(std::string_view name)
{
    for (const Scenario& scenario : kScenarios) {
        if (scenario.name == name) {
            return scenario;
        }
    }
    return std::nullopt;
}

} // namespace orizon::bench
