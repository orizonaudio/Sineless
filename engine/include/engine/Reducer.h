#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <span>
#include <vector>

namespace orizon::engine {

// Reduces the visible span of one channel to the UI's device-pixel resolution (AD-2, FR-2).
//
// With more samples than columns, column c covers samples [c * n / columns, (c + 1) * n / columns)
// and contributes a (min, max) pair, so every sample lands in exactly one column and no peak is
// lost. With at most one sample per column the samples are passed through unchanged.
// Values stay in signal units; all mapping to pixels happens in the UI.
//
// Returns the mode used. `output` is resized but keeps its capacity across calls.
protocol::FrameMode reduceToColumns(std::span<const float> samples, std::size_t columns,
                                    std::vector<float>& output);

} // namespace orizon::engine
