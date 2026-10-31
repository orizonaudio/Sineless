#pragma once

#include "engine/RealtimeSafety.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

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
    bool secondsValid = false;
    double seconds = 0.0; // Project time at the block start.
};

// Two sweep lengths, or the tempi that set them, closer than this fraction of the larger: no
// screen column resolves the difference, so a sweep keeps its length (FR-9.4), and a tempo change
// that small is no new position (Timeline's records).
inline constexpr double kSweepLengthTolerance = 1.0 / 1024.0;

// The musical position at one capture sample index.
struct MusicalStamp
{
    std::uint64_t sampleIndex = 0;
    double quarterNotes = 0.0;
    double seconds = 0.0; // Project time, in seconds.
    double tempo = 120.0;
    // Whether the host has reported a tempo yet; until it has, `tempo` is only a default.
    bool tempoKnown = false;
    int numerator = 4;
    int denominator = 4;
    bool playing = false;
    // Whether the instance has played a block with a host position, and so runs on from a
    // position every instance shares, also after a stop (AD-15). A position reported while
    // stopped, taken on an instance's first block, does not anchor it.
    bool anchored = false;
};

// The musical clock of an instance. The audio thread advances it once per block; any
// other thread reads the latest stamp without locks.
//
// While the host plays and reports a position, the clock follows it exactly. Otherwise it keeps
// running at the last known tempo, so tempo-synced views keep moving with the transport stopped;
// it snaps back to the host position when playback starts.
//
// It also records each position that does not continue the last one recorded (a transport jump,
// a tempo or metre change, the first block, the block that anchors it), in a ring of the newest
// kPositionChanges, so that audio placed after it arrived, a delay behind the newest, is placed
// by the position it was played at (FR-9.2, AD-13).
class Timeline
{
public:
    static constexpr std::size_t kPositionChanges = 256;

    // Audio thread, after the block's samples were captured. `numSamples` must match what the
    // capture ring recorded for the block, so that sample indices agree.
    void advance(const HostTimeline& host, int numSamples,
                 double sampleRate) noexcept ORIZON_NONBLOCKING;

    // Any non-audio thread.
    [[nodiscard]] MusicalStamp latest() const;
    // The recorded positions that place samples from `from` on, oldest first: the one in effect at
    // `from` (the oldest kept, if none is that old), then each recorded after it. Writes at most
    // out.size(), the newest if more; returns how many. None before the first block.
    [[nodiscard]] std::size_t positionsSince(std::uint64_t from, std::span<MusicalStamp> out) const;

    [[nodiscard]] static double quarterNotesAt(const MusicalStamp& stamp, std::uint64_t sampleIndex,
                                               double sampleRate);
    [[nodiscard]] static double sampleIndexAt(const MusicalStamp& stamp, double quarterNotes,
                                              double sampleRate);
    // The project time at `quarterNotes`, at the stamp's tempo.
    [[nodiscard]] static double secondsAt(const MusicalStamp& stamp, double quarterNotes);

    // Length of one bar in quarter notes for the stamp's time signature.
    [[nodiscard]] static double barQuarterNotes(const MusicalStamp& stamp);

private:
    void publish(const MusicalStamp& stamp) noexcept ORIZON_NONBLOCKING;
    [[nodiscard]] bool continuesRecord(const MusicalStamp& stamp,
                                       double sampleRate) const noexcept ORIZON_NONBLOCKING;
    void record(const MusicalStamp& stamp) noexcept ORIZON_NONBLOCKING;

    // Writer-only state.
    MusicalStamp current;
    std::uint64_t nextSampleIndex = 0;
    bool started = false;
    bool anchored = false;
    MusicalStamp lastRecorded;
    double recordedRate = 0.0; // The rate of the records kept; 0 before the first.

    // One recorded position, behind a seqlock of its own; `sequence` says which record it holds.
    struct Record
    {
        std::atomic<std::uint64_t> version{0};
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::uint64_t> sampleIndex{0};
        std::atomic<double> quarterNotes{0.0};
        std::atomic<double> seconds{0.0};
        std::atomic<double> tempo{120.0};
        std::atomic<bool> tempoKnown{false};
        std::atomic<int> numerator{4};
        std::atomic<int> denominator{4};
        std::atomic<bool> playing{false};
        std::atomic<bool> anchored{false};
    };
    std::array<Record, kPositionChanges> records;
    std::atomic<std::uint64_t> recorded{0};  // Records written, ever; record n is in n % size.
    std::atomic<std::uint64_t> firstKept{0}; // Records before it are of another sample rate.

    // Seqlock: odd while the writer updates the fields.
    std::atomic<std::uint64_t> version{0};
    std::atomic<std::uint64_t> sampleIndexValue{0};
    std::atomic<double> quarterNotesValue{0.0};
    std::atomic<double> secondsValue{0.0};
    std::atomic<double> tempoValue{120.0};
    std::atomic<bool> tempoKnownValue{false};
    std::atomic<int> numeratorValue{4};
    std::atomic<int> denominatorValue{4};
    std::atomic<bool> playingValue{false};
    std::atomic<bool> anchoredValue{false};

    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

} // namespace orizon::engine
