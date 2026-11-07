#include "editor/FrameView.h"

#include <cstring>

namespace orizon::editor {

namespace {

template <typename Value> Value field(std::span<const std::byte> bytes, std::size_t offset)
{
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

} // namespace

std::optional<FrameView> FrameView::read(std::span<const std::byte> bytes)
{
    using protocol::FrameHeader;
    if (bytes.size() < FrameHeader::kSize) {
        return std::nullopt;
    }
    FrameView frame;
    frame.mode =
        static_cast<protocol::FrameMode>(field<std::uint8_t>(bytes, FrameHeader::kModeOffset));
    frame.flags = field<std::uint8_t>(bytes, FrameHeader::kFlagsOffset);
    frame.pointCount = field<std::uint32_t>(bytes, FrameHeader::kPointCountOffset);
    frame.laneSet = field<std::uint32_t>(bytes, FrameHeader::kLaneSetOffset);
    frame.startFraction = field<float>(bytes, FrameHeader::kStartFractionOffset);
    frame.cursor = field<float>(bytes, FrameHeader::kCursorOffset);
    frame.cycleStart = field<float>(bytes, FrameHeader::kCycleStartOffset);
    frame.spanQuarterNotes = field<float>(bytes, FrameHeader::kSpanQuarterNotesOffset);
    frame.barQuarterNotes = field<float>(bytes, FrameHeader::kBarQuarterNotesOffset);
    frame.viewStart = field<float>(bytes, FrameHeader::kViewStartOffset);
    frame.viewEnd = field<float>(bytes, FrameHeader::kViewEndOffset);
    frame.visibleSamples = field<float>(bytes, FrameHeader::kVisibleSamplesOffset);
    frame.triggerPosition = field<float>(bytes, FrameHeader::kTriggerPositionOffset);
    const auto viewMask = field<std::uint8_t>(bytes, FrameHeader::kViewsOffset);
    const auto laneCount = field<std::uint8_t>(bytes, FrameHeader::kLaneCountOffset);

    const std::size_t perPoint = frame.mode == protocol::FrameMode::kMinMax ? 2 : 1;
    const std::size_t perBlock = std::size_t{frame.pointCount} * perPoint;
    const std::size_t available = (bytes.size() - FrameHeader::kSize) / sizeof(float);
    // The float data: frames live in vectors (allocations aligned for any scalar) and the header is
    // a whole number of floats long, so the values are aligned.
    static_assert(FrameHeader::kSize % sizeof(float) == 0);
    const auto* values = reinterpret_cast<const float*>(bytes.data() + FrameHeader::kSize);
    std::size_t used = 0;
    const auto next = [&]() -> std::optional<std::span<const float>> {
        if (used + perBlock > available) {
            return std::nullopt;
        }
        const std::span<const float> block(values + used, perBlock);
        used += perBlock;
        return block;
    };
    for (std::size_t view = 0; view < protocol::kChannelViewCount; ++view) {
        if ((viewMask & (1U << view)) == 0) {
            continue;
        }
        const auto block = next();
        if (!block) {
            return std::nullopt;
        }
        frame.views.push_back({.channelView = static_cast<int>(view), .values = *block});
    }
    for (unsigned lane = 0; lane < laneCount; ++lane) {
        const auto block = next();
        if (!block) {
            return std::nullopt;
        }
        frame.lanes.push_back(*block);
    }
    if (frame.compared()) {
        const std::size_t tracks = laneCount > 0 ? laneCount - 1U : 0U;
        if (used + tracks * protocol::kMatchValues > available) {
            return std::nullopt;
        }
        for (std::size_t track = 0; track < tracks; ++track) {
            const float polarity = values[used + track * protocol::kMatchValues + 2];
            frame.related.push_back(polarity != 0.0f);
        }
    }
    return frame;
}

} // namespace orizon::editor
