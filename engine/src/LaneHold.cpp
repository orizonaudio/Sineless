#include "engine/LaneHold.h"

#include "engine/ChannelViews.h"

#include <algorithm>

namespace orizon::engine {

bool LaneHold::capture(const Engine& engine, protocol::ChannelView view, std::size_t length,
                       std::vector<float>& scratch, bool allChannels)
{
    const CaptureRing& ring = engine.captureRing();
    const MusicalStamp clockNow = engine.timeline().latest();
    const double rateNow = engine.sampleRate();
    const int inputChannels = engine.inputChannels();
    const std::uint64_t ringEnd = ring.endIndex();
    const auto count = std::min<std::uint64_t>(length, ringEnd - ring.beginIndex());
    const auto blockSize = static_cast<std::size_t>(count);
    // A mono track's one channel is every view of it.
    channelCount = allChannels ? inputChannels : 1;
    heldView = view;
    everyView = allChannels || inputChannels < 2;
    samples.resize(blockSize * static_cast<std::size_t>(channelCount));
    if (allChannels) {
        isHeld = true;
        for (int channel = 0; channel < channelCount && isHeld; ++channel) {
            isHeld = ring.read(channel, ringEnd - count,
                               std::span(samples).subspan(
                                   static_cast<std::size_t>(channel) * blockSize, blockSize));
        }
    } else {
        scratch.resize(std::max(scratch.size(), blockSize));
        isHeld = readView(ring, inputChannels, view, ringEnd - count, samples, scratch);
    }
    if (!isHeld) {
        release();
        return false;
    }
    begin = ringEnd - count;
    end = ringEnd;
    stamp = clockNow;
    rate = rateNow;
    return true;
}

void LaneHold::release()
{
    samples.clear();
    samples.shrink_to_fit();
    begin = 0;
    end = 0;
    isHeld = false;
}

bool LaneHold::read(int channel, std::uint64_t start, std::span<float> destination) const
{
    if (!isHeld || channel < 0 || channel >= channelCount || start < begin ||
        start + destination.size() > end) {
        return false;
    }
    const auto block =
        static_cast<std::ptrdiff_t>(channel) * static_cast<std::ptrdiff_t>(end - begin);
    std::copy_n(samples.begin() + block + static_cast<std::ptrdiff_t>(start - begin),
                destination.size(), destination.begin());
    return true;
}

} // namespace orizon::engine
