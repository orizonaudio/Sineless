#include "engine/SweepImage.h"

#include "engine/ChannelViews.h"

#include <algorithm>
#include <cstddef>

namespace orizon::engine {

namespace {

// The most samples read from the source at once: bounds the working space however long the
// sweep.
constexpr std::uint64_t kChunkSamples = 16384;

} // namespace

SweepImage::SweepImage(int channels) : channelCount(channels) {}

bool SweepImage::update(const SampleSource& source, const SweepWindow& window, bool rebuild,
                        int inputChannels)
{
    const std::uint64_t length = window.sampleCount;
    const std::int64_t end = window.start + static_cast<std::int64_t>(length);
    if (rebuild || !sweepLengthsSimilar(length, count)) {
        count = length;
        samples.assign(static_cast<std::size_t>(count) * static_cast<std::size_t>(channelCount),
                       0.0f);
        placedEnd = 0;
        placedSinceBuild = 0;
        changed.clear();
        markChanged(0, count);
    }
    if (count == 0) {
        return true;
    }
    // What the source holds of the window and is not on screen yet.
    const std::int64_t first = std::max({static_cast<std::int64_t>(placedEnd), window.start,
                                         static_cast<std::int64_t>(source.beginIndex())});
    if (first >= end) {
        return true;
    }
    // Source sample i shows at screen position (i - cycleStart) mod count, where the cycle start
    // is `start + rotation` (the image's length may differ a little from the window's); the new
    // samples fill one run of positions, wrapping at the right edge at most once. They are read
    // in chunks, so the working space stays small however long the sweep.
    const std::int64_t cycleStart = window.start + static_cast<std::int64_t>(window.rotation);
    const auto signedCount = static_cast<std::int64_t>(count);
    const auto total = static_cast<std::uint64_t>(end - first);
    for (std::uint64_t done = 0; done < total;) {
        const auto chunkFirst = static_cast<std::uint64_t>(first) + done;
        const std::uint64_t chunk = std::min({kChunkSamples, total - done, count});
        const std::int64_t fromCycle =
            (static_cast<std::int64_t>(chunkFirst) - cycleStart) % signedCount;
        const auto position =
            static_cast<std::uint64_t>(fromCycle < 0 ? fromCycle + signedCount : fromCycle);
        const std::uint64_t beforeWrap = std::min(chunk, count - position);
        scratch.resize(static_cast<std::size_t>(chunk));
        for (int channel = 0; channel < channelCount; ++channel) {
            bool read = false;
            if (channelCount == 1) {
                viewScratch.resize(scratch.size());
                read = readView(source, inputChannels, protocol::ChannelView::kMid, chunkFirst,
                                scratch, viewScratch);
            } else {
                read = source.read(channel, chunkFirst, scratch);
            }
            if (!read) {
                count = 0; // Partly overwritten: rebuild on the next update.
                return false;
            }
            float* const block = samples.data() + static_cast<std::size_t>(channel) * count;
            std::copy_n(scratch.begin(), static_cast<std::ptrdiff_t>(beforeWrap), block + position);
            std::copy_n(scratch.begin() + static_cast<std::ptrdiff_t>(beforeWrap),
                        static_cast<std::ptrdiff_t>(chunk - beforeWrap), block);
        }
        markChanged(position, position + beforeWrap);
        markChanged(0, chunk - beforeWrap);
        done += chunk;
    }
    placedEnd = static_cast<std::uint64_t>(end);
    placedSinceBuild += total;
    return true;
}

void SweepImage::clear()
{
    count = 0;
    placedEnd = 0;
    placedSinceBuild = 0;
    changed.clear();
    samples.clear();
    samples.shrink_to_fit();
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
    if (channel < 0 || channel >= channelCount || start > count ||
        destination.size() > count - start) {
        return false;
    }
    const float* const block = samples.data() + static_cast<std::size_t>(channel) * count;
    std::copy_n(block + start, destination.size(), destination.begin());
    return true;
}

} // namespace orizon::engine
