#pragma once

#include "engine/Engine.h"
#include "engine/SampleSource.h"
#include "engine/Timeline.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orizon::engine {

// One multi-track lane's audio, held on freeze: the track's Mid, with its clock and capture end
// at that moment, so a sweep of another span can still be built from it. Mono halves what a hold
// of both channels would take. Reads return the Mid for either channel, so a sweep image built
// from it holds the Mid.
//
// Used only by the analysis worker; it allocates on capture and frees on release.
class LaneHold final : public SampleSource
{
public:
    // Copies the Mid of the newest `length` samples of `engine` (fewer if fewer exist). False if
    // the writer outran the copy; nothing is held then. `scratch` is working space.
    bool capture(const Engine& engine, std::size_t length, std::vector<float>& scratch);
    void release();

    [[nodiscard]] bool held() const { return isHeld; }
    [[nodiscard]] const MusicalStamp& clock() const { return stamp; }

    [[nodiscard]] std::uint64_t endIndex() const override { return end; }
    [[nodiscard]] std::uint64_t beginIndex() const override { return begin; }
    [[nodiscard]] bool read(int channel, std::uint64_t start,
                            std::span<float> destination) const override;

private:
    std::vector<float> mid;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    MusicalStamp stamp;
    bool isHeld = false;
};

} // namespace orizon::engine
