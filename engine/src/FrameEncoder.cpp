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
    put(output, Layout::kSequenceOffset, header.sequence);
    put(output, Layout::kModeOffset, static_cast<std::uint8_t>(header.mode));
    put(output, Layout::kFlagsOffset, header.flags);
    put(output, Layout::kViewsOffset, header.views);
    put(output, Layout::kReservedOffset, std::uint8_t{0});
    put(output, Layout::kPointCountOffset, header.pointCount);
    put(output, Layout::kSamplesPerPointOffset, header.samplesPerPoint);
    put(output, Layout::kStartFractionOffset, header.startFraction);
    put(output, Layout::kPeakOffset, header.peak);
    put(output, Layout::kRmsOffset, header.rms);
    put(output, Layout::kFrequencyOffset, header.frequency);
    put(output, Layout::kCursorOffset, header.cursor);
    put(output, Layout::kCycleStartOffset, header.cycleStart);
    put(output, Layout::kSpanQuarterNotesOffset, header.spanQuarterNotes);
    put(output, Layout::kBarQuarterNotesOffset, header.barQuarterNotes);
    if (!viewData.empty()) {
        std::memcpy(output.data() + Layout::kSize, viewData.data(), viewData.size_bytes());
    }
}

} // namespace orizon::engine
