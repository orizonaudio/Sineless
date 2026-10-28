#include "engine/CaptureRing.h"

#include <algorithm>
#include <bit>
#include <cassert>

namespace orizon::engine {

// Synchronisation follows the seqlock pattern with fences. The writer publishes `pendingIndex`,
// issues a release fence, then stores samples. A reader that observes any of those stores and
// then issues an acquire fence is guaranteed to see the new `pendingIndex`, so it detects that
// its range was overwritten. Samples are accessed through relaxed std::atomic_ref, which
// compiles to plain loads and stores, so concurrent access is well-defined.
//
// A resize swaps in a new storage, which begins at the published index. A reader reads the
// published index before it takes the storage, and copies only samples below it. If a resize came
// in between, the storage taken begins at or after that index, and the read fails. If not, every
// sample below it was written into the storage taken, and a later resize leaves that storage to the
// reader alone, kept alive by the reader's shared_ptr: the writer moves on to the new one.

CaptureRing::Storage::Storage(std::size_t size, std::uint64_t firstIndex)
    : capacity(size), mask(size - 1), first(firstIndex), samples(size * kMaxChannels, 0.0f)
{
    assert(std::has_single_bit(size));
}

CaptureRing::CaptureRing(std::size_t capacity)
    : storage(std::make_shared<Storage>(capacity, 0)), writerStorage(storage.get())
{}

void CaptureRing::resize(std::size_t capacity)
{
    const std::scoped_lock lock(storageMutex);
    if (storage->capacity == capacity) {
        return; // Keeps what it holds.
    }
    storage = std::make_shared<Storage>(capacity, publishedIndex.load(std::memory_order_acquire));
    writerStorage.store(storage.get(), std::memory_order_release);
}

std::shared_ptr<const CaptureRing::Storage> CaptureRing::current() const
{
    const std::scoped_lock lock(storageMutex);
    return storage;
}

std::size_t CaptureRing::capacity() const
{
    return current()->capacity;
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
    Storage& into = *writerStorage.load(std::memory_order_acquire);

    pendingIndex.store(end, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    for (int channel = 0; channel < kMaxChannels; ++channel) {
        const Sample* source = channel < numChannels ? channels[channel] : nullptr;
        float* block = into.samples.data() + static_cast<std::size_t>(channel) * into.capacity;
        for (std::uint64_t i = 0; i < count; ++i) {
            const float value = source != nullptr ? static_cast<float>(source[i]) : 0.0f;
            std::atomic_ref<float>(block[(start + i) & into.mask])
                .store(value, std::memory_order_relaxed);
        }
    }

    writerIndex = end;
    publishedIndex.store(end, std::memory_order_release);
}

std::uint64_t CaptureRing::writeIndex() const noexcept ORIZON_NONBLOCKING
{
    return publishedIndex.load(std::memory_order_acquire);
}

std::uint64_t CaptureRing::beginIndex() const
{
    const std::shared_ptr<const Storage> held = current();
    const std::uint64_t end = writeIndex();
    const std::uint64_t oldest = end > held->capacity ? end - held->capacity : 0;
    return std::max(oldest, std::min(held->first, end));
}

bool CaptureRing::read(int channel, std::uint64_t start, std::span<float> destination) const
{
    // Before the storage is taken (see the top of this file).
    const std::uint64_t published = publishedIndex.load(std::memory_order_acquire);
    const std::shared_ptr<const Storage> held = current();
    const std::size_t size = held->capacity;
    if (channel < 0 || channel >= kMaxChannels || start < held->first) {
        return false;
    }
    const std::uint64_t end = start + destination.size();
    if (end > published || published - start > size) {
        return false;
    }

    const float* block = held->samples.data() + static_cast<std::size_t>(channel) * size;
    for (std::size_t i = 0; i < destination.size(); ++i) {
        // atomic_ref needs a non-const object; the load does not modify it.
        auto& slot = const_cast<float&>(block[(start + i) & held->mask]);
        destination[i] = std::atomic_ref<float>(slot).load(std::memory_order_relaxed);
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t pending = pendingIndex.load(std::memory_order_relaxed);
    return pending - start <= size;
}

} // namespace orizon::engine
