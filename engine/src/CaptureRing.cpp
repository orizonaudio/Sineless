#include "engine/CaptureRing.h"

#include <bit>
#include <cassert>

namespace orizon::engine {

// Synchronisation follows the seqlock pattern with fences. The writer publishes `pendingIndex`,
// issues a release fence, then stores samples. A reader that observes any of those stores and
// then issues an acquire fence is guaranteed to see the new `pendingIndex`, so it detects that
// its range was overwritten. Samples are accessed through relaxed std::atomic_ref, which
// compiles to plain loads and stores, so concurrent access is well-defined.

CaptureRing::CaptureRing(std::size_t capacity)
    : capacityValue(capacity), mask(capacity - 1), samples(capacity * kMaxChannels, 0.0f)
{
    assert(std::has_single_bit(capacity));
}

void CaptureRing::write(const float* const* channels, int numChannels,
                        int numSamples) noexcept ORIZON_NONBLOCKING
{
    writeSamples(channels, numChannels, numSamples);
}

void CaptureRing::write(const double* const* channels, int numChannels,
                        int numSamples) noexcept ORIZON_NONBLOCKING
{
    writeSamples(channels, numChannels, numSamples);
}

template <typename Sample>
void CaptureRing::writeSamples(const Sample* const* channels, int numChannels,
                               int numSamples) noexcept ORIZON_NONBLOCKING
{
    if (numSamples <= 0) {
        return;
    }
    const auto count = static_cast<std::uint64_t>(numSamples);
    const std::uint64_t start = writerIndex;
    const std::uint64_t end = start + count;

    pendingIndex.store(end, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    for (int channel = 0; channel < kMaxChannels; ++channel) {
        const Sample* source = channel < numChannels ? channels[channel] : nullptr;
        float* block = samples.data() + static_cast<std::size_t>(channel) * capacityValue;
        for (std::uint64_t i = 0; i < count; ++i) {
            const float value = source != nullptr ? static_cast<float>(source[i]) : 0.0f;
            std::atomic_ref<float>(block[(start + i) & mask])
                .store(value, std::memory_order_relaxed);
        }
    }

    writerIndex = end;
    publishedIndex.store(end, std::memory_order_release);
}

std::uint64_t CaptureRing::writeIndex() const noexcept
{
    return publishedIndex.load(std::memory_order_acquire);
}

bool CaptureRing::read(int channel, std::uint64_t start, std::span<float> destination) const
{
    if (channel < 0 || channel >= kMaxChannels || destination.size() > capacityValue) {
        return false;
    }
    const std::uint64_t end = start + destination.size();
    const std::uint64_t published = publishedIndex.load(std::memory_order_acquire);
    if (end > published || published - start > capacityValue) {
        return false;
    }

    const float* block = samples.data() + static_cast<std::size_t>(channel) * capacityValue;
    for (std::size_t i = 0; i < destination.size(); ++i) {
        // atomic_ref needs a non-const object; the load does not modify it.
        auto& slot = const_cast<float&>(block[(start + i) & mask]);
        destination[i] = std::atomic_ref<float>(slot).load(std::memory_order_relaxed);
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t pending = pendingIndex.load(std::memory_order_relaxed);
    return pending - start <= capacityValue;
}

} // namespace orizon::engine
