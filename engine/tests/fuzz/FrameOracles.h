#pragma once

// What every frame a fuzz target draws must satisfy, whatever led to it. Each check stops the run
// through require(), so that libFuzzer keeps the input.

#include "FrameDecoding.h"
#include "FuzzCheck.h"
#include "protocol/Protocol.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace orizon::fuzz {

// The frame holds exactly the data its header announces (frame.ts).
inline void checkFrame(const std::vector<std::byte>& frame)
{
    using protocol::FrameHeader;
    require(frame.size() >= FrameHeader::kSize, "a frame holds its header");
    const auto byte = [&](std::size_t offset) { return std::to_integer<unsigned>(frame[offset]); };
    std::uint32_t points = 0;
    std::memcpy(&points, frame.data() + FrameHeader::kPointCountOffset, sizeof(points));
    const auto mode = static_cast<protocol::FrameMode>(byte(FrameHeader::kModeOffset));
    const std::size_t perBlock =
        std::size_t{points} * (mode == protocol::FrameMode::kMinMax ? 2 : 1);
    const auto views = static_cast<std::size_t>(std::popcount(byte(FrameHeader::kViewsOffset)));
    const std::size_t lanes = byte(FrameHeader::kLaneCountOffset);
    const bool compared = (byte(FrameHeader::kFlagsOffset) &
                           static_cast<unsigned>(protocol::FrameFlag::kCompared)) != 0;
    const std::size_t matches = compared && lanes > 0 ? (lanes - 1) * protocol::kMatchValues : 0;
    require(frame.size() == FrameHeader::kSize + ((views + lanes) * perBlock + matches) * 4,
            "a frame's size matches its header");
}

// Peak and RMS are both NaN (nothing measured), or both finite with 0 <= RMS <= peak: the root of
// a mean of squares is never above the largest of them, up to the rounding of a float.
inline void checkReadouts(const engine::DecodedFrame& frame)
{
    if (std::isnan(frame.peak) || std::isnan(frame.rms)) {
        require(std::isnan(frame.peak) && std::isnan(frame.rms),
                "peak and RMS are measured together or not at all");
        counted("readouts-unmeasured");
    } else {
        require(std::isfinite(frame.peak) && std::isfinite(frame.rms) && frame.rms >= 0.0f &&
                    frame.rms <= frame.peak * (1.0f + 0x1p-20f),
                "the RMS is between 0 and the peak");
    }
    counted("readouts");
}

// A Multi frame's two lanes, of tracks that played the same audio with one negated, mirror each
// other: point by point, one lane's (min, max) is the other's (-max, -min), or each raw value
// negated. Negation rounds nothing, so the comparison is exact. The caller checks only live frames
// in Trigger and Free, whose lanes hold no non-finite sample, while the tracks were treated alike.
inline void checkMirroredLanes(const engine::DecodedFrame& frame)
{
    const bool minMax = frame.mode == protocol::FrameMode::kMinMax;
    const std::size_t perLane = std::size_t{frame.pointCount} * (minMax ? 2 : 1);
    require(frame.laneCount == 2 && frame.data.size() >= 2 * perLane, "a lane per track");
    const float* first = frame.data.data();
    const float* second = first + perLane;
    bool sounding = false;
    for (std::size_t i = 0; i < perLane; ++i) {
        const std::size_t mirrored = minMax ? (i % 2 == 0 ? i + 1 : i - 1) : i;
        require(second[i] == -first[mirrored], "a track's lane mirrors its negated copy's");
        sounding = sounding || first[i] != 0.0f;
    }
    // Silent lanes mirror each other whatever the product does: only sounding ones count.
    if (sounding) {
        counted("polarity");
    }
}

} // namespace orizon::fuzz
