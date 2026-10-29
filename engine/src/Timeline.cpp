#include "engine/Timeline.h"

#include <algorithm>
#include <cmath>

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
        stamp.tempoKnown = true;
    }
    if (host.timeSignatureValid && host.numerator > 0 && host.denominator > 0) {
        stamp.numerator = host.numerator;
        stamp.denominator = host.denominator;
    }
    stamp.playing = host.playing && host.positionValid;
    if (stamp.playing) {
        stamp.quarterNotes = host.quarterNotes;
        stamp.seconds = host.secondsValid ? host.seconds : stamp.seconds;
    } else if (started) {
        // Free-run: continue from the previous block at the previous tempo.
        const auto elapsed = static_cast<double>(nextSampleIndex - current.sampleIndex);
        stamp.quarterNotes =
            current.quarterNotes + elapsed * current.tempo / (kSecondsPerMinute * sampleRate);
        stamp.seconds = current.seconds + elapsed / sampleRate;
    } else {
        stamp.quarterNotes = host.positionValid ? host.quarterNotes : 0.0;
        stamp.seconds = host.secondsValid ? host.seconds : 0.0;
    }
    anchored = anchored || stamp.playing;
    stamp.anchored = anchored;
    current = stamp;
    started = true;
    nextSampleIndex += static_cast<std::uint64_t>(numSamples);
    publish(stamp);
    if (sampleRate != recordedRate) {
        // Records of another rate would extrapolate at the wrong one.
        firstKept.store(recorded.load(std::memory_order_relaxed), std::memory_order_release);
        recordedRate = sampleRate;
        record(stamp);
    } else if (!continuesRecord(stamp, sampleRate)) {
        record(stamp);
    }
}

// Whether `stamp` places its samples where the last record does, to within a sample: the same
// metre and anchoring, a tempo within kSweepLengthTolerance, and its position within one sample
// of where the record leads. Written to be false for a position that is not a number.
bool Timeline::continuesRecord(const MusicalStamp& stamp,
                               double sampleRate) const noexcept ORIZON_NONBLOCKING
{
    const double samplesOff = sampleIndexAt(lastRecorded, stamp.quarterNotes, sampleRate) -
                              static_cast<double>(stamp.sampleIndex);
    return stamp.numerator == lastRecorded.numerator &&
           stamp.denominator == lastRecorded.denominator &&
           stamp.anchored == lastRecorded.anchored &&
           std::abs(stamp.tempo - lastRecorded.tempo) <=
               std::max(stamp.tempo, lastRecorded.tempo) * kSweepLengthTolerance &&
           std::abs(samplesOff) <= 1.0;
}

void Timeline::record(const MusicalStamp& stamp) noexcept ORIZON_NONBLOCKING
{
    const std::uint64_t sequence = recorded.load(std::memory_order_relaxed);
    Record& slot = records[sequence % kPositionChanges];
    const std::uint64_t start = slot.version.load(std::memory_order_relaxed);
    slot.version.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.sequence.store(sequence, std::memory_order_relaxed);
    slot.sampleIndex.store(stamp.sampleIndex, std::memory_order_relaxed);
    slot.quarterNotes.store(stamp.quarterNotes, std::memory_order_relaxed);
    slot.seconds.store(stamp.seconds, std::memory_order_relaxed);
    slot.tempo.store(stamp.tempo, std::memory_order_relaxed);
    slot.tempoKnown.store(stamp.tempoKnown, std::memory_order_relaxed);
    slot.numerator.store(stamp.numerator, std::memory_order_relaxed);
    slot.denominator.store(stamp.denominator, std::memory_order_relaxed);
    slot.playing.store(stamp.playing, std::memory_order_relaxed);
    slot.anchored.store(stamp.anchored, std::memory_order_relaxed);
    slot.version.store(start + 2, std::memory_order_release);
    recorded.store(sequence + 1, std::memory_order_release);
    lastRecorded = stamp;
}

std::size_t Timeline::positionsSince(std::uint64_t from, std::span<MusicalStamp> out) const
{
    if (out.empty()) {
        return 0;
    }
    while (true) {
        const std::uint64_t end = recorded.load(std::memory_order_acquire);
        const std::uint64_t oldest =
            std::max(firstKept.load(std::memory_order_acquire),
                     end > kPositionChanges ? end - kPositionChanges : std::uint64_t{0});
        std::size_t count = 0;
        bool overwritten = false;
        // Newest first, until the one in effect at `from`.
        for (std::uint64_t sequence = end; sequence > oldest && count < out.size();) {
            --sequence;
            const Record& slot = records[sequence % kPositionChanges];
            const std::uint64_t before = slot.version.load(std::memory_order_acquire);
            MusicalStamp stamp;
            stamp.sampleIndex = slot.sampleIndex.load(std::memory_order_relaxed);
            stamp.quarterNotes = slot.quarterNotes.load(std::memory_order_relaxed);
            stamp.seconds = slot.seconds.load(std::memory_order_relaxed);
            stamp.tempo = slot.tempo.load(std::memory_order_relaxed);
            stamp.tempoKnown = slot.tempoKnown.load(std::memory_order_relaxed);
            stamp.numerator = slot.numerator.load(std::memory_order_relaxed);
            stamp.denominator = slot.denominator.load(std::memory_order_relaxed);
            stamp.playing = slot.playing.load(std::memory_order_relaxed);
            stamp.anchored = slot.anchored.load(std::memory_order_relaxed);
            const std::uint64_t held = slot.sequence.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((before & 1U) != 0 || slot.version.load(std::memory_order_relaxed) != before ||
                held != sequence) {
                overwritten = true; // Being written, or the writer lapped the ring: read again.
                break;
            }
            out[count++] = stamp;
            if (stamp.sampleIndex <= from) {
                break;
            }
        }
        if (overwritten) {
            continue;
        }
        std::reverse(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count));
        return count;
    }
}

void Timeline::publish(const MusicalStamp& stamp) noexcept ORIZON_NONBLOCKING
{
    const std::uint64_t start = version.load(std::memory_order_relaxed);
    version.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    sampleIndexValue.store(stamp.sampleIndex, std::memory_order_relaxed);
    quarterNotesValue.store(stamp.quarterNotes, std::memory_order_relaxed);
    secondsValue.store(stamp.seconds, std::memory_order_relaxed);
    tempoValue.store(stamp.tempo, std::memory_order_relaxed);
    tempoKnownValue.store(stamp.tempoKnown, std::memory_order_relaxed);
    numeratorValue.store(stamp.numerator, std::memory_order_relaxed);
    denominatorValue.store(stamp.denominator, std::memory_order_relaxed);
    playingValue.store(stamp.playing, std::memory_order_relaxed);
    anchoredValue.store(stamp.anchored, std::memory_order_relaxed);
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
        stamp.seconds = secondsValue.load(std::memory_order_relaxed);
        stamp.tempo = tempoValue.load(std::memory_order_relaxed);
        stamp.tempoKnown = tempoKnownValue.load(std::memory_order_relaxed);
        stamp.numerator = numeratorValue.load(std::memory_order_relaxed);
        stamp.denominator = denominatorValue.load(std::memory_order_relaxed);
        stamp.playing = playingValue.load(std::memory_order_relaxed);
        stamp.anchored = anchoredValue.load(std::memory_order_relaxed);
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

double Timeline::secondsAt(const MusicalStamp& stamp, double quarterNotes)
{
    return stamp.seconds + (quarterNotes - stamp.quarterNotes) * kSecondsPerMinute / stamp.tempo;
}

double Timeline::barQuarterNotes(const MusicalStamp& stamp)
{
    return stamp.numerator * 4.0 / stamp.denominator;
}

} // namespace orizon::engine
