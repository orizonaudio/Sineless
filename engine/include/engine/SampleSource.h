#pragma once

#include <cstdint>
#include <span>

namespace orizon::engine {

// Read access to captured samples by absolute sample index: the live capture ring, or the hold
// buffer while frozen (FR-6). Analysis reads through this interface and works on either.
class SampleSource
{
public:
    SampleSource() = default;
    virtual ~SampleSource() = default;
    SampleSource(const SampleSource&) = delete;
    SampleSource& operator=(const SampleSource&) = delete;

    // One past the newest sample.
    [[nodiscard]] virtual std::uint64_t endIndex() const = 0;
    // The oldest sample that may still be read.
    [[nodiscard]] virtual std::uint64_t beginIndex() const = 0;
    // Copies `destination.size()` samples of `channel` from `start`; false if not all available.
    [[nodiscard]] virtual bool read(int channel, std::uint64_t start,
                                    std::span<float> destination) const = 0;
};

} // namespace orizon::engine
