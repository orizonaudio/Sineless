#include "engine/BlockStats.h"

#include <algorithm>
#include <cmath>

namespace orizon::engine {

namespace {

// The most samples read at once when measuring again: bounds the working space.
constexpr std::size_t kChunkSamples = 16384;

} // namespace

BlockStats::Block measureBlock(std::span<const float> samples)
{
    BlockStats::Block block;
    for (const float sample : samples) {
        if (!std::isfinite(sample)) {
            ++block.invalid;
            continue;
        }
        block.peak = std::max(block.peak, std::abs(sample));
        block.sumSquares += static_cast<double>(sample) * sample;
    }
    return block;
}

void BlockStats::reset(std::size_t count, std::size_t blockSamples)
{
    spanCount = count;
    blockLength = std::max<std::size_t>(blockSamples, 1);
    values.assign((count + blockLength - 1) / blockLength, Block{});
    valid = true;
}

bool BlockStats::update(std::size_t first, std::size_t end, const Reader& read)
{
    end = std::min(end, spanCount);
    if (!valid || first >= end) {
        return valid;
    }
    const std::size_t lastBlock = (end - 1) / blockLength;
    // In stretches of whole blocks, so the working space stays small however long the span.
    const std::size_t perStretch = std::max<std::size_t>(1, kChunkSamples / blockLength);
    for (std::size_t block = first / blockLength; block <= lastBlock;) {
        const std::size_t stop = std::min(lastBlock + 1, block + perStretch);
        const std::size_t from = block * blockLength;
        const std::size_t to = std::min(stop * blockLength, spanCount);
        scratch.resize(to - from);
        if (!read(from, scratch)) {
            valid = false;
            return false;
        }
        for (; block < stop; ++block) {
            const std::size_t begin = block * blockLength - from;
            const std::size_t length = std::min(blockLength, to - from - begin);
            values[block] = measureBlock(std::span<const float>(scratch).subspan(begin, length));
        }
    }
    return true;
}

float BlockStats::peak() const
{
    float peak = 0.0f;
    for (const Block& block : values) {
        peak = std::max(peak, block.peak);
    }
    return peak;
}

double BlockStats::sumSquares() const
{
    double total = 0.0;
    for (const Block& block : values) {
        total += block.sumSquares;
    }
    return total;
}

} // namespace orizon::engine
