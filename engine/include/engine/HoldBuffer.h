#pragma once

#include "engine/CaptureRing.h"
#include "engine/SampleSource.h"
#include "protocol/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orizon::engine {

// Copy of the newest samples taken on freeze. Analysis reads it through SampleSource
// exactly like the live ring, so scale, views, panning and readouts keep working while frozen.
// Allocated when the editor opens; used only by the analysis worker.
class HoldBuffer final : public SampleSource
{
public:
    // The audio held on freeze in Seconds mode: the 4 s maximum visible span and its margin for the
    // paced edge (kSecondsFreezeSeconds).
    static constexpr double kHoldSeconds = kSecondsFreezeSeconds;

    // Holds up to `capacity` samples per channel: the capture's, which follows the sample rate.
    explicit HoldBuffer(std::size_t capacity = CaptureRing::kMinimumCapacity);

    // Reallocates for `size` samples per channel, empty, if it holds a different number: the
    // capture was resized for another sample rate. Analysis worker only.
    void resize(std::size_t size);

    // Copies the newest `length` samples of every channel (fewer if fewer exist). If the source
    // overwrote them during the copy, the buffer is left empty and false is returned; the
    // caller retries on its next step.
    bool capture(const SampleSource& source, std::size_t length);

    // Samples to hold at `sampleRate`: kHoldSeconds, limited to the capacity.
    [[nodiscard]] std::size_t holdLength(double sampleRate) const;

    // The capture's capacity when it was last held: it may have changed since.
    [[nodiscard]] std::size_t heldCapacity() const { return capacity; }

    [[nodiscard]] std::uint64_t endIndex() const override { return end; }
    [[nodiscard]] std::uint64_t beginIndex() const override { return begin; }
    [[nodiscard]] bool read(int channel, std::uint64_t start,
                            std::span<float> destination) const override;

private:
    std::size_t capacity;
    std::vector<float> samples; // CaptureRing::kMaxChannels blocks of `capacity`.
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

} // namespace orizon::engine
