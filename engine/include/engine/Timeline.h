#pragma once

#include "engine/RealtimeSafety.h"

#include <atomic>
#include <cstdint>

namespace orizon::engine {

// What the host reports for one block (the VST3 process context), as far as it is valid.
struct HostTimeline
{
    bool playing = false;
    bool positionValid = false;
    double quarterNotes = 0.0; // Project position at the block start.
    bool tempoValid = false;
    double tempo = 120.0; // Beats (quarter notes) per minute.
    bool timeSignatureValid = false;
    int numerator = 4;
    int denominator = 4;
};

// The musical position at one capture sample index.
struct MusicalStamp
{
    std::uint64_t sampleIndex = 0;
    double quarterNotes = 0.0;
    double tempo = 120.0;
    int numerator = 4;
    int denominator = 4;
    bool playing = false;
};

// The musical clock of an instance. The audio thread advances it once per block; any
// other thread reads the latest stamp without locks.
//
// While the host plays and reports a position, the clock follows it exactly. Otherwise it keeps
// running at the last known tempo, so tempo-synced views keep moving with the transport stopped;
// it snaps back to the host position when playback starts.
class Timeline
{
public:
    // Audio thread, after the block's samples were captured. `numSamples` must match what the
    // capture ring recorded for the block, so that sample indices agree.
    void advance(const HostTimeline& host, int numSamples,
                 double sampleRate) noexcept ORIZON_NONBLOCKING;

    // Any non-audio thread.
    [[nodiscard]] MusicalStamp latest() const;

    [[nodiscard]] static double quarterNotesAt(const MusicalStamp& stamp, std::uint64_t sampleIndex,
                                               double sampleRate);
    [[nodiscard]] static double sampleIndexAt(const MusicalStamp& stamp, double quarterNotes,
                                              double sampleRate);

    // Length of one bar in quarter notes for the stamp's time signature.
    [[nodiscard]] static double barQuarterNotes(const MusicalStamp& stamp);

private:
    void publish(const MusicalStamp& stamp) noexcept ORIZON_NONBLOCKING;

    // Writer-only state.
    MusicalStamp current;
    std::uint64_t nextSampleIndex = 0;
    bool started = false;

    // Seqlock: odd while the writer updates the fields.
    std::atomic<std::uint64_t> version{0};
    std::atomic<std::uint64_t> sampleIndexValue{0};
    std::atomic<double> quarterNotesValue{0.0};
    std::atomic<double> tempoValue{120.0};
    std::atomic<int> numeratorValue{4};
    std::atomic<int> denominatorValue{4};
    std::atomic<bool> playingValue{false};

    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

} // namespace orizon::engine
