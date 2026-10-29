#pragma once

// A host for tests: instances of group A on one host clock at 120 BPM in 4/4, playing
// deterministic signals in blocks of 10 ms, the first one viewed by an analyzer.

#include "FrameDecoding.h"
#include "Signals.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"
#include "engine/InstanceRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace orizon::engine {

class TestHost
{
public:
    // `tempoKnown` false: a host that reports no tempo, as one that has not played the project.
    TestHost(std::uint32_t sampleRate, const std::vector<Track>& tracks, bool tempoKnown = true)
        : rate(sampleRate), tempoKnown(tempoKnown)
    {
        for (const Track& track : tracks) {
            add(track);
        }
    }

    [[nodiscard]] Engine& own() const { return *members.front().engine; }
    [[nodiscard]] Engine& track(std::size_t index) const { return *members.at(index).engine; }

    // Plays `count` samples, in blocks of 10 ms, producing a frame after each as a page asks.
    void play(std::uint64_t count, Analyzer& analyzer, std::optional<DecodedFrame>& frame,
              std::vector<std::byte>& bytes)
    {
        const std::uint64_t block = rate / 100;
        std::vector<float> left(block);
        std::vector<float> right(block);
        for (std::uint64_t done = 0; done < count; done += block) {
            for (const auto& [engine, track] : members) {
                for (std::uint64_t i = 0; i < block; ++i) {
                    left[i] = track.left(sample + i, rate);
                    right[i] = track.right ? track.right(sample + i, rate) : left[i];
                }
                const std::array<const float*, 2> channels{left.data(), right.data()};
                engine->captureRing().write(channels.data(), track.channels,
                                            static_cast<int>(block));
                // The project's time in seconds too, as hostTimeline in plugin/src/Processor.cpp
                // reports it.
                engine->timeline().advance({.playing = true,
                                            .positionValid = true,
                                            .quarterNotes = quarterNotes,
                                            .tempoValid = tempoKnown,
                                            .tempo = kTempo,
                                            .secondsValid = true,
                                            .seconds = seconds},
                                           static_cast<int>(block), rate);
            }
            sample += block;
            quarterNotes += static_cast<double>(block) * kTempo / (60.0 * rate);
            seconds += static_cast<double>(block) / rate;
            std::vector<std::byte> produced;
            if (analyzer.produce(produced)) {
                bytes = std::move(produced);
                frame = decode(bytes);
            }
        }
    }

    // The host changes the sample rate while the instances keep processing, as
    // Processor::setupProcessing reports it: the captures keep the size they were prepared at.
    void setSampleRate(std::uint32_t sampleRate)
    {
        rate = sampleRate;
        for (const auto& member : members) {
            member.engine->setSampleRate(sampleRate);
        }
    }

    InstanceRegistry registry;

private:
    static constexpr double kTempo = 120.0;

    struct Member
    {
        std::shared_ptr<Engine> engine;
        Track track;
    };

    void add(const Track& track)
    {
        auto engine = std::make_shared<Engine>();
        engine->prepare(rate);
        engine->setInputChannels(track.channels);
        registry.add(engine);
        members.push_back({.engine = std::move(engine), .track = track});
    }

    std::uint32_t rate;
    bool tempoKnown;
    std::vector<Member> members;
    std::uint64_t sample = 0;
    double quarterNotes = 0.0;
    double seconds = 0.0;
};

} // namespace orizon::engine
