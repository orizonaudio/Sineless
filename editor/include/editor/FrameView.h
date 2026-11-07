#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace orizon::editor {

// What the macOS editor draws natively reads from a frame (protocol/src/frame.ts): the header
// fields that place the traces, the views or lanes (spans into the frame's bytes), and Compare's
// matches. A view of the bytes, which must outlive it; nullopt when they are shorter than the
// header announces, as the page's decoder refuses them.
struct FrameView
{
    protocol::FrameMode mode = protocol::FrameMode::kRaw;
    std::uint8_t flags = 0;
    std::uint32_t pointCount = 0;
    std::uint32_t laneSet = 0;
    float startFraction = 0.0f;
    float cursor = -1.0f;
    float cycleStart = 0.0f;
    float spanQuarterNotes = 0.0f;
    float barQuarterNotes = 0.0f;
    float viewStart = 0.0f;
    float viewEnd = 1.0f;
    float visibleSamples = 0.0f;
    // Where the trigger point sits across the unpanned span, as the engine placed it; NaN while
    // running free and in Beats.
    float triggerPosition = std::numeric_limits<float>::quiet_NaN();
    // Views present, in ChannelView order, each with its ChannelView index; empty in lane frames.
    struct View
    {
        int channelView = 0;
        std::span<const float> values;
    };
    std::vector<View> views;
    std::vector<std::span<const float>> lanes;
    // Compare frames: per track (lanes but the sum), whether it matches this track clearly.
    std::vector<bool> related;

    [[nodiscard]] bool has(protocol::FrameFlag flag) const
    {
        return (flags & static_cast<std::uint8_t>(flag)) != 0;
    }
    [[nodiscard]] bool compared() const { return has(protocol::FrameFlag::kCompared); }

    static std::optional<FrameView> read(std::span<const std::byte> bytes);
};

} // namespace orizon::editor
