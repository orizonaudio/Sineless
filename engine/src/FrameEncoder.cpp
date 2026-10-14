#include "engine/FrameEncoder.h"

#include <bit>
#include <cstring>

namespace orizon::engine {

static_assert(std::endian::native == std::endian::little,
              "The frame layout is little-endian; big-endian hosts need byte swapping");

namespace {

template <typename Value> void put(std::vector<std::byte>& output, std::size_t offset, Value value)
{
    std::memcpy(output.data() + offset, &value, sizeof(Value));
}

} // namespace

void encodeFrame(const FrameHeaderValues& header, std::span<const float> viewData,
                 std::vector<std::byte>& output)
{
    using Layout = protocol::FrameHeader;
    output.resize(Layout::kSize + viewData.size_bytes());
    put(output, Layout::kModeOffset, static_cast<std::uint8_t>(header.mode));
    put(output, Layout::kFlagsOffset, header.flags);
    put(output, Layout::kViewsOffset, header.views);
    put(output, Layout::kLaneCountOffset, header.laneCount);
    put(output, Layout::kPointCountOffset, header.pointCount);
    put(output, Layout::kStartFractionOffset, header.startFraction);
    put(output, Layout::kPeakOffset, header.peak);
    put(output, Layout::kRmsOffset, header.rms);
    put(output, Layout::kFrequencyOffset, header.frequency);
    put(output, Layout::kCursorOffset, header.cursor);
    put(output, Layout::kCycleStartOffset, header.cycleStart);
    put(output, Layout::kSpanQuarterNotesOffset, header.spanQuarterNotes);
    put(output, Layout::kBarQuarterNotesOffset, header.barQuarterNotes);
    put(output, Layout::kViewStartOffset, header.viewStart);
    put(output, Layout::kViewEndOffset, header.viewEnd);
    put(output, Layout::kVisibleSamplesOffset, header.visibleSamples);
    put(output, Layout::kLaneSetOffset, header.laneSet);
    put(output, Layout::kSumGainOffset, header.sumGain);
    put(output, Layout::kFixedGainOffset, header.fixedGain);
    put(output, Layout::kCycleSecondsOffset, header.cycleSeconds);
    put(output, Layout::kPanReachOffset, header.panReach);
    put(output, Layout::kInvalidSamplesOffset, header.invalidSamples);
    put(output, Layout::kInvalidLanesOffset, header.invalidLanes);
    if (!viewData.empty()) {
        std::memcpy(output.data() + Layout::kSize, viewData.data(), viewData.size_bytes());
    }
}

} // namespace orizon::engine
