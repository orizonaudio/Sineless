#include "engine/ChannelViews.h"

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace orizon::engine {

using protocol::ChannelView;

namespace {

constexpr std::uint8_t bitOf(ChannelView view)
{
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(view));
}

} // namespace

std::uint8_t effectiveViews(std::uint8_t requested, int inputChannels)
{
    return inputChannels < 2 ? bitOf(ChannelView::kLeft) : requested;
}

ChannelView effectiveTriggerSource(ChannelView requested, int inputChannels)
{
    return inputChannels < 2 ? ChannelView::kLeft : requested;
}

bool readView(const SampleSource& source, int inputChannels, ChannelView view, std::uint64_t start,
              std::span<float> destination, std::span<float> scratch)
{
    if (inputChannels < 2 || view == ChannelView::kLeft) {
        return source.read(0, start, destination);
    }
    if (view == ChannelView::kRight) {
        return source.read(1, start, destination);
    }
    assert(scratch.size() >= destination.size());
    const std::span<float> right = scratch.first(destination.size());
    if (!source.read(0, start, destination) || !source.read(1, start, right)) {
        return false;
    }
    const float sign = view == ChannelView::kMid ? 1.0f : -1.0f;
    for (std::size_t i = 0; i < destination.size(); ++i) {
        destination[i] = 0.5f * (destination[i] + sign * right[i]);
    }
    return true;
}

bool readViewHeld(const SampleSource& source, int inputChannels, ChannelView view,
                  std::uint64_t start, std::span<float> destination, std::span<float> scratch)
{
    const std::uint64_t end = start + destination.size();
    const std::uint64_t first = std::clamp(source.beginIndex(), start, end);
    const std::uint64_t last = std::clamp(source.endIndex(), first, end);
    std::ranges::fill(destination, 0.0f);
    if (first < last && !readView(source, inputChannels, view, first,
                                  destination.subspan(static_cast<std::size_t>(first - start),
                                                      static_cast<std::size_t>(last - first)),
                                  scratch)) {
        std::ranges::fill(destination, 0.0f); // Overwritten during the copy.
        return false;
    }
    return first == start && last == end;
}

} // namespace orizon::engine
