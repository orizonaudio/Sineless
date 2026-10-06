#include "engine/Timeline.h"

namespace orizon::engine {

namespace {

constexpr double kSecondsPerMinute = 60.0;

} // namespace

void Timeline::advance(const HostTimeline& host, int numSamples,
                       double sampleRate) noexcept ORIZON_NONBLOCKING
{
    if (numSamples <= 0 || sampleRate <= 0.0) {
        return;
    }
    MusicalStamp stamp = current;
    stamp.sampleIndex = nextSampleIndex;
    if (host.tempoValid && host.tempo > 0.0) {
        stamp.tempo = host.tempo;
    }
    if (host.timeSignatureValid && host.numerator > 0 && host.denominator > 0) {
        stamp.numerator = host.numerator;
        stamp.denominator = host.denominator;
    }
    stamp.playing = host.playing && host.positionValid;
    if (stamp.playing) {
        stamp.quarterNotes = host.quarterNotes;
    } else if (started) {
        // Free-run: continue from the previous block at the previous tempo.
        stamp.quarterNotes =
            current.quarterNotes + static_cast<double>(nextSampleIndex - current.sampleIndex) *
                                       current.tempo / (kSecondsPerMinute * sampleRate);
    } else {
        stamp.quarterNotes = host.positionValid ? host.quarterNotes : 0.0;
    }
    current = stamp;
    started = true;
    nextSampleIndex += static_cast<std::uint64_t>(numSamples);
    publish(stamp);
}

void Timeline::publish(const MusicalStamp& stamp) noexcept ORIZON_NONBLOCKING
{
    const std::uint64_t start = version.load(std::memory_order_relaxed);
    version.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    sampleIndexValue.store(stamp.sampleIndex, std::memory_order_relaxed);
    quarterNotesValue.store(stamp.quarterNotes, std::memory_order_relaxed);
    tempoValue.store(stamp.tempo, std::memory_order_relaxed);
    numeratorValue.store(stamp.numerator, std::memory_order_relaxed);
    denominatorValue.store(stamp.denominator, std::memory_order_relaxed);
    playingValue.store(stamp.playing, std::memory_order_relaxed);
    version.store(start + 2, std::memory_order_release);
}

MusicalStamp Timeline::latest() const
{
    MusicalStamp stamp;
    while (true) {
        const std::uint64_t before = version.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue; // A write is in progress.
        }
        stamp.sampleIndex = sampleIndexValue.load(std::memory_order_relaxed);
        stamp.quarterNotes = quarterNotesValue.load(std::memory_order_relaxed);
        stamp.tempo = tempoValue.load(std::memory_order_relaxed);
        stamp.numerator = numeratorValue.load(std::memory_order_relaxed);
        stamp.denominator = denominatorValue.load(std::memory_order_relaxed);
        stamp.playing = playingValue.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (version.load(std::memory_order_relaxed) == before) {
            return stamp;
        }
    }
}

double Timeline::quarterNotesAt(const MusicalStamp& stamp, std::uint64_t sampleIndex,
                                double sampleRate)
{
    const double samples =
        static_cast<double>(sampleIndex) - static_cast<double>(stamp.sampleIndex);
    return stamp.quarterNotes + samples * stamp.tempo / (kSecondsPerMinute * sampleRate);
}

double Timeline::sampleIndexAt(const MusicalStamp& stamp, double quarterNotes, double sampleRate)
{
    return static_cast<double>(stamp.sampleIndex) +
           (quarterNotes - stamp.quarterNotes) * kSecondsPerMinute * sampleRate / stamp.tempo;
}

double Timeline::barQuarterNotes(const MusicalStamp& stamp)
{
    return stamp.numerator * 4.0 / stamp.denominator;
}

} // namespace orizon::engine
