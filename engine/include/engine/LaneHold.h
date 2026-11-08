#pragma once

#include "engine/Engine.h"
#include "engine/SampleSource.h"
#include "engine/Timeline.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orizon::engine {

// One multi-track lane's audio, held on freeze, with its clock, capture end and sample rate at
// that moment, so a sweep of another span can still be built from it: another track's lane
// channel alone, which halves what a hold of both would take, or every channel of this track's,
// which a channel chosen while frozen is drawn from. It is read with channels(): read with two, a
// held Side would come out silent.
//
// Used only by the analysis worker; it allocates on capture and frees on release.
class LaneHold final : public SampleSource
{
public:
    // Copies `view` of the newest `length` samples of `engine` (fewer if fewer exist), or with
    // `allChannels` every channel it captures. False if the writer outran the copy; nothing is
    // held then. `scratch` is working space.
    bool capture(const Engine& engine, protocol::ChannelView view, std::size_t length,
                 std::vector<float>& scratch, bool allChannels = false);
    // How many channels it holds: the track's, or the one view captured.
    [[nodiscard]] int channels() const { return channelCount; }
    // Whether reading `view` from it reads that view of the track: it holds every channel, or the
    // track's one, or that view.
    [[nodiscard]] bool shows(protocol::ChannelView view) const
    {
        return everyView || view == heldView;
    }
    void release();

    [[nodiscard]] bool held() const { return isHeld; }
    [[nodiscard]] const MusicalStamp& clock() const { return stamp; }
    // The rate the audio held was recorded at: the host may change the track's since.
    [[nodiscard]] double sampleRate() const { return rate; }

    [[nodiscard]] std::uint64_t endIndex() const override { return end; }
    [[nodiscard]] std::uint64_t beginIndex() const override { return begin; }
    [[nodiscard]] bool read(int channel, std::uint64_t start,
                            std::span<float> destination) const override;

private:
    std::vector<float> samples; // `channelCount` blocks of `end - begin` samples.
    int channelCount = 1;
    protocol::ChannelView heldView = protocol::ChannelView::kMid;
    bool everyView = false;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    MusicalStamp stamp;
    double rate = 0.0;
    bool isHeld = false;
};

} // namespace orizon::engine
