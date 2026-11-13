#pragma once

// Decodes the analyzer's binary frames for tests: the header fields and the values after it.

#include "protocol/Protocol.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace orizon::engine {

struct DecodedFrame
{
    protocol::FrameMode mode = protocol::FrameMode::kRaw;
    std::uint8_t flags = 0;
    std::uint8_t views = 0;
    std::uint8_t laneCount = 0;
    std::uint32_t laneSet = 0;
    std::uint32_t pointCount = 0;
    float startFraction = 0.0f;
    float peak = 0.0f;
    float rms = 0.0f;
    float cursor = -1.0f;
    double viewStart = 0.0;
    double viewEnd = 1.0;
    float visibleSamples = 0.0f;
    std::uint32_t invalidSamples = 0;
    std::uint32_t invalidLanes = 0;
    float sampleRate = 0.0f;
    float spanSeconds = 0.0f;
    protocol::ChannelView readoutView = protocol::ChannelView::kMid;
    protocol::ChannelView triggerView = protocol::ChannelView::kMid;
    float triggerPosition = 0.0f;
    std::uint32_t pointsPerSample = 1;
    float sumGain = 0.0f;
    float fixedGain = 0.0f;
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
    frame.mode = static_cast<protocol::FrameMode>(
        get<std::uint8_t>(bytes, protocol::FrameHeader::kModeOffset));
    frame.flags = get<std::uint8_t>(bytes, protocol::FrameHeader::kFlagsOffset);
    frame.views = get<std::uint8_t>(bytes, protocol::FrameHeader::kViewsOffset);
    frame.laneCount = get<std::uint8_t>(bytes, protocol::FrameHeader::kLaneCountOffset);
    frame.laneSet = get<std::uint32_t>(bytes, protocol::FrameHeader::kLaneSetOffset);
    frame.pointCount = get<std::uint32_t>(bytes, protocol::FrameHeader::kPointCountOffset);
    frame.startFraction = get<float>(bytes, protocol::FrameHeader::kStartFractionOffset);
    frame.peak = get<float>(bytes, protocol::FrameHeader::kPeakOffset);
    frame.rms = get<float>(bytes, protocol::FrameHeader::kRmsOffset);
    frame.cursor = get<float>(bytes, protocol::FrameHeader::kCursorOffset);
    frame.viewStart = get<double>(bytes, protocol::FrameHeader::kViewStartOffset);
    frame.viewEnd = get<double>(bytes, protocol::FrameHeader::kViewEndOffset);
    frame.visibleSamples = get<float>(bytes, protocol::FrameHeader::kVisibleSamplesOffset);
    frame.invalidSamples = get<std::uint32_t>(bytes, protocol::FrameHeader::kInvalidSamplesOffset);
    frame.invalidLanes = get<std::uint32_t>(bytes, protocol::FrameHeader::kInvalidLanesOffset);
    frame.sampleRate = get<float>(bytes, protocol::FrameHeader::kSampleRateOffset);
    frame.spanSeconds = get<float>(bytes, protocol::FrameHeader::kSpanSecondsOffset);
    frame.readoutView = static_cast<protocol::ChannelView>(
        get<std::uint32_t>(bytes, protocol::FrameHeader::kReadoutViewOffset));
    frame.triggerView = static_cast<protocol::ChannelView>(
        get<std::uint32_t>(bytes, protocol::FrameHeader::kTriggerViewOffset));
    frame.triggerPosition = get<float>(bytes, protocol::FrameHeader::kTriggerPositionOffset);
    frame.pointsPerSample =
        get<std::uint32_t>(bytes, protocol::FrameHeader::kPointsPerSampleOffset);
    frame.sumGain = get<float>(bytes, protocol::FrameHeader::kSumGainOffset);
    frame.fixedGain = get<float>(bytes, protocol::FrameHeader::kFixedGainOffset);
    frame.data.resize((bytes.size() - protocol::FrameHeader::kSize) / sizeof(float));
    if (!frame.data.empty()) { // A frame of no points: no data to copy from or to.
        std::memcpy(frame.data.data(), bytes.data() + protocol::FrameHeader::kSize,
                    frame.data.size() * sizeof(float));
    }
    return frame;
}

// The frame with only its samples: a raw frame's every pointsPerSample-th point, without the wave
// reconstructed between them, for tests of which samples are shown and where (a deep view's
// points between samples are tested on their own). Data after the traces (matches) is kept.
inline DecodedFrame samplesOnly(DecodedFrame frame)
{
    const std::size_t factor = frame.pointsPerSample;
    if (frame.mode != protocol::FrameMode::kRaw || factor <= 1 || frame.pointCount == 0) {
        return frame;
    }
    const std::size_t points = frame.pointCount;
    const std::size_t samples = ((points - 1) / factor) + 1;
    const std::size_t traces =
        static_cast<std::size_t>(std::popcount(frame.views)) + std::size_t{frame.laneCount};
    std::vector<float> data;
    data.reserve(frame.data.size());
    for (std::size_t trace = 0; trace < traces; ++trace) {
        for (std::size_t sample = 0; sample < samples; ++sample) {
            data.push_back(frame.data.at((trace * points) + (sample * factor)));
        }
    }
    data.insert(data.end(), frame.data.begin() + static_cast<std::ptrdiff_t>(traces * points),
                frame.data.end());
    frame.data = std::move(data);
    frame.pointCount = static_cast<std::uint32_t>(samples);
    frame.pointsPerSample = 1;
    return frame;
}

} // namespace orizon::engine
