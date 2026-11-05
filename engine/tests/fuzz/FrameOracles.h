#pragma once

// What every frame a fuzz target draws must satisfy, whatever led to it. Each check stops the run
// through require(), so that libFuzzer keeps the input.

#include "FuzzCheck.h"
#include "protocol/Protocol.h"

#include <bit>
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

} // namespace orizon::fuzz
