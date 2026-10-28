#pragma once

#include "engine/RealtimeSafety.h"
#include "engine/SampleSource.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace orizon::engine {

// Seconds of audio a freeze in Seconds mode holds: the longest span a frozen view pans back through
// (protocol::kFrozenHoldSeconds), and a margin of twice the largest delay a paced edge trails the
// newest audio by (protocol::kPacingDelayCapSeconds). The last frame shown ended at most one
// delay before the newest audio when it was drawn, and at most a block more arrived before the
// freeze, a block being no longer than the delay where pacing applies: so the frozen window of
// the longest span is the one last shown, never moved forward to what is held.
inline constexpr double kSecondsFreezeSeconds =
    protocol::kFrozenHoldSeconds + 2.0 * protocol::kPacingDelayCapSeconds;

// Per-instance history of the input signal. The audio thread is the only writer; any number of
// non-real-time readers copy ranges out of it. The writer never waits for them.
//
// Samples are addressed by a monotonic 64-bit index: the n-th sample ever written has index n.
// A read fails, rather than returning mixed data, if the writer overwrote any part of the range
// while it was being copied, or the capture was resized meanwhile.
//
// The capacity follows the sample rate (capacityFor), set where the host prepares processing:
// resize() allocates, never on the audio thread. A reader keeps the buffer it is copying from
// alive through a resize, so it cannot read freed memory; its read then fails and is retried.
class CaptureRing final : public SampleSource
{
public:
    // The highest sample rate hosts run at (sample rates from 44.1 kHz up to it are tested). The
    // capture is sized for rates up to it; above it, holds shrink to what the capture keeps.
    static constexpr double kHighestSampleRate = 384000.0;

    // The smallest capacity, at every rate up to 229 kHz: 2^20 samples per channel, 8 MB for two
    // channels, 21.8 s at 48 kHz. It holds a freeze in Seconds mode up to 229 kHz with room to
    // spare, and a freeze in Beats mode holds readableLength of it, so that a longer beat span
    // chosen while frozen has its audio: shrinking it at low rates would cut that short.
    static constexpr std::size_t kMinimumCapacity = std::size_t{1} << 20U;
    static constexpr int kMaxChannels = 2;

    // The most of a capture of `capacity` samples a freeze copies at once: all but an eighth, room
    // for the audio thread to write on while the copy runs without it failing. An eighth is at
    // least 0.57 s at any rate up to the highest, far longer than a copy takes.
    [[nodiscard]] static constexpr std::size_t readableLength(std::size_t capacity) noexcept
    {
        return capacity - capacity / 8;
    }

    // The capacity for `sampleRate`: the smallest power of two (for the index mask), from
    // kMinimumCapacity, whose readableLength holds a freeze in Seconds mode
    // (kSecondsFreezeSeconds: the longest span and its margin), so a frame of the longest span can
    // be read live and frozen, and a frozen view panned through all of its hold. 2^20 up to
    // 215.9 kHz, 2^21 (16 MB) from there to the highest rate, at which it keeps 4.78 s: the
    // 4.25 s hold and 0.53 s of the 1 s margin another track's lane holds beyond it. A live frame
    // reads at most the 4 s span, and the trigger search two spans of up to 1 s. Above the highest
    // rate, no larger.
    [[nodiscard]] static constexpr std::size_t capacityFor(double sampleRate) noexcept
    {
        if (!(sampleRate > 0.0)) {
            return kMinimumCapacity; // Not a rate (zero, negative or NaN): the smallest.
        }
        const double rate = sampleRate < kHighestSampleRate ? sampleRate : kHighestSampleRate;
        std::size_t size = kMinimumCapacity;
        while (static_cast<double>(readableLength(size)) < kSecondsFreezeSeconds * rate) {
            size *= 2;
        }
        return size;
    }

    // The largest capacity, at the highest rate (checked below the class).
    static constexpr std::size_t kMaximumCapacity = std::size_t{1} << 21U;

    // Allocates the whole buffer. Not real-time safe. `capacity` must be a power of two.
    explicit CaptureRing(std::size_t capacity = kMinimumCapacity);

    // Reallocates the buffer for `capacity` samples per channel (a power of two), empty from the
    // newest index on, if it is not that size already. Not real-time safe, and only while the
    // audio thread does not write: the host's setupProcessing, which comes before processing.
    void resize(std::size_t capacity);

    // Audio thread. `channels` holds `numChannels` pointers (at most kMaxChannels) to
    // `numSamples` samples each; a null pointer writes silence for that channel.
    void write(const float* const* channels, int numChannels,
               int numSamples) noexcept ORIZON_NONBLOCKING;
    void write(const double* const* channels, int numChannels,
               int numSamples) noexcept ORIZON_NONBLOCKING;

    // Index one past the newest sample published by the writer. Any thread, the audio thread too.
    [[nodiscard]] std::uint64_t writeIndex() const noexcept ORIZON_NONBLOCKING;

    [[nodiscard]] std::uint64_t endIndex() const override { return writeIndex(); }
    [[nodiscard]] std::uint64_t beginIndex() const override;

    // Copies `destination.size()` samples of `channel`, starting at sample index `start`.
    // Returns false if any of them is not held, not yet written, or was overwritten (or the
    // capture resized) before the copy ended.
    [[nodiscard]] bool read(int channel, std::uint64_t start,
                            std::span<float> destination) const override;

    // Samples per channel the capture holds. Not for the audio thread.
    [[nodiscard]] std::size_t capacity() const;

private:
    // One allocation of the capture. Readers share it, so it outlives a resize while they copy.
    struct Storage
    {
        Storage(std::size_t size, std::uint64_t firstIndex);

        std::size_t capacity;
        std::size_t mask;
        std::uint64_t first; // The oldest index it can hold: the newest when it was allocated.
        std::vector<float> samples; // kMaxChannels blocks of `capacity` samples.
    };

    template <typename Sample>
    void writeSamples(const Sample* const* channels, int numChannels,
                      int numSamples) noexcept ORIZON_NONBLOCKING;

    [[nodiscard]] std::shared_ptr<const Storage> current() const;

    // Readers take the storage under this lock; the writer never does.
    mutable std::mutex storageMutex;
    std::shared_ptr<Storage> storage;
    // The writer's view of `storage`, changed only by resize() while the writer does not run.
    std::atomic<Storage*> writerStorage{nullptr};

    // Writer-only copy of the write index; avoids reading the atomic back on the audio thread.
    std::uint64_t writerIndex = 0;
    // Published before a block is written: samples below `pendingIndex - capacity` are
    // invalid from this point on.
    std::atomic<std::uint64_t> pendingIndex{0};
    // Published after a block is written: samples below this index are readable.
    std::atomic<std::uint64_t> publishedIndex{0};

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<Storage*>::is_always_lock_free);
    static_assert(std::atomic_ref<float>::is_always_lock_free);
};

static_assert(CaptureRing::capacityFor(CaptureRing::kHighestSampleRate) ==
              CaptureRing::kMaximumCapacity);
// Audio thread. Records one block of a plugin's input, whatever the plugin format: each channel
// whose bit is set in `silentChannels` is recorded as silence, whatever its buffer holds (hosts
// flag silence without clearing the buffer), and channels past kMaxChannels are left out.
template <typename Sample>
void captureBlock(CaptureRing& ring, const Sample* const* channels, int numChannels,
                  std::uint64_t silentChannels, int numSamples) noexcept ORIZON_NONBLOCKING
{
    std::array<const Sample*, CaptureRing::kMaxChannels> sources{};
    const int count = std::min(numChannels, CaptureRing::kMaxChannels);
    for (int channel = 0; channel < count; ++channel) {
        const bool silent =
            (silentChannels & (std::uint64_t{1} << static_cast<unsigned>(channel))) != 0;
        sources[static_cast<std::size_t>(channel)] = silent ? nullptr : channels[channel];
    }
    ring.write(sources.data(), count, numSamples);
}

static_assert(kSecondsFreezeSeconds * CaptureRing::kHighestSampleRate <=
                  static_cast<double>(CaptureRing::readableLength(CaptureRing::kMaximumCapacity)),
              "the capture must keep a whole freeze at the highest sample rate");

} // namespace orizon::engine
