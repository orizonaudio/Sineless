#pragma once

#include "engine/SampleSource.h"
#include "engine/Sweep.h"

#include <cstdint>
#include <span>
#include <vector>

namespace orizon::engine {

// The screen of a tempo-synced sweep, sample by sample, as on a sweeping scope: each position
// keeps what was drawn there until the playhead passes it again. Only samples that arrived since
// the last update are placed, where the musical clock puts them now, so a jump of the host
// transport (stop and play, a loop, a relocation) leaves earlier drawing in place.
//
// Read through SampleSource in screen order: index 0 is the left edge. Used only by the
// analysis worker; it allocates when the sweep length grows.
class SweepImage final : public SampleSource
{
public:
    // Places the samples of `window` from `source` that are not on screen yet: all of them when
    // the length changed or `rebuild` is set. False if the source no longer holds them.
    bool update(const SampleSource& source, const SweepWindow& window, bool rebuild);

    [[nodiscard]] std::uint64_t length() const { return count; }

    [[nodiscard]] std::uint64_t endIndex() const override { return count; }
    [[nodiscard]] std::uint64_t beginIndex() const override { return 0; }
    [[nodiscard]] bool read(int channel, std::uint64_t start,
                            std::span<float> destination) const override;

private:
    std::vector<float> samples; // CaptureRing::kMaxChannels blocks of `count` samples.
    std::uint64_t count = 0;
    std::uint64_t placedEnd = 0; // One past the newest source sample placed.
};

} // namespace orizon::engine
