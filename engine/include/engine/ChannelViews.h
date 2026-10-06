#pragma once

#include "engine/SampleSource.h"
#include "protocol/Protocol.h"

#include <cstdint>
#include <span>

namespace orizon::engine {

// Channel views (FR-5), derived from the captured L/R channels: L, R, Mid = (L + R) / 2 and
// Side = (L - R) / 2, so every view shares the full-scale range. With mono input there is one
// channel, and every view is that channel.

// Bit mask of the views to draw: the requested set for stereo, the single channel for mono.
[[nodiscard]] std::uint8_t effectiveViews(std::uint8_t requested, int inputChannels);

// The view the trigger reads: the requested source for stereo, the single channel for mono.
[[nodiscard]] protocol::ChannelView effectiveTriggerSource(protocol::ChannelView requested,
                                                           int inputChannels);

// Reads `view` over [start, start + destination.size()). `scratch` must be at least as large as
// `destination`; it holds the second channel for Mid and Side.
[[nodiscard]] bool readView(const SampleSource& source, int inputChannels,
                            protocol::ChannelView view, std::uint64_t start,
                            std::span<float> destination, std::span<float> scratch);

} // namespace orizon::engine
