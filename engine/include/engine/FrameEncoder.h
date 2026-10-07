#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orizon::engine {

// Header values of one display frame; the layout is protocol::FrameHeader.
struct FrameHeaderValues
{
    std::uint32_t sequence = 0;
    protocol::FrameMode mode = protocol::FrameMode::kRaw;
    std::uint8_t flags = 0; // protocol::FrameFlag bits.
    std::uint8_t views = 0; // protocol::ChannelView bit mask.
    std::uint32_t pointCount = 0;
    float samplesPerPoint = 1.0f;
    float startFraction = 0.0f;
    float peak = 0.0f;
    float rms = 0.0f;
    float frequency = 0.0f; // 0 when unavailable.
    float cursor = -1.0f;   // Sweep playhead position (0 to 1), or -1 (FR-9).
    // Musical context of a synced sweep in quarter notes; 0 otherwise (FR-9).
    float cycleStart = 0.0f;
    float spanQuarterNotes = 0.0f;
    float barQuarterNotes = 0.0f;
    // The part of the span covered, and its length in samples (FR-10).
    float viewStart = 0.0f;
    float viewEnd = 1.0f;
    float visibleSamples = 0.0f;
};

// Writes the header and `viewData` (each view's points, in ChannelView order) little-endian into
// `output`, which keeps its capacity across calls.
void encodeFrame(const FrameHeaderValues& header, std::span<const float> viewData,
                 std::vector<std::byte>& output);

} // namespace orizon::engine
