#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace orizon::engine {

// Header values of one display frame; the layout is protocol::FrameHeader.
struct FrameHeaderValues
{
    protocol::FrameMode mode = protocol::FrameMode::kRaw;
    std::uint8_t flags = 0;     // protocol::FrameFlag bits.
    std::uint8_t views = 0;     // protocol::ChannelView bit mask.
    std::uint8_t laneCount = 0; // Multi-track frames: lanes instead of views.
    std::uint32_t pointCount = 0;
    float startFraction = 0.0f;
    // Readouts; NaN when not measured.
    float peak = std::numeric_limits<float>::quiet_NaN();
    float rms = std::numeric_limits<float>::quiet_NaN();
    float frequency = std::numeric_limits<float>::quiet_NaN();
    float cursor = -1.0f; // Sweep playhead position (0 to 1), or -1.
    // Musical context of a synced sweep in quarter notes; 0 otherwise.
    float cycleStart = 0.0f;
    float spanQuarterNotes = 0.0f;
    float barQuarterNotes = 0.0f;
    float cycleSeconds = 0.0f; // The left edge's project time.
    // The part of the span covered, and its length in samples.
    float viewStart = 0.0f;
    float viewEnd = 1.0f;
    float visibleSamples = 0.0f;
    // The lanes message that describes this frame's lanes.
    std::uint32_t laneSet = 0;
    // Compare frames (flag kCompared): the summing gain of the compared tracks, and with the fix.
    float sumGain = 0.0f;
    float fixedGain = 0.0f;
    // Frozen in Seconds mode: how far back the view can be panned, in seconds; 0 otherwise.
    float panReach = 0.0f;
    // Samples not finite: how many in what the readouts measure, and a bit per lane holding any.
    std::uint32_t invalidSamples = 0;
    std::uint32_t invalidLanes = 0;
    // The rate of the audio shown (frozen, the one it was held at), for the frame's times.
    float sampleRate = 0.0f;
};

// Writes the header and `viewData` (each view's points, in ChannelView order, or each lane's,
// then a Compare frame's matches) little-endian into `output`, which keeps its capacity across
// calls.
void encodeFrame(const FrameHeaderValues& header, std::span<const float> viewData,
                 std::vector<std::byte>& output);

} // namespace orizon::engine
