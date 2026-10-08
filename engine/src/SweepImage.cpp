#include "engine/SweepImage.h"

#include "engine/CaptureRing.h"

#include <algorithm>
#include <cstddef>

namespace orizon::engine {

bool SweepImage::update(const SampleSource& source, const SweepWindow& window, bool rebuild)
{
    const std::uint64_t length = window.sampleCount;
    const std::uint64_t end = window.start + length;
    if (rebuild || length != count) {
        count = length;
        samples.assign(static_cast<std::size_t>(count) * CaptureRing::kMaxChannels, 0.0f);
        placedEnd = 0;
        changed.clear();
        markChanged(0, count);
    }
    const std::uint64_t first = std::max(placedEnd, window.start);
    if (first >= end) {
        return true;
    }
    // Source sample i shows at screen position (i - start - rotation) mod count; the new samples
    // fill one run of positions, wrapping at the right edge at most once.
    const std::uint64_t position = (first - window.start + count - window.rotation) % count;
    const std::uint64_t total = end - first;
    const std::uint64_t beforeWrap = std::min(total, count - position);
    for (int channel = 0; channel < CaptureRing::kMaxChannels; ++channel) {
        float* const block = samples.data() + static_cast<std::size_t>(channel) * count;
        if (!source.read(channel, first,
                         std::span(block + position, static_cast<std::size_t>(beforeWrap))) ||
            !source.read(channel, first + beforeWrap,
                         std::span(block, static_cast<std::size_t>(total - beforeWrap)))) {
            count = 0; // Partly overwritten: rebuild on the next update.
            return false;
        }
    }
    placedEnd = end;
    markChanged(position, position + beforeWrap);
    markChanged(0, total - beforeWrap);
    return true;
}

// Ranges pile up only while no frame is taken; past a few, the whole screen counts as changed.
void SweepImage::markChanged(std::uint64_t first, std::uint64_t end)
{
    constexpr std::size_t kMaxRanges = 8;
    if (first >= end) {
        return;
    }
    if (changed.size() >= kMaxRanges) {
        changed.assign(1, {.first = 0, .end = count});
        return;
    }
    changed.push_back({.first = first, .end = end});
}

bool SweepImage::read(int channel, std::uint64_t start, std::span<float> destination) const
{
    if (channel < 0 || channel >= CaptureRing::kMaxChannels || start > count ||
        destination.size() > count - start) {
        return false;
    }
    const float* const block = samples.data() + static_cast<std::size_t>(channel) * count;
    std::copy_n(block + start, destination.size(), destination.begin());
    return true;
}

} // namespace orizon::engine
