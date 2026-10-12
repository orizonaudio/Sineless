#include "engine/HoldBuffer.h"

#include <algorithm>
#include <cmath>

namespace orizon::engine {

HoldBuffer::HoldBuffer(std::size_t capacity)
    : capacity(capacity), samples(capacity * CaptureRing::kMaxChannels)
{}

void HoldBuffer::resize(std::size_t size)
{
    if (size == capacity) {
        return;
    }
    capacity = size;
    samples.assign(size * CaptureRing::kMaxChannels, 0.0f);
    samples.shrink_to_fit();
    begin = 0;
    end = 0;
}

bool HoldBuffer::capture(const SampleSource& source, std::size_t length)
{
    const std::uint64_t sourceEnd = source.endIndex();
    const std::uint64_t available = sourceEnd - source.beginIndex();
    const auto count = std::min<std::uint64_t>({length, available, capacity});
    const std::uint64_t start = sourceEnd - count;
    for (int channel = 0; channel < CaptureRing::kMaxChannels; ++channel) {
        std::span<float> block(samples.data() + static_cast<std::size_t>(channel) * capacity,
                               static_cast<std::size_t>(count));
        if (!source.read(channel, start, block)) {
            begin = 0;
            end = 0;
            return false;
        }
    }
    begin = start;
    end = sourceEnd;
    return true;
}

std::size_t HoldBuffer::holdLength(double sampleRate) const
{
    const auto length = static_cast<std::size_t>(std::lround(kHoldSeconds * sampleRate));
    return std::min(length, capacity);
}

bool HoldBuffer::read(int channel, std::uint64_t start, std::span<float> destination) const
{
    if (channel < 0 || channel >= CaptureRing::kMaxChannels || start < begin ||
        start + destination.size() > end) {
        return false;
    }
    const float* block = samples.data() + static_cast<std::size_t>(channel) * capacity;
    std::copy_n(block + (start - begin), destination.size(), destination.begin());
    return true;
}

} // namespace orizon::engine
