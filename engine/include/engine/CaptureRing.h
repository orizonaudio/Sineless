#pragma once

#include "engine/RealtimeSafety.h"
#include "engine/SampleSource.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orizon::engine {

// Per-instance history of the input signal (DESIGN 5.2). The audio thread is the only writer;
// any number of non-real-time readers copy ranges out of it without locks.
//
// Samples are addressed by a monotonic 64-bit index: the n-th sample ever written has index n.
// A read fails, rather than returning mixed data, if the writer overwrote any part of the range
// while it was being copied.
class CaptureRing final : public SampleSource
{
public:
    // 2^20 samples per channel: 5.46 s at 192 kHz. Covers the 4 s maximum span (FR-2) plus a
    // 1.46 s margin for the trigger search of spans up to 1 s (FR-3) and for readers copying while
    // the audio thread keeps writing.
    static constexpr std::size_t kDefaultCapacity = std::size_t{1} << 20U;
    static constexpr int kMaxChannels = 2;

    // Allocates the whole buffer. Not real-time safe. `capacity` must be a power of two.
    explicit CaptureRing(std::size_t capacity = kDefaultCapacity);

    // Audio thread. `channels` holds `numChannels` pointers (at most kMaxChannels) to
    // `numSamples` samples each; a null pointer writes silence for that channel.
    void write(const float* const* channels, int numChannels,
               int numSamples) noexcept ORIZON_NONBLOCKING;
    void write(const double* const* channels, int numChannels,
               int numSamples) noexcept ORIZON_NONBLOCKING;

    // Index one past the newest sample published by the writer.
    [[nodiscard]] std::uint64_t writeIndex() const noexcept;

    [[nodiscard]] std::uint64_t endIndex() const override { return writeIndex(); }
    [[nodiscard]] std::uint64_t beginIndex() const override;

    // Copies `destination.size()` samples of `channel`, starting at sample index `start`.
    // Returns false if any of them is not yet written or was overwritten before the copy ended.
    [[nodiscard]] bool read(int channel, std::uint64_t start,
                            std::span<float> destination) const override;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacityValue; }

private:
    template <typename Sample>
    void writeSamples(const Sample* const* channels, int numChannels,
                      int numSamples) noexcept ORIZON_NONBLOCKING;

    std::size_t capacityValue;
    std::size_t mask;
    std::vector<float> samples; // kMaxChannels blocks of `capacity` samples.

    // Writer-only copy of the write index; avoids reading the atomic back on the audio thread.
    std::uint64_t writerIndex = 0;
    // Published before a block is written: samples below `pendingIndex - capacity` are
    // invalid from this point on.
    std::atomic<std::uint64_t> pendingIndex{0};
    // Published after a block is written: samples below this index are readable.
    std::atomic<std::uint64_t> publishedIndex{0};

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic_ref<float>::is_always_lock_free);
};

} // namespace orizon::engine
