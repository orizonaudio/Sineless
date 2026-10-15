#include "engine/LaneHold.h"

#include "engine/ChannelViews.h"

#include <algorithm>

namespace orizon::engine {

bool LaneHold::capture(const Engine& engine, std::size_t length, std::vector<float>& scratch)
{
    const CaptureRing& ring = engine.captureRing();
    const MusicalStamp clockNow = engine.timeline().latest();
    const double rateNow = engine.sampleRate();
    const std::uint64_t ringEnd = ring.endIndex();
    const auto count = std::min<std::uint64_t>(length, ringEnd - ring.beginIndex());
    mid.resize(static_cast<std::size_t>(count));
    scratch.resize(std::max(scratch.size(), mid.size()));
    isHeld = readView(ring, engine.inputChannels(), protocol::ChannelView::kMid, ringEnd - count,
                      mid, scratch);
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
    mid.clear();
    mid.shrink_to_fit();
    begin = 0;
    end = 0;
    isHeld = false;
}

bool LaneHold::read(int channel, std::uint64_t start, std::span<float> destination) const
{
    if (!isHeld || channel < 0 || channel > 1 || start < begin ||
        start + destination.size() > end) {
        return false;
    }
    std::copy_n(mid.begin() + static_cast<std::ptrdiff_t>(start - begin), destination.size(),
                destination.begin());
    return true;
}

} // namespace orizon::engine
