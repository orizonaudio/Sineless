#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace orizon::engine {

// Level statistics of a span whose samples change only in places, kept per block of
// `blockSamples` samples: each block's peak (largest absolute value) and sum of squares, of its
// finite samples, and how many are not finite (NaN or infinite, from a broken signal upstream).
// The Meter's levels over the part on screen (while it is off: measure), and the block energies
// the Compare view's summing gains need, then cost a pass over the blocks instead of over the
// samples, and after a change only the blocks holding changed samples are measured again.
class BlockStats
{
public:
    // Reads samples [first, first + destination.size()) of the span into `destination`.
    using Reader = std::function<bool(std::size_t first, std::span<float> destination)>;

    struct Block
    {
        float peak = 0.0f;
        double sumSquares = 0.0;
        std::uint32_t invalid = 0; // Samples not finite, left out of the peak and sum.
    };

    // Starts over for a span of `count` samples in blocks of `blockSamples` (at least 1); the
    // last block may be shorter. Every block reads as silence until measured.
    void reset(std::size_t count, std::size_t blockSamples);
    [[nodiscard]] bool fits(std::size_t count, std::size_t blockSamples) const
    {
        return valid && count == spanCount && blockSamples == blockLength;
    }

    // Measures again the blocks holding samples [first, end). False if `read` fails; the
    // statistics are then invalid until the next reset.
    bool update(std::size_t first, std::size_t end, const Reader& read);

    // The levels of samples [first, end), as measured: from the blocks inside them, and from the
    // samples of the blocks their edges fall inside, read again. None if the statistics are
    // invalid or a read fails.
    [[nodiscard]] std::optional<Block> measure(std::size_t first, std::size_t end,
                                               const Reader& read);

    [[nodiscard]] std::size_t sampleCount() const { return spanCount; }
    [[nodiscard]] std::size_t blockSamples() const { return blockLength; }
    [[nodiscard]] std::span<const Block> blocks() const { return values; }

    // Over the whole span, as the summing gains read them.
    [[nodiscard]] float peak() const;
    [[nodiscard]] double sumSquares() const;

private:
    std::size_t spanCount = 0;
    std::size_t blockLength = 1;
    bool valid = false;
    std::vector<Block> values;
    std::vector<float> scratch;
};

// The peak and sum of squares of the finite `samples`, and how many are not finite.
[[nodiscard]] BlockStats::Block measureBlock(std::span<const float> samples);

} // namespace orizon::engine
