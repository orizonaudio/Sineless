#pragma once

#include "engine/CaptureRing.h"
#include "engine/SampleSource.h"
#include "engine/Sweep.h"

#include <cstdint>
#include <span>
#include <vector>

namespace orizon::engine {

// The screen of a tempo-synced sweep, sample by sample, as on a sweeping scope: each position
// keeps what was drawn there until the playhead passes it again. Only samples that arrived since
// the last update are placed, where the musical clock puts them now, so a jump of the host
// transport (stop and play, a loop, a relocation) leaves earlier drawing in place. A sweep longer
// than what the source holds starts from what it holds and fills in as the playhead goes round.
//
// Holds the captured channels or, with one channel, their Mid: all a lane needs, at half the
// memory. Read through SampleSource in screen order: index 0 is the left edge. Used only by the
// analysis worker; it allocates when the sweep length grows.
class SweepImage final : public SampleSource
{
public:
    // `channels`: the captured channels (CaptureRing::kMaxChannels), or 1 for their Mid.
    explicit SweepImage(int channels = CaptureRing::kMaxChannels);

    // Places the samples of `window` from `source` that are not on screen yet, as far as the
    // source still holds them: all of them when the length changed (beyond what a tempo change
    // moves it, see sweepLengthsSimilar) or `rebuild` is set. The Mid of a one-channel image is
    // read from `inputChannels` captured channels. False if the source overwrote samples during
    // the copy; the image is then rebuilt on the next update.
    bool update(const SampleSource& source, const SweepWindow& window, bool rebuild,
                int inputChannels = CaptureRing::kMaxChannels);
    // Forgets the sweep and frees its memory.
    void clear();

    [[nodiscard]] std::uint64_t length() const { return count; }
    [[nodiscard]] int channels() const { return channelCount; }
    // Whether every position has been drawn since the image was (re)built: false while a sweep
    // longer than what the source held fills in over its first pass.
    [[nodiscard]] bool complete() const { return placedSinceBuild >= count; }

    // Screen positions [first, end) written since the last clearChanges(), so that only those
    // need reducing again. Everything after a rebuild.
    struct Range
    {
        std::uint64_t first = 0;
        std::uint64_t end = 0;
    };
    [[nodiscard]] const std::vector<Range>& changes() const { return changed; }
    void clearChanges() { changed.clear(); }

    [[nodiscard]] std::uint64_t endIndex() const override { return count; }
    [[nodiscard]] std::uint64_t beginIndex() const override { return 0; }
    [[nodiscard]] bool read(int channel, std::uint64_t start,
                            std::span<float> destination) const override;

private:
    int channelCount;
    std::vector<float> samples; // `channelCount` blocks of `count` samples.
    std::uint64_t count = 0;
    std::uint64_t placedEnd = 0;        // One past the newest source sample placed.
    std::uint64_t placedSinceBuild = 0; // Samples placed since the image was (re)built.
    std::vector<Range> changed;
    std::vector<float> scratch;     // One chunk of samples read from the source.
    std::vector<float> viewScratch; // The second channel, for the Mid.

    void markChanged(std::uint64_t first, std::uint64_t end);
};

} // namespace orizon::engine
