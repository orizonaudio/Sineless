#pragma once

// Decodes the analyzer's binary frames for tests: the header fields and the values after it.

#include "protocol/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace orizon::engine {

struct DecodedFrame
{
    std::uint32_t sequence = 0;
    protocol::FrameMode mode = protocol::FrameMode::kRaw;
    std::uint8_t flags = 0;
    std::uint8_t views = 0;
    std::uint8_t laneCount = 0;
    std::uint32_t laneSet = 0;
    std::uint32_t pointCount = 0;
    float samplesPerPoint = 0.0f;
    float startFraction = 0.0f;
    float peak = 0.0f;
    float rms = 0.0f;
    float frequency = 0.0f;
    float cursor = -1.0f;
    float viewStart = 0.0f;
    float viewEnd = 1.0f;
    float visibleSamples = 0.0f;
    std::uint32_t invalidSamples = 0;
    std::uint32_t invalidLanes = 0;
    std::vector<float> data;

    [[nodiscard]] bool has(protocol::FrameFlag value) const
    {
        return (flags & static_cast<std::uint8_t>(value)) != 0;
    }
};

template <typename Value> Value get(const std::vector<std::byte>& bytes, std::size_t offset)
{
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

inline DecodedFrame decode(const std::vector<std::byte>& bytes)
{
    DecodedFrame frame;
    frame.sequence = get<std::uint32_t>(bytes, protocol::FrameHeader::kSequenceOffset);
    frame.mode = static_cast<protocol::FrameMode>(
        get<std::uint8_t>(bytes, protocol::FrameHeader::kModeOffset));
    frame.flags = get<std::uint8_t>(bytes, protocol::FrameHeader::kFlagsOffset);
    frame.views = get<std::uint8_t>(bytes, protocol::FrameHeader::kViewsOffset);
    frame.laneCount = get<std::uint8_t>(bytes, protocol::FrameHeader::kLaneCountOffset);
    frame.laneSet = get<std::uint32_t>(bytes, protocol::FrameHeader::kLaneSetOffset);
    frame.pointCount = get<std::uint32_t>(bytes, protocol::FrameHeader::kPointCountOffset);
    frame.samplesPerPoint = get<float>(bytes, protocol::FrameHeader::kSamplesPerPointOffset);
    frame.startFraction = get<float>(bytes, protocol::FrameHeader::kStartFractionOffset);
    frame.peak = get<float>(bytes, protocol::FrameHeader::kPeakOffset);
    frame.rms = get<float>(bytes, protocol::FrameHeader::kRmsOffset);
    frame.frequency = get<float>(bytes, protocol::FrameHeader::kFrequencyOffset);
    frame.cursor = get<float>(bytes, protocol::FrameHeader::kCursorOffset);
    frame.viewStart = get<float>(bytes, protocol::FrameHeader::kViewStartOffset);
    frame.viewEnd = get<float>(bytes, protocol::FrameHeader::kViewEndOffset);
    frame.visibleSamples = get<float>(bytes, protocol::FrameHeader::kVisibleSamplesOffset);
    frame.invalidSamples = get<std::uint32_t>(bytes, protocol::FrameHeader::kInvalidSamplesOffset);
    frame.invalidLanes = get<std::uint32_t>(bytes, protocol::FrameHeader::kInvalidLanesOffset);
    frame.data.resize((bytes.size() - protocol::FrameHeader::kSize) / sizeof(float));
    std::memcpy(frame.data.data(), bytes.data() + protocol::FrameHeader::kSize,
                frame.data.size() * sizeof(float));
    return frame;
}

} // namespace orizon::engine
