#include "engine/Analyzer.h"

#include "engine/ChannelViews.h"
#include "engine/FrameEncoder.h"
#include "engine/FreeRunning.h"
#include "engine/Reconstruction.h"
#include "engine/Reducer.h"
#include "engine/Sweep.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <utility>

namespace orizon::engine {

using protocol::ChannelView;
using protocol::FrameFlag;
using protocol::SettingId;

namespace {

// In Trigger or Free a freeze holds the longest span, which the view can be panned back through.
// The lanes of other tracks hold up to this much more: their captures can run a little ahead of or
// behind this one's, and each must still cover the whole span with the view panned as far back as
// this track's hold allows. What the capture keeps cuts it short above 174.8 kHz with the smallest
// capture, and above 349.5 kHz with the largest: to 0.53 s at 192 and 384 kHz (see
// CaptureRing::capacityFor).
constexpr double kLaneMarginSeconds = 1.0;
// Align reads a compared track up to its reach from where this track's audio is, beside the late
// block the margin is there for (the paced edge stands up to twice pacingDelayCapSeconds behind
// the newest audio): both fit in it. At 384 kHz, where the capture leaves 0.53 s, they still do.
static_assert(protocol::kAlignReachSeconds + 2.0 * protocol::kPacingDelayCapSeconds <=
                  std::min(kLaneMarginSeconds, static_cast<double>(CaptureRing::readableLength(
                                                   CaptureRing::kMaximumCapacity)) /
                                                       CaptureRing::kHighestSampleRate -
                                                   HoldBuffer::kHoldSeconds),
              "an aligned read and a late block fit in what a lane's hold keeps beyond the span");

// Samples a freeze holds. In Beats mode, the newest audio the capture can spare (a margin for the
// audio thread, which writes on), so that a longer beat span chosen while frozen has its audio;
// in Trigger or Free, the longest span, and for another track's lane the margin above.
std::uint64_t frozenLength(const CaptureRing& ring, double sampleRate, bool synced,
                           bool lane = false)
{
    const std::uint64_t readable = CaptureRing::readableLength(ring.capacity());
    if (synced) {
        return readable;
    }
    // Never more than the capture can spare while the audio thread writes on. That always covers
    // the hold up to the highest sample rate (see CaptureRing::capacityFor); a lane's margin may
    // be cut short.
    const double seconds = HoldBuffer::kHoldSeconds + (lane ? kLaneMarginSeconds : 0.0);
    return std::min<std::uint64_t>(static_cast<std::uint64_t>(std::llround(seconds * sampleRate)),
                                   readable);
}

// The narrowest view accepted. The UI zooms to `kMinimumViewSamples` of the span it was shown;
// this only keeps a view's width well above rounding at the longest spans and highest rates.
constexpr double kMinimumViewWidth = 1e-12;

constexpr std::uint8_t flag(FrameFlag value)
{
    return static_cast<std::uint8_t>(value);
}

// The part of a window of `count` samples that the screen shows (Crop). The window starts
// `fraction` of a sample after its first sample. A window of no samples (a failed read, or a
// free-running span before its first column of audio) shows none. With a `margin`, up to that many
// more samples each side are read as neighbours for reconstructing the wave between the samples:
// within the window, or with `pastWindow` beyond its ends too, the block then starting `margin`
// samples before the window (Crop::offset), so its neighbours come from the audio around it.
Crop cropOf(double count, double fraction, double cropStart, double cropEnd, std::size_t margin = 0,
            bool pastWindow = false)
{
    if (count < 1.0) {
        return {};
    }
    const double visibleFrom = fraction + cropStart * count;
    const double visibleTo = fraction + cropEnd * count;
    const auto first = static_cast<std::size_t>(
        std::clamp(std::floor(visibleFrom), 0.0, std::max(count - 1.0, 0.0)));
    const auto end = static_cast<std::size_t>(
        std::clamp(std::ceil(visibleTo) + 1.0, static_cast<double>(first) + 1.0, count));
    const std::size_t offset = pastWindow ? margin : 0;
    const std::size_t lead = std::min(margin, first + offset);
    const std::size_t trail = std::min(margin, static_cast<std::size_t>(count) - end + offset);
    const std::size_t blockFirst = first + offset - lead;
    return {.first = blockFirst,
            .end = end + offset + trail,
            .startFraction =
                visibleFrom + static_cast<double>(offset) - static_cast<double>(blockFirst),
            .visibleSamples = (cropEnd - cropStart) * count,
            .lead = lead,
            .trail = trail,
            .offset = offset};
}

// Fills `block` with window positions from `position` (a crop's first, less its offset) of a
// window of `count` samples that starts at `windowStart` in `source`: the window's own positions
// through `readWindow(from, destination)` (from a window position), and the neighbours past its
// ends as `source` holds them, NaN where it does not. A source that `holdsNone` of this window (a
// lane frozen at another rate or channel, placed past its hold) has no neighbours either, though it
// holds audio there. `scratch` holds at least the block. What `readWindow` returns (true when the
// block holds none of the window).
template <typename ReadWindow>
bool fillBlock(const SampleSource& source, int channels, protocol::ChannelView view,
               std::int64_t windowStart, std::int64_t count, std::int64_t position,
               std::span<float> block, std::span<float> scratch, const ReadWindow& readWindow,
               bool holdsNone = false)
{
    const auto size = static_cast<std::int64_t>(block.size());
    const std::int64_t inFirst = std::clamp<std::int64_t>(-position, 0, size);
    const std::int64_t inEnd = std::clamp<std::int64_t>(count - position, inFirst, size);
    const auto readMargin = [&](std::int64_t from, std::span<float> margin) {
        if (holdsNone) {
            std::ranges::fill(margin, std::numeric_limits<float>::quiet_NaN());
        } else {
            readViewHeldOrNaN(source, channels, view, windowStart + from, margin, scratch);
        }
    };
    if (inFirst > 0) {
        readMargin(position, block.first(static_cast<std::size_t>(inFirst)));
    }
    if (inEnd < size) {
        readMargin(position + inEnd, block.subspan(static_cast<std::size_t>(inEnd)));
    }
    if (inFirst == inEnd) {
        return true;
    }
    return readWindow(position + inFirst, block.subspan(static_cast<std::size_t>(inFirst),
                                                        static_cast<std::size_t>(inEnd - inFirst)));
}

// The samples of a block read with `crop`'s margins that the screen shows.
std::span<const float> shownPart(std::span<const float> block, const Crop& crop)
{
    const std::size_t lead = std::min(crop.lead, block.size());
    return block.subspan(lead, block.size() - lead - std::min(crop.trail, block.size() - lead));
}

// What a view shows of a window (cropOf's arguments) across `columns`, and its points per sample.
// One number decides the frame's mode, the samples shown: raw while there are at most as many as
// columns, drawn as the wave between them (at least kMinPointsPerSample points a sample, never
// straight) and read with the raw view's reach of neighbours each side; min/max columns otherwise,
// read with the columns' reach (kColumnReach) for the wave around their extreme samples. The
// neighbours decide what is read, never the mode. With `pastWindow`, they are read past the
// window's ends too (cropOf).
struct ViewCrop
{
    Crop crop;
    std::size_t factor = 1;
};

ViewCrop viewCropOf(double count, double fraction, double cropStart, double cropEnd,
                    std::size_t columns, bool pastWindow = false)
{
    const Crop shown = cropOf(count, fraction, cropStart, cropEnd);
    if (shown.end - shown.first > columns) {
        return {.crop = cropOf(count, fraction, cropStart, cropEnd, kColumnReach, pastWindow),
                .factor = 1};
    }
    return {.crop = cropOf(count, fraction, cropStart, cropEnd, kReconstructionReach, pastWindow),
            .factor = Reconstruction::pointsPerSample(shown.visibleSamples, columns)};
}

// The image position of a block's first sample, read with `crop` from a sweep image: before the
// image's first position when its neighbours are read across the wrap (Crop::offset).
std::int64_t blockStart(const Crop& crop)
{
    return static_cast<std::int64_t>(crop.first) - static_cast<std::int64_t>(crop.offset);
}

// `position` taken round an image of `count` samples, into [0, count).
std::int64_t wrapped(std::int64_t position, std::int64_t count)
{
    const std::int64_t rest = position % count;
    return rest < 0 ? rest + count : rest;
}

// Reads image positions from `position` into `into` through `read(first, destination)`, which
// reads within the image, going round its wrap: a sweep's screen is written cyclically, the
// position after its last the next sample of the audio (the next pass's first) unless a join lies
// there, which the seams mark. False if a read fails or the image is empty.
template <typename Read>
bool readWrapped(const Read& read, std::uint64_t count, std::int64_t position,
                 std::span<float> into)
{
    if (count == 0) {
        return into.empty();
    }
    std::size_t done = 0;
    auto from = static_cast<std::uint64_t>(wrapped(position, static_cast<std::int64_t>(count)));
    while (done < into.size()) {
        const std::size_t length =
            std::min<std::size_t>(into.size() - done, static_cast<std::size_t>(count - from));
        if (!read(from, into.subspan(done, length))) {
            return false;
        }
        done += length;
        from = 0;
    }
    return true;
}

// Calls `apply(from, end)` with the block indices that image positions [first, end) of an image of
// `count` samples fall on, in a block of `size` read with `crop`, round the wrap. The positions
// may reach a little past the image's ends (a change widened by what it reaches).
template <typename Apply>
void forEachInBlock(std::int64_t first, std::int64_t end, const Crop& crop, std::size_t size,
                    std::uint64_t count, const Apply& apply)
{
    const auto length = static_cast<std::int64_t>(count);
    for (const std::int64_t turn : {-length, std::int64_t{0}, length}) {
        const std::int64_t from = std::max<std::int64_t>(first + turn - blockStart(crop), 0);
        const std::int64_t to =
            std::min<std::int64_t>(end + turn - blockStart(crop), static_cast<std::int64_t>(size));
        if (from < to) {
            apply(static_cast<std::size_t>(from), static_cast<std::size_t>(to));
        }
    }
}

// Adds to `seams` the block indices that image position `join` (the first after a join) falls on,
// in a block read with `crop` from an image of `count` samples, round the wrap: a join at a
// block's first sample has nothing before it to join.
void addSeam(std::int64_t join, const Crop& crop, std::uint64_t count,
             std::vector<std::size_t>& seams)
{
    if (count == 0) {
        return;
    }
    const auto length = static_cast<std::int64_t>(count);
    const auto size = static_cast<std::int64_t>(crop.end - crop.first);
    for (std::int64_t index = wrapped(join - blockStart(crop), length); index < size;
         index += length) {
        if (index > 0) {
            seams.push_back(static_cast<std::size_t>(index));
        }
    }
}

// Adds to `seams` where a sweep's playhead, at `position` (0 to 1) of an image of `count` samples,
// falls in a block read with `crop`: the first sample of the previous pass. None with no playhead
// (a negative position).
void addPlayhead(double position, std::uint64_t count, const Crop& crop,
                 std::vector<std::size_t>& seams)
{
    if (position >= 0.0) {
        addSeam(std::llround(position * static_cast<double>(count)), crop, count, seams);
    }
}

// Adds to `seams` an image's joins (SweepImage::joins) that lie in `crop`, as indices into it,
// round the wrap; and, while it shows a part (a frozen shorter span rebuilt from the held audio),
// that part's two edges, beside which the image's own samples are of another layout. For a lane
// drawn lined up, each lies `shift` samples before where the image has it.
void addJoins(const SweepImage& image, const Crop& crop, std::vector<std::size_t>& seams,
              std::ptrdiff_t shift = 0)
{
    for (const std::uint64_t join : image.joins()) {
        addSeam(static_cast<std::int64_t>(join) - shift, crop, image.length(), seams);
    }
    if (const auto part = image.shownPart()) {
        addSeam(static_cast<std::int64_t>(part->first) - shift, crop, image.length(), seams);
        addSeam(static_cast<std::int64_t>(part->end) - shift, crop, image.length(), seams);
    }
}

bool contains(const std::vector<std::uint64_t>& ids, std::uint64_t id)
{
    return std::ranges::find(ids, id) != ids.end();
}

// Where a window's time zero sits across its unpanned span, for the frame; NaN without one.
template <typename Window> float triggerPositionOf(const Window& window)
{
    return std::isfinite(window.zero)
               ? static_cast<float>((window.zero - window.placedStart) / window.span)
               : std::numeric_limits<float>::quiet_NaN();
}

TriggerParameters triggerParameters(const Settings& settings, double sampleRate)
{
    return {.level = settings.value(SettingId::kTriggerLevel),
            .hysteresis = settings.value(SettingId::kTriggerHysteresis),
            .slope = settings.enumValue<protocol::TriggerSlope>(SettingId::kTriggerSlope),
            .holdOffSamples = settings.value(SettingId::kTriggerHoldOff) * sampleRate,
            .position = settings.value(SettingId::kTriggerPosition),
            .followingSamples = static_cast<double>(kReconstructionReach)};
}

// Samples per block of levels: kStatsBlockSeconds of audio.
std::size_t statsBlock(double sampleRate)
{
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(kStatsBlockSeconds * sampleRate)));
}

// The samples of a block read with `crop` that the Meter measures: those whose positions lie in the
// visible part [visibleFrom, visibleTo), so that two views side by side count each sample once.
// Positions from the window's first sample, within [0, count).
std::pair<std::size_t, std::size_t> meterRange(const Crop& crop, std::size_t count)
{
    const double from =
        static_cast<double>(crop.first) - static_cast<double>(crop.offset) + crop.startFraction;
    const double to = from + crop.visibleSamples;
    const auto clamp = [count](double position) {
        return static_cast<std::size_t>(
            std::clamp(std::ceil(position), 0.0, static_cast<double>(count)));
    };
    const std::size_t first = clamp(from);
    return {first, std::max(first, clamp(to))};
}

// The seconds of audio before a window's visible part the Meter's filters settle on.
constexpr double kMeterSettleSeconds =
    static_cast<double>(MeterBlocks::kSettleBlocks) * kStatsBlockSeconds;

// The seconds of this track's audio the Meter's blocks keep in Trigger and Free: what a freeze
// there holds (HoldBuffer::kHoldSeconds). It is the same quantity, the audio a view can show or
// be panned back to: a live window ends at most a paced delay before the newest audio and is at
// most frozenHoldSeconds long, and a freeze then holds what the blocks already cover, so freezing
// measures nothing again. The capture holds it at every supported rate, as the hold relies on
// (CaptureRing::capacityFor); the blocks go by whole blocks, so they keep up to one more.
constexpr double kMeterKeepSeconds = HoldBuffer::kHoldSeconds;

// Goes on with a sum's pending recomputation (an Analyzer::SumRefresh): up to `step` positions of
// a span of `length`, from where it stopped, wrapping at the end.
template <typename Refresh, typename Update>
void continueRefresh(Refresh& refresh, std::size_t length, std::size_t step, const Update& update)
{
    if (length == 0) {
        refresh = {};
        return;
    }
    std::size_t budget = std::min(std::max<std::size_t>(step, 1), refresh.remaining);
    refresh.remaining -= budget;
    while (budget > 0) {
        refresh.cursor %= length;
        const std::size_t run = std::min(budget, length - refresh.cursor);
        update(refresh.cursor, refresh.cursor + run);
        refresh.cursor += run;
        budget -= run;
    }
}

// Merges `ranges` that overlap or touch, in place, in position order: the positions where several
// tracks' sweeps changed (all of them alike, as tracks play together) are then summed once.
void mergeRanges(std::vector<SweepImage::Range>& ranges)
{
    std::ranges::sort(ranges, {}, &SweepImage::Range::first);
    std::size_t kept = 0;
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        if (kept > 0 && ranges[index].first <= ranges[kept - 1].end) {
            ranges[kept - 1].end = std::max(ranges[kept - 1].end, ranges[index].end);
        } else {
            ranges[kept++] = ranges[index];
        }
    }
    ranges.resize(kept);
}

// Lane `index`'s bit in a frame's `invalidLanes`: the lanes, then the sum after them.
static_assert(kMaxLanes < 32, "every lane and the sum have a bit");
std::uint32_t laneBit(std::size_t index)
{
    return std::uint32_t{1} << index;
}

// The channel count a lane's image is read with: one view of the captured channels (another
// track's lane channel), or this track's captured channels.
int imageChannels(const SweepImage& image, int inputChannels)
{
    return image.channels() == 1 ? 1 : inputChannels;
}

// Whether any of `samples` is not finite (NaN or infinite, from a broken signal upstream).
bool holdsInvalid(std::span<const float> samples)
{
    return !std::ranges::all_of(samples, [](float sample) { return std::isfinite(sample); });
}

// Whether the blocks of `stats` holding samples [first, end) have any that is not finite.
bool holdsInvalid(const BlockStats& stats, std::size_t first, std::size_t end)
{
    const std::size_t block = stats.blockSamples();
    const std::span<const BlockStats::Block> blocks = stats.blocks();
    const std::size_t firstBlock = std::min(first / block, blocks.size());
    const std::size_t endBlock = std::min((end + block - 1) / block, blocks.size());
    return std::any_of(blocks.begin() + static_cast<std::ptrdiff_t>(firstBlock),
                       blocks.begin() + static_cast<std::ptrdiff_t>(endBlock),
                       [](const BlockStats::Block& level) { return level.invalid > 0; });
}

// `lanes` matches that are none: no correlation or offset (NaN), and not related (polarity 0).
void assignNoMatches(std::vector<float>& values, std::size_t lanes)
{
    values.assign(lanes * protocol::kMatchValues, std::numeric_limits<float>::quiet_NaN());
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        values[(lane * protocol::kMatchValues) + 2] = 0.0f;
    }
}

// A part's gain as a whole number for a sum's key: its bits, so that any change of it is one.
std::int64_t gainKey(float gain)
{
    return std::bit_cast<std::int32_t>(gain);
}

// Adds to `into` the wave through `neighbours` a fraction of a sample on, as `taps` weigh it,
// times `gain`: point i from neighbours i to i + taps.size() - 1. A stretch at a time, tap by
// tap: each pass adds one tap's share to every point of the stretch, with no sum carried from
// one multiply to the next, so the compiler does several points at once; summed point by point,
// a filter of 32 taps takes five times as long.
void addFiltered(std::span<const float> taps, const float* neighbours, float gain,
                 std::span<float> into)
{
    constexpr std::size_t kStretch = 512; // With its neighbours, well inside the nearest cache.
    for (std::size_t first = 0; first < into.size(); first += kStretch) {
        const std::size_t count = std::min(kStretch, into.size() - first);
        const float* const samples = neighbours + first;
        std::array<float, kStretch> sums{};
        for (std::size_t k = 0; k < taps.size(); ++k) {
            const float tap = taps[k];
            const float* const shifted = samples + k;
            for (std::size_t i = 0; i < count; ++i) {
                sums[i] += tap * shifted[i];
            }
        }
        float* const out = into.data() + first;
        for (std::size_t i = 0; i < count; ++i) {
            out[i] += gain * sums[i];
        }
    }
}

// What a sum of `parts` costs to recompute, in tracks read: a part read through a filter counts
// as four.
template <typename Parts> std::size_t weightOf(const Parts& parts)
{
    std::size_t weight = 0;
    for (const auto& part : parts) {
        weight += part.taps ? 4 : 1;
    }
    return weight;
}

// Whether a part's levels per block are other than its lane's as recorded: shifted (a flip
// changes no level).
template <typename Part> bool changesLevels(const Part& part)
{
    return part.offset != 0 || part.fraction != 0.0f;
}

// How many positions of a sum of `parts` tracks to recompute per frame, as one of `shares`
// recomputations under way together, which share the frame's kSumRefreshSamples.
std::size_t refreshStep(std::size_t parts, std::size_t shares)
{
    return kSumRefreshSamples / std::max<std::size_t>(shares, 1) / std::max<std::size_t>(parts, 1);
}

// Where the fixed sum is measured, of positions [first, end) of a span of `count`: where every
// compared track, shifted by its fix (offsets from `earliest`, at most 0, to `latest`, at least 0),
// has samples of its own. At the span's edges a shift reaches past them (into audio not captured
// yet, at the right edge live), and a track summed with that silence would read as a weaker fix
// than it is. Empty (first == end) when no position has them all.
std::pair<std::size_t, std::size_t> fixedPart(std::size_t first, std::size_t end, std::size_t count,
                                              std::ptrdiff_t earliest, std::ptrdiff_t latest)
{
    const auto from = std::max<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(first), -earliest);
    const auto to =
        std::max(from, std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(end),
                                                static_cast<std::ptrdiff_t>(count) - latest));
    return {static_cast<std::size_t>(from), static_cast<std::size_t>(to)};
}

// Frozen, positions [first, last) of `image` cropped for a shorter span: when the image is still
// filling in (rebuilt for a span longer than the hold, or begun partway through a cycle), that
// span rebuilt from the audio `held` up to `heldEnd`, which may cover more of it, is shown there
// instead, beside the image, which stays as it is for when the crop ends; otherwise nothing is.
// True if what is shown changed.
bool showHeldPart(SweepImage& image, const SampleSource& held, const MusicalStamp& clock,
                  std::uint64_t heldEnd, double sampleRate, protocol::BeatSpan span,
                  int inputChannels, protocol::ChannelView view, std::uint64_t first,
                  std::uint64_t last)
{
    const bool cropped = last > first && (first > 0 || last < image.length());
    if (!cropped || image.complete()) {
        const bool had = image.hasPart();
        image.clearPart();
        return had;
    }
    if (image.showsPart(first, last - first)) {
        return false;
    }
    const auto window = sweepWindow(clock, heldEnd, sampleRate, span, kMaxSweepSamples);
    SweepImage part(image.channels());
    if (!window || !part.update(held, *window, true, inputChannels, last - first, view)) {
        const bool had = image.hasPart();
        image.clearPart();
        return had;
    }
    image.showPart(part, first);
    return true;
}

} // namespace

Analyzer::Analyzer(Engine& engine, const InstanceRegistry& registry)
    : engine(engine), registry(registry), hold(engine.captureRing().capacity())
{}

LaneSet Analyzer::laneSet() const
{
    const std::scoped_lock lock(laneMutex);
    return published;
}

TrackList Analyzer::trackList() const
{
    const std::scoped_lock lock(laneMutex);
    return publishedTracks;
}

void Analyzer::publishLanes(std::vector<LaneLabel> labels, std::vector<TrackEntry> tracks)
{
    const std::scoped_lock lock(laneMutex);
    if (published.id == 0 || published.lanes != labels) {
        published = {.id = published.id + 1, .lanes = std::move(labels)};
    }
    if (publishedTracks.id == 0 || publishedTracks.tracks != tracks) {
        publishedTracks = {.id = publishedTracks.id + 1, .tracks = std::move(tracks)};
    }
}

void Analyzer::setColumns(std::size_t columns)
{
    const std::scoped_lock lock(commandMutex);
    commands.columns = columns;
}

void Analyzer::setFrozen(bool frozen)
{
    const std::scoped_lock lock(commandMutex);
    commands.frozen = frozen;
    if (!frozen) {
        commands.panSeconds = 0.0;
    }
}

void Analyzer::setPan(double seconds)
{
    const std::scoped_lock lock(commandMutex);
    commands.panSeconds = std::max(seconds, 0.0);
}

void Analyzer::setView(double start, double end)
{
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0.0 || end > 1.0 ||
        end - start < kMinimumViewWidth) {
        return;
    }
    const std::scoped_lock lock(commandMutex);
    commands.viewStart = start;
    commands.viewEnd = end;
}

// The selection is kept with the instance, so that it outlives this analyzer (the editor closed
// and opened again); it is not saved. This track is always shown, whether or not it is asked
// for: selectLanes shows it first and counts the others only.
void Analyzer::setShown(const std::vector<std::uint64_t>& ids)
{
    std::vector<std::uint64_t> next;
    for (const std::uint64_t id : ids) {
        if (!contains(next, id)) {
            next.push_back(id);
        }
    }
    engine.setShown(std::move(next));
}

void Analyzer::setPreviewFix(bool on)
{
    const std::scoped_lock lock(commandMutex);
    commands.previewFix = on;
}

void Analyzer::redraw()
{
    const std::scoped_lock lock(commandMutex);
    ++commands.redraws;
}

void Analyzer::arm()
{
    const std::scoped_lock lock(commandMutex);
    commands.armRequested = true;
}

// Readouts are shown at most every 100 ms, so live they are measured no more often than every
// 100 ms of audio, unless the settings, the view to measure (`view`, which a track turned mono
// changes) or the zoom (`shown`) changed. Frozen frames come only when something changed (such as a
// pan), so they always measure. `end` is this track's capture end.
bool Analyzer::readoutsDue(std::uint64_t end, ChannelView view, const ReadoutView& shown) const
{
    const auto interval =
        static_cast<std::uint64_t>(protocol::kReadoutIntervalSeconds * engine.sampleRate());
    return isFrozen || !readoutCache.valid || readoutCache.view != view ||
           readoutCache.settings != frameSettings || readoutCache.frozen != isFrozen ||
           readoutCache.shown != shown || end < readoutCache.end ||
           end - readoutCache.end >= interval;
}

void Analyzer::storeReadouts(const std::optional<Readouts>& values, std::uint64_t end,
                             ChannelView view, const ReadoutView& shown)
{
    if (!values) {
        return;
    }
    readoutCache.shown = shown;
    readoutCache.values = *values;
    readoutCache.end = end;
    readoutCache.view = view;
    readoutCache.valid = true;
}

void Analyzer::applyReadouts(FrameHeaderValues& header)
{
    if (!readoutCache.valid) {
        return;
    }
    readoutCache.settings = frameSettings;
    readoutCache.frozen = isFrozen;
    header.readoutView = readoutCache.view;
    header.invalidSamples = readoutCache.values.invalid;
    header.peak = readoutCache.values.peak;
    header.rms = readoutCache.values.rms;
    header.truePeak = readoutCache.values.truePeak;
    header.dcOffset = readoutCache.values.dcOffset;
    header.loudness = readoutCache.values.loudness;
    header.correlation = readoutCache.values.correlation;
}

Analyzer::Commands Analyzer::takeCommands()
{
    const std::scoped_lock lock(commandMutex);
    Commands taken = commands;
    commands.armRequested = false;
    return taken;
}

void Analyzer::followTransport(const Settings& settings, Commands& command)
{
    const bool playing = engine.timeline().latest().playing;
    const bool enabled =
        settings.enumValue<protocol::Toggle>(SettingId::kAutoFreeze) == protocol::Toggle::kOn;
    if (!enabled || !command.frozen) {
        autoFrozen = false;
        resumeAt.reset(); // Unfrozen by hand, or Auto-freeze turned off, while waiting.
    }
    if (enabled && lastPlaying.has_value()) {
        if (*lastPlaying && !playing && !command.frozen) {
            setFrozen(true);
            command.frozen = true;
            autoFrozen = true;
        } else if (!*lastPlaying && playing && autoFrozen) {
            if (settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) ==
                protocol::TimeBase::kBeats) {
                // The paced playhead is still in audio from the stop: frozen until it reaches what
                // was played after (resume).
                resumeAt = engine.captureRing().endIndex();
            } else {
                unfreezeAfterStop(command);
            }
        } else if (*lastPlaying && !playing) {
            resumeAt.reset(); // Stopped again while waiting: still frozen, until the next play.
        }
    }
    lastPlaying = playing;
    resume(settings, command);
}

void Analyzer::unfreezeAfterStop(Commands& command)
{
    setFrozen(false);
    command.frozen = false;
    command.panSeconds = 0.0;
    autoFrozen = false;
    resumeAt.reset();
}

void Analyzer::resume(const Settings& settings, Commands& command)
{
    if (!resumeAt) {
        return;
    }
    const std::uint64_t until = *resumeAt;
    // The edge moves on while frozen only here: once it reaches audio played after the stop, or
    // is not paced (it is then the newest audio, past it), the view goes live.
    const std::uint64_t edge =
        ownSweepEdge(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan));
    if (!pacedEdge.paced() || edge >= until) {
        unfreezeAfterStop(command);
    }
}

bool Analyzer::freeze(double sampleRate, std::uint64_t minimumSamples,
                      std::optional<std::uint64_t> holdEnd)
{
    hold.resize(engine.captureRing().capacity()); // The rate may have changed since it opened.
    frozenStamp = engine.timeline().latest();
    frozenRate = sampleRate;
    const auto length = std::max<std::uint64_t>(hold.holdLength(sampleRate), minimumSamples);
    if (!hold.capture(engine.captureRing(), static_cast<std::size_t>(length), holdEnd)) {
        return false; // The writer outran the copy; try again on the next step.
    }
    frozenEnd = hold.endIndex();
    frozenStamp = clockAt(engine.timeline(), frozenEnd, frozenStamp);
    isFrozen = true;
    const std::scoped_lock lock(commandMutex);
    commands.frozen = true; // Single mode freezes on its own; keep the command in step.
    return true;
}

bool Analyzer::produce(std::vector<std::byte>& frame)
{
    if (!produceFrame(frame)) {
        return false;
    }
    // Only Compare draws frames for its results alone; other frames are not kept to compare.
    if (!comparing) {
        lastSent.clear();
        return true;
    }
    if (resultsOnly && frame == lastSent) {
        return false; // The same frame again: a search or gains ended with the same values.
    }
    lastSent.assign(frame.begin(), frame.end());
    return true;
}

bool Analyzer::produceFrame(std::vector<std::byte>& frame)
{
    spreadStepped = false;
    searchEnded = false;
    resultsOnly = false;
    // The block clock sees every stamp, whichever path returns below: a frame that does not change
    // (frozen in Multi and Beats) keeps the request open, so the worker asks again at each step and
    // never advances, and blocks seen only after a long freeze would fold into one late update.
    pacedEdge.observe(engine.blockStamp().latest(), engine.sampleRate());
    Commands command = takeCommands();
    const Settings settings = engine.settings().get();
    frameSettings = settings;
    followTransport(settings, command);
    const double liveRate = engine.sampleRate();
    if (command.armRequested) {
        trigger.arm();
    }
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    if (command.frozen && !isFrozen) {
        // A live Beats sweep is held up to the paced playhead now, not to the newest audio,
        // which it had not reached (the newest when not paced, as the playhead then is).
        const bool multiNow = settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) !=
                              protocol::DisplayMode::kSingle;
        const std::optional<std::uint64_t> placed =
            synced && !multiNow && !isFrozen
                ? std::optional(
                      ownSweepEdge(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan)))
                : std::nullopt;
        if (!freeze(liveRate, frozenLength(engine.captureRing(), liveRate, synced), placed)) {
            return false;
        }
        if (synced) {
            catchUpImageToHold();
        }
    }
    if (!command.frozen) {
        isFrozen = false;
    }
    // Frozen, what is shown is the audio held, placed and timed at the rate it was recorded at.
    const double sampleRate = isFrozen ? frozenRate : liveRate;
    if (command.columns == 0) {
        return false;
    }
    // Multi draws lanes of the tracks shown, with Compare on or off.
    const bool multi = settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) !=
                       protocol::DisplayMode::kSingle;
    // A Beats sweep is placed by the host's tempo. Until the host has reported one (an instance
    // it has not played through), the clock's tempo is only a default: no sweep is drawn, and the
    // frame says why, again whenever the inputs change or the page asks for a redraw.
    if (synced && !(isFrozen ? frozenStamp : engine.timeline().latest()).tempoKnown) {
        // Multi still selects the tracks shown and sends their set and the track list, so the
        // page has their names ready; it names lanes only once a frame carries
        // them, after the host reports a tempo (FR-9.1: nothing is placed until then).
        std::uint32_t laneSetId = 0;
        if (multi) {
            selectLanes(settings);
            laneSetId = laneSet().id;
        }
        // A track turned mono changes the channel the frame names, though no setting changed.
        const bool newFormat = !isFrozen && formatsChanged(multi);
        if (sentNoTempo == isFrozen && !newFormat && !inputsChanged(settings, command, laneSetId)) {
            return false;
        }
        sentNoTempo = isFrozen;
        rememberInputs(settings, command, laneSetId);
        if (!isFrozen) {
            lastLiveFormats = liveFormats;
        }
        // Shown in Beats: switched to Seconds, the newest span is shown at once, as after a sweep.
        lastShown = Window{.synced = true};
        const FrameHeaderValues header{
            .flags = static_cast<std::uint8_t>(static_cast<unsigned>(flag(FrameFlag::kSynced)) |
                                               flag(FrameFlag::kNoTempo) |
                                               (isFrozen ? flag(FrameFlag::kFrozen) : 0U)),
            .laneSet = laneSetId,
            .sampleRate = static_cast<float>(sampleRate),
            // No readouts yet, but the channel they will measure, which the page names.
            .readoutView = multi ? ownLaneView(settings)
                                 : effectiveReadoutView(
                                       settings.enumValue<ChannelView>(SettingId::kReadoutSource),
                                       effectiveViews(static_cast<std::uint8_t>(
                                                          settings.value(SettingId::kChannelViews)),
                                                      engine.inputChannels())),
            .triggerView = triggerViewFor(settings, multi)};
        encodeFrame(header, {}, frame);
        return true;
    }
    sentNoTempo.reset();
    if (multi && synced) {
        return produceLanes(settings, command, frame);
    }
    if (multi) {
        selectLanes(settings);
    } else {
        lanes.clear(); // Frees the lanes' sweeps and holds outside the multi-track view.
        comparing = false;
    }

    const bool freeRunning =
        isFreeRunning(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase));
    // Each of Trigger and Free has a span of its own (Beats places its sweep from the beat span).
    const double spanSeconds =
        settings.value(freeRunning ? SettingId::kFreeSpan : SettingId::kTriggerSpan);
    // The capture is sized for the longest span at its rate, but a host that changes the rate
    // while processing leaves it as it was, and above the highest rate it grows no more: a longer
    // span shows what is kept, and says so. Live, that is all the capture can be read of. Frozen,
    // all that is held of what the capture could be read of then (the host may have prepared it
    // again since). Other tracks' lanes hold no more, placed by their own clocks, so a margin is
    // kept for the offset between tracks: what the capture spares beyond the longest span, up to
    // a lane's margin, or an eighth of what it keeps when it cannot hold that span.
    const auto readable = static_cast<double>(CaptureRing::readableLength(
        isFrozen ? hold.heldCapacity() : engine.captureRing().capacity()));
    // In whole samples, as holds are: the longest span held whole is then never short of a margin
    // by a fraction of a sample.
    const double spare =
        readable - static_cast<double>(std::llround(HoldBuffer::kHoldSeconds * sampleRate));
    const double laneMargin = !multi        ? 0.0
                              : spare > 0.0 ? std::min(spare, kLaneMarginSeconds * sampleRate)
                                            : readable / 8.0;
    const double kept = isFrozen
                            ? std::min(static_cast<double>(hold.endIndex() - hold.beginIndex()),
                                       readable - laneMargin)
                            : readable;
    // Holds are whole samples: at a rate that is not, a held span a fraction of a sample short
    // of the span's length holds all of it.
    const bool spanLimited =
        !synced && static_cast<double>(std::llround(spanSeconds * sampleRate)) > kept;
    const double span = spanLimited ? kept : spanSeconds * sampleRate;
    // Live in Trigger or Free, a frame shows the newest audio: with no new audio on any track shown
    // and nothing else changed, it would be the last frame again, so none is drawn. The page asks
    // at display rate, also when the transport is stopped or the host has stopped processing.
    const bool liveSeconds = !synced && !isFrozen;
    // Where a free-running window, or Auto's untriggered fallback, ends: the paced edge, which
    // moves evenly between blocks, or the newest sample where pacing does not apply (short spans,
    // whose whole trace is new every frame anyway; before the block clock locks; offline).
    const std::uint64_t newest = engine.captureRing().endIndex();
    pacedEdge.observe(engine.blockStamp().latest(), liveRate);
    const bool paceable = liveSeconds && span >= protocol::kPacedSpanMinimumSeconds * sampleRate;
    std::uint64_t edge = newest;
    if (paceable) {
        edge = pacedEdge.edge(clockNanos(), newest, protocol::kPacingDelayCapSeconds);
    } else if (liveSeconds) {
        edge = pacedEdge.showNewest(clockNanos(), newest);
    }
    // A free-running window, or Auto drawing untriggered, is new when its edge moved; a triggered
    // window only with new audio, so no work is done between blocks.
    const bool followsEdge =
        freeRunning || (settings.enumValue<protocol::TriggerMode>(SettingId::kTriggerMode) ==
                            protocol::TriggerMode::kAuto &&
                        lastShown && !lastShown->triggered && !lastShown->synced);
    if (liveSeconds) {
        liveEnds.clear();
        liveEnds.push_back(followsEdge ? edge : newest);
        if (multi) {
            for (const std::uint64_t serial : laneOrder) {
                liveEnds.push_back(lanes.at(serial)->engine->captureRing().endIndex());
            }
        }
    }
    const bool newAudio = !liveSeconds || liveEnds != lastLiveEnds;
    const bool newFormat = !isFrozen && formatsChanged(multi);
    // Other lanes, or other tracks compared, change the frame too. A match still to search does
    // only when its search can run: live with the transport playing, searches are spaced by audio,
    // so without new audio none would, and the frame drawn would be the same; and not while
    // another runs. Gains not measured for what is shown do, to begin measuring them.
    const std::uint32_t laneSetId = multi ? laneSet().id : 0;
    const bool inputs = inputsChanged(settings, command, laneSetId) || newFormat;
    resultsOnly = !inputs && (isFrozen || !newAudio);
    bool changed = inputs || (comparing &&
                              ((matchesPending && !searchingSerial && (newAudio || searchAtOnce)) ||
                               (!comparisonCache.valid && !gainJob.active)));
    // A search or gains running over frames go on whatever arrives, but change what is shown only
    // when one ends: until then they are stepped without drawing the same frame again.
    if (!changed && comparing && (searchingSerial || gainJob.active)) {
        changed = stepSpreadWork(liveRate);
    }
    if (liveSeconds && !changed && !newAudio && lastShown && !lastShown->synced) {
        return false;
    }

    Window window{.span = span, .freeRunning = freeRunning, .limited = spanLimited};
    auto sampleCount = static_cast<std::uint64_t>(std::llround(span));
    bool freezeAfterRender = false;
    const SampleSource* source =
        isFrozen ? static_cast<const SampleSource*>(&hold) : &engine.captureRing();

    if (synced) {
        // Tempo-synced sweep. Live, the capture's new samples are placed. Frozen, the sweep stays
        // as it is: a shorter span is cropped from it, and a longer one rebuilt from the held
        // audio, stepping down if it does not fit.
        const CaptureRing& ring = engine.captureRing();
        // Live, the sweep and its playhead reach the paced edge, so they move evenly between
        // blocks and no audio newer than the playhead shows in the dimmed previous cycle.
        const std::uint64_t end =
            isFrozen ? frozenEnd
                     : ownSweepEdge(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan));
        if (!changed && end == lastEnd) {
            return false;
        }
        const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
        const int channels = engine.inputChannels();
        SweepWindow sweep;
        if (isFrozen) {
            const auto frozen = frozenSweep(frozenStamp, frozenEnd, sampleRate, beatSpan,
                                            imageWindow, image.length());
            if (!frozen) {
                return false;
            }
            if (frozen->rebuild) {
                if (!image.update(hold, frozen->window, true, channels)) {
                    return false;
                }
                imageWindow = frozen->window;
            } else {
                const auto count = static_cast<double>(image.length());
                // Every channel held, as the image holds: the view picks none.
                showHeldPart(image, hold, frozenStamp, frozenEnd, sampleRate, beatSpan, channels,
                             ChannelView::kMid,
                             static_cast<std::uint64_t>(std::llround(frozen->start * count)),
                             static_cast<std::uint64_t>(std::llround(frozen->end * count)));
            }
            sweep = frozen->window;
            window.subStart = frozen->start;
            window.subEnd = frozen->end;
        } else {
            const auto placeBefore = [&](const MusicalStamp& previous, std::uint64_t until) {
                if (const auto before =
                        sweepWindow(previous, until, sampleRate, beatSpan, kMaxSweepSamples)) {
                    ++workDone.sweepUpdates;
                    (void)image.update(ring, *before, false, channels);
                }
            };
            const auto live =
                sweepWindow(placeThroughChanges(engine.timeline(),
                                                std::max(image.placedTo(), ring.beginIndex()), end,
                                                engine.timeline().latest(), placeBefore),
                            end, sampleRate, beatSpan, kMaxSweepSamples);
            ++workDone.sweepUpdates;
            if (!live || !image.update(ring, *live, false, channels)) {
                return false;
            }
            imageWindow = live;
            placedSpan = beatSpan;
            sweep = *live;
        }
        source = &image;
        window.start = 0.0;
        window.span = static_cast<double>(image.length());
        window.freeRunning = false;
        window.synced = true;
        window.limited = sweep.limited;
        window.partial = !(image.complete() || image.partComplete());
        window.cursor = sweep.cursor;
        window.cycleStart = sweep.cycleStart;
        window.spanQuarterNotes = sweep.spanQuarterNotes;
        window.barQuarterNotes = sweep.barQuarterNotes;
        window.cycleSeconds = sweep.cycleSeconds;
        sampleCount = image.length();
        lastEnd = end;

    } else if (isFrozen) {
        if (!changed) {
            return false;
        }
        // Keep the trigger point (triggered) or the right edge (free-running) of the last live
        // window where it was, so changing the scale while frozen zooms around it. A free-running
        // live window has no trigger point: Trigger chosen while frozen shows its span ending at
        // that window's right edge, as a live switch shows the newest span. A Beats sweep's
        // window is a place in the sweep, not in the capture: frozen from Beats, the view ends at
        // the newest audio held, as when frozen before any live frame. The trigger point stays
        // where the Position puts it, unless too little is held after it; an untriggered view
        // counts time from the Position, wherever that leaves it.
        const double position = settings.value(SettingId::kTriggerPosition);
        const auto held = static_cast<double>(hold.endIndex());
        const std::optional<Window> live =
            lastLive && !lastLive->synced ? lastLive : std::optional<Window>{};
        const double rightEdge = live && live->freeRunning ? live->edge
                                 : live                    ? live->start + live->span
                                                           : held;
        const bool fromEdge = !live || freeRunning || live->freeRunning;
        const double anchor = fromEdge ? rightEdge : live->zero;
        const double anchorOffset = fromEdge ? span : position * span;
        const double latestStart = held - span;
        // A view pans back through the newest frozenHoldSeconds held. The hold reaches a little
        // further (kSecondsFreezeSeconds), so the unpanned window, which may end a paced delay
        // before the newest audio, is never moved forward to fit.
        const auto heldStart = static_cast<double>(hold.beginIndex());
        const double earliestStart = std::max(
            heldStart,
            held - static_cast<double>(std::llround(protocol::kFrozenHoldSeconds * sampleRate)));
        const double unpanned = anchor - anchorOffset;
        const double lowest = std::max(heldStart, std::min(earliestStart, unpanned));
        const double highest = std::max(lowest, latestStart);
        // The pan counts back from where the span is placed, so all of its reach moves the view.
        window.placedStart = std::clamp(unpanned, lowest, highest);
        window.start =
            std::clamp(window.placedStart - command.panSeconds * sampleRate, lowest, highest);
        // The UI keeps its pan within this, so it never asks for more than is held.
        window.panReach = std::max(0.0, (window.placedStart - earliestStart) / sampleRate);
        window.triggered = live && live->triggered;
        window.zero = freeRunning        ? std::numeric_limits<double>::quiet_NaN()
                      : window.triggered ? anchor
                                         : window.placedStart + position * span;
    } else if (freeRunning) {
        const std::uint64_t end = edge;
        const auto spanSamples = static_cast<std::uint64_t>(std::llround(span));
        const auto visibleSamples = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(std::llround((command.viewEnd - command.viewStart) * span)));
        window.edge = static_cast<double>(end);
        if (visibleSamples <= command.columns) {
            // Raw samples: the view of the whole span, as a triggered window crops it, read with
            // a sample beyond each edge so the trace reaches past the screen's edges. Only those
            // are read: the edge moves every frame, and the span may be seconds long.
            const std::uint64_t count = std::min(spanSamples, end);
            const auto wholeStart = static_cast<double>(end - count);
            const double from = wholeStart + command.viewStart * static_cast<double>(count);
            const double to = wholeStart + command.viewEnd * static_cast<double>(count);
            // The neighbours for the wave between the samples are read past these, from the
            // audio around the window, as for any window (cropOf): before the span's start, and
            // past the paced edge as far as the capture (or the hold) has the audio.
            const double first = std::max(wholeStart, std::floor(from) - 1.0);
            const std::uint64_t held = std::max(end, source->endIndex());
            const double last = std::min(static_cast<double>(held), std::ceil(to) + 2.0);
            sampleCount = static_cast<std::uint64_t>(std::max(0.0, last - first));
            window.start = first;
            if (sampleCount > 0) {
                window.cropStart = (from - first) / static_cast<double>(sampleCount);
                window.cropEnd = (to - first) / static_cast<double>(sampleCount);
            }
            window.visiblePart = true;
        } else {
            // Columns: the window covers only the visible part, on a grid of its own, so its
            // columns keep fixed sample boundaries and do not shimmer; readouts still cover the
            // whole span.
            const std::uint64_t hidden = std::min<std::uint64_t>(
                end, static_cast<std::uint64_t>(std::llround((1.0 - command.viewEnd) * span)));
            const FreeWindow shown = freeWindow(end - hidden, visibleSamples, command.columns);
            window.start = static_cast<double>(shown.start);
            window.visiblePart = true;
            window.onGrid = shown.onGrid;
            window.grid = {.spanSamples = visibleSamples, .columns = command.columns};
            window.firstColumn = shown.firstColumn;
            window.cropStart = 0.0;
            window.cropEnd = 1.0;
            sampleCount = shown.sampleCount;
        }
    } else {
        const CaptureRing& ring = engine.captureRing();
        const double timeout = kAutoTimeoutSeconds * sampleRate;
        const auto [first, end] =
            TriggerState::searchRegion(ring.beginIndex(), ring.endIndex(), span, timeout);
        triggerSamples.resize(static_cast<std::size_t>(end - first));
        scratch.resize(std::max(scratch.size(), triggerSamples.size()));
        const ChannelView triggerView = triggerViewFor(settings, multi);
        if (!readSamples(ring, engine.inputChannels(), triggerView, first, triggerSamples,
                         scratch)) {
            return false;
        }
        const TriggerDecision decision = trigger.decide(
            triggerSamples, first, span, timeout, triggerParameters(settings, sampleRate),
            settings.enumValue<protocol::TriggerMode>(SettingId::kTriggerMode));
        if (decision.action == TriggerDecision::Action::kHold && lastShown &&
            (lastShown->synced || lastShown->freeRunning)) {
            // Just switched from Beats, or from a free-running window: the last window was a
            // place in a sweep, or another kind of window. Show the newest span, untriggered,
            // then wait for a trigger from there.
            window.start = static_cast<double>(end) - span;
        } else if (decision.action == TriggerDecision::Action::kHold) {
            if (!changed || !lastShown) {
                return false;
            }
            // A held trace keeps its trigger point; a new Position applies from the next trigger.
            window.start = lastShown->start;
            window.triggered = lastShown->triggered;
            window.zero = lastShown->zero;
        } else {
            // Auto drawing untriggered shows the newest span, ending at the paced edge.
            window.start =
                decision.triggered ? decision.windowStart : static_cast<double>(edge) - span;
            window.triggered = decision.triggered;
            window.zero =
                decision.triggered ? decision.trigger : std::numeric_limits<double>::quiet_NaN();
            freezeAfterRender = decision.freeze;
        }
        // An untriggered window counts time from the Position, whatever it and the span are now.
        if (!window.triggered) {
            window.zero = window.start + settings.value(SettingId::kTriggerPosition) * span;
        }
        window.placedStart = window.start;
    }

    window.viewStart = command.viewStart;
    window.viewEnd = command.viewEnd;
    if (!window.visiblePart) {
        const double shown = window.subEnd - window.subStart;
        window.cropStart = window.subStart + command.viewStart * shown;
        window.cropEnd = window.subStart + command.viewEnd * shown;
    }
    if (multi
            ? !renderLanes(*source, window, sampleCount, sampleRate, command, frame)
            : !render(*source, settings, sampleRate, window, sampleCount, command.columns, frame)) {
        return false;
    }
    if (!isFrozen) {
        lastLive = window;
        lastLiveEnds = liveEnds;
        lastLiveFormats = liveFormats;
        // For benchmarks: where a moving frame's right edge stood (a sweep's playhead in Beats).
        if (window.freeRunning || window.synced) {
            engine.recordEdge({.nanos = clockNanos(),
                               .edge = window.synced ? lastEnd : edge,
                               .newest = engine.captureRing().endIndex()});
        }
    }
    lastShown = window;
    rememberInputs(settings, command, laneSetId);
    if (freezeAfterRender) {
        freeze(sampleRate);
        lastFrozen = isFrozen; // The frame just sent already shows the held window.
    }
    return true;
}

void Analyzer::reconstruct(FrameHeaderValues& header, std::size_t blocks, const Crop& crop,
                           std::size_t factor,
                           const std::function<std::span<const std::size_t>(std::size_t)>& seams)
{
    const std::size_t points = header.pointCount;
    if (header.mode != protocol::FrameMode::kRaw || points == 0 ||
        (factor <= 1 && crop.lead + crop.trail == 0)) {
        return;
    }
    blocks = std::min(blocks, frameData.size() / points);
    reconstructed.clear();
    for (std::size_t block = 0; block < blocks; ++block) {
        reconstruction.upsample(std::span(frameData).subspan(block * points, points), crop.lead,
                                crop.trail, factor, upsampled, seams(block));
        reconstructed.insert(reconstructed.end(), upsampled.begin(), upsampled.end());
    }
    // Data after the blocks (a Compare frame's matches) is kept as it is.
    reconstructed.insert(reconstructed.end(),
                         frameData.begin() + static_cast<std::ptrdiff_t>(blocks * points),
                         frameData.end());
    std::swap(frameData, reconstructed);
    header.pointCount = static_cast<std::uint32_t>(upsampled.size());
    header.startFraction = static_cast<float>(crop.startFraction - static_cast<double>(crop.lead));
    header.pointsPerSample = static_cast<std::uint32_t>(std::max<std::size_t>(factor, 1));
}

bool Analyzer::render(const SampleSource& source, const Settings& settings, double sampleRate,
                      const Window& window, std::uint64_t sampleCount, std::size_t columns,
                      std::vector<std::byte>& frame)
{
    const int inputChannels = engine.inputChannels();
    const double first = std::floor(window.start);
    const auto start = static_cast<std::uint64_t>(std::max(first, 0.0));

    const auto views = effectiveViews(
        static_cast<std::uint8_t>(settings.value(SettingId::kChannelViews)), inputChannels);

    // A window's neighbours come from the audio around it too, a sweep's from round its wrap.
    const auto [crop, factor] = viewCropOf(static_cast<double>(sampleCount), window.start - first,
                                           window.cropStart, window.cropEnd, columns, true);
    const double visibleSamples = crop.visibleSamples;

    FrameHeaderValues header{
        .flags =
            static_cast<std::uint8_t>((window.triggered ? flag(FrameFlag::kTriggered) : 0U) |
                                      (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                      (window.freeRunning ? flag(FrameFlag::kFreeRunning) : 0U) |
                                      (window.synced ? flag(FrameFlag::kSynced) : 0U) |
                                      (window.limited ? flag(FrameFlag::kLimited) : 0U) |
                                      (window.partial ? flag(FrameFlag::kPartial) : 0U)),
        .views = views,
        .startFraction = static_cast<float>(crop.startFraction - static_cast<double>(crop.lead)),
        .cursor = window.cursor,
        .cycleStart = window.cycleStart,
        .spanQuarterNotes = static_cast<float>(window.spanQuarterNotes),
        .barQuarterNotes = static_cast<float>(window.barQuarterNotes),
        .cycleSeconds = window.cycleSeconds,
        .viewStart = window.viewStart,
        .viewEnd = window.viewEnd,
        .visibleSamples = static_cast<float>(visibleSamples),
        .panReach = static_cast<float>(window.panReach),
        .sampleRate = static_cast<float>(sampleRate),
        .spanSeconds = window.synced ? 0.0f : static_cast<float>(window.span / sampleRate),
        .triggerPosition = triggerPositionOf(window)};
    frameData.clear();
    std::size_t pointCount = 0;
    // Each view keeps its columns from frame to frame where it can: a tempo-synced sweep reduces
    // again only the columns its newest samples landed in, a free-running window only the columns
    // that scrolled in. A triggered window is new every time.
    const bool synced = window.synced;
    // Readouts measure the readout source when it is drawn, else the first view drawn, whatever the
    // trigger reads; a mono track draws only L.
    const ChannelView readoutView =
        effectiveReadoutView(settings.enumValue<ChannelView>(SettingId::kReadoutSource), views);
    header.readoutView = readoutView;
    header.triggerView = triggerViewFor(settings, false);
    const bool rolling = window.freeRunning && window.onGrid && !window.synced &&
                         window.cropStart == 0.0 && window.cropEnd == 1.0;
    // A sweep's playhead joins the newest pass to the previous one: no curve across it. Columns
    // draw no wave across any join in the image: the playhead's, and a transport jump's.
    viewSeams.clear();
    if (synced) {
        addPlayhead(window.subStart + window.cursor * (window.subEnd - window.subStart),
                    sampleCount, crop, viewSeams);
        addJoins(image, crop, viewSeams);
    }
    const std::size_t visibleCount = crop.end - crop.first;
    const bool sameReduction = viewReduction.first == crop.first &&
                               viewReduction.channels == inputChannels &&
                               viewReduction.views == views;
    if (!rolling || viewReduction.channels != inputChannels) {
        for (RollColumns& roll : viewRolls) {
            roll.clear();
        }
    }
    for (std::size_t index = 0; index < protocol::kChannelViewCount; ++index) {
        if ((views & (1U << index)) == 0) {
            continue;
        }
        const auto view = static_cast<ChannelView>(index);
        // Reads for the view's columns: a sweep's or a free-running window's.
        const auto read = [&](std::uint64_t first, std::span<float> destination) {
            workDone.samplesReduced += destination.size();
            readScratch.resize(std::max(readScratch.size(), destination.size()));
            return readSamples(source, inputChannels, view, first, destination, readScratch);
        };
        if (synced) {
            ColumnReduction& reduction = viewReductions.at(index);
            const auto readVisible = [&](std::size_t first, std::span<float> destination) {
                return readWrapped(read, sampleCount,
                                   blockStart(crop) + static_cast<std::int64_t>(first),
                                   destination);
            };
            bool updated = true;
            if (!sameReduction || !reduction.fits(visibleCount, columns, crop.lead, crop.trail)) {
                // Tracking what is not finite in the views that show every broken channel:
                // Single has no lanes, and bit 0 says whether the part of a drawn trace on screen
                // holds such samples.
                reduction.reset(visibleCount, columns,
                                (invalidCheckViews(views) & (1U << index)) != 0, crop.lead,
                                crop.trail);
                updated = reduction.update(0, visibleCount, readVisible, viewSeams);
            } else {
                for (const SweepImage::Range& range : image.changes()) {
                    forEachInBlock(
                        static_cast<std::int64_t>(range.first),
                        static_cast<std::int64_t>(range.end), crop, visibleCount, sampleCount,
                        [&](std::size_t from, std::size_t to) {
                            updated = reduction.update(from, to, readVisible, viewSeams) && updated;
                        });
                }
            }
            if (!updated) {
                return false;
            }
            // Only what the screen shows: neighbours read for the wave are not flagged. Raw values
            // carry them; min/max columns track their own samples alone.
            header.invalidLanes |=
                (reduction.mode() == protocol::FrameMode::kRaw && crop.lead + crop.trail > 0
                     ? (invalidCheckViews(views) & (1U << index)) != 0 &&
                           holdsInvalid(shownPart(reduction.values(), crop))
                     : reduction.holdsInvalid())
                    ? 1U
                    : 0U;
            header.mode = reduction.mode();
            pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visibleCount;
            frameData.insert(frameData.end(), reduction.values().begin(), reduction.values().end());
            continue;
        }
        if (rolling) {
            const auto values =
                viewRolls.at(index).update(0, window.firstColumn, window.grid, read);
            header.invalidLanes |= viewRolls.at(index).holdsInvalid() ? 1U : 0U;
            header.mode = protocol::FrameMode::kMinMax;
            pointCount = columns;
            frameData.insert(frameData.end(), values.begin(), values.end());
            continue;
        }
        // The window's part of the crop, which must all be held, and its neighbours past the
        // window's ends as the source holds them.
        if (start < source.beginIndex() || start + sampleCount > source.endIndex()) {
            return false;
        }
        viewSamples.resize(crop.end - crop.first);
        scratch.resize(std::max(scratch.size(), viewSamples.size()));
        if (!fillBlock(source, inputChannels, view, static_cast<std::int64_t>(start),
                       static_cast<std::int64_t>(sampleCount),
                       static_cast<std::int64_t>(crop.first) -
                           static_cast<std::int64_t>(crop.offset),
                       viewSamples, scratch, [&](std::int64_t from, std::span<float> destination) {
                           readScratch.resize(std::max(readScratch.size(), destination.size()));
                           return readSamples(source, inputChannels, view,
                                              start + static_cast<std::uint64_t>(from), destination,
                                              readScratch);
                       })) {
            return false;
        }
        const std::span<const float> visible = viewSamples;
        header.invalidLanes |= holdsInvalid(shownPart(visible, crop)) ? 1U : 0U;
        workDone.samplesReduced += visible.size();
        header.mode = reduceToColumns(visible, crop.lead, crop.trail, columns, reduced, kNoSeam,
                                      &columnPeaks);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visible.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    }
    if (synced) {
        // The levels per block, kept in step with the sweep on every frame, and measured afresh
        // from the whole sweep when the readout view changes. With the Meter on, its statistics
        // too; off, they are not kept.
        const std::size_t block = statsBlock(sampleRate);
        const bool fresh = !sweepStats.fits(static_cast<std::size_t>(sampleCount), block) ||
                           sweepStatsChannels != inputChannels || sweepStatsView != readoutView;
        sweepStatsChannels = inputChannels;
        sweepStatsView = readoutView;
        if (!updateStats(sweepStats, image, readoutView, inputChannels, fresh, block)) {
            return false;
        }
        if (!metered()) {
            sweepMeter.clear();
        } else if (!updateMeter(sweepMeter, image, readoutView, inputChannels,
                                fresh ||
                                    !sweepMeter.fits(static_cast<std::size_t>(sampleCount), block,
                                                     sampleRate, std::min(inputChannels, 2)),
                                block, sampleRate)) {
            return false;
        }
        image.clearChanges();
        viewReduction = {.first = crop.first, .channels = inputChannels, .views = views};
    } else {
        viewReduction = {};
    }
    if (rolling) {
        viewReduction.channels = inputChannels;
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    reconstruct(header, frameData.size(), crop, factor,
                [&](std::size_t) { return std::span<const std::size_t>(viewSeams); });

    // Readouts cover the visible part, at most every 100 ms of audio and whenever the zoom
    // changes: a sweep's from its statistics per block, a window's from its samples.
    const std::uint64_t end = engine.captureRing().endIndex();
    const ReadoutView shown{.viewStart = window.viewStart, .viewEnd = window.viewEnd};
    if (readoutsDue(end, readoutView, shown)) {
        const auto [shownFirst, shownEnd] = meterRange(crop, static_cast<std::size_t>(sampleCount));
        if (synced && metered()) {
            meterJoins.assign(image.joins().begin(), image.joins().end());
            std::ranges::sort(meterJoins);
            storeReadouts(sweepMeter.measure(shownFirst, shownEnd, meterJoins,
                                             meterReader(image, inputChannels, readoutView, 0)),
                          end, readoutView, shown);
        } else if (synced) {
            storeReadouts(levelsOf(sweepStats, shownFirst, shownEnd,
                                   statsReader(image, inputChannels, readoutView)),
                          end, readoutView, shown);
        } else {
            storeReadouts(measureWindow(source, inputChannels, readoutView, start + shownFirst,
                                        start + shownEnd, sampleRate),
                          end, readoutView, shown);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    return true;
}

bool Analyzer::updateStats(BlockStats& stats, const SweepImage& sweep, ChannelView view,
                           int channels, bool fresh, std::size_t block)
{
    const BlockStats::Reader read = statsReader(sweep, channels, view);
    const auto length = static_cast<std::size_t>(sweep.length());
    if (fresh) {
        stats.reset(length, block);
        return stats.update(0, length, read);
    }
    bool measured = true;
    for (const SweepImage::Range& range : sweep.changes()) {
        measured = stats.update(static_cast<std::size_t>(range.first),
                                static_cast<std::size_t>(range.end), read) &&
                   measured;
    }
    return measured;
}

BlockStats::Reader Analyzer::statsReader(const SampleSource& source, int channels, ChannelView view)
{
    return [this, &source, channels, view](std::size_t first, std::span<float> destination) {
        readScratch.resize(std::max(readScratch.size(), destination.size()));
        return readSamples(source, channels, view, first, destination, readScratch);
    };
}

// The Peak, RMS and invalid count of samples [first, end) of a span, from its levels per block and
// the samples of the blocks at the edges: what the engine measures of the Meter while it is off.
// No value for an empty part; nothing if a read fails.
std::optional<Readouts> Analyzer::levelsOf(BlockStats& stats, std::size_t first, std::size_t end,
                                           const BlockStats::Reader& read)
{
    Readouts readouts;
    if (first >= end) {
        return readouts;
    }
    const auto levels = stats.measure(first, end, read);
    if (!levels) {
        return std::nullopt;
    }
    readouts.invalid = levels->invalid;
    const std::size_t finite = end - first - levels->invalid;
    if (finite > 0) {
        readouts.peak = levels->peak;
        readouts.rms =
            static_cast<float>(std::sqrt(levels->sumSquares / static_cast<double>(finite)));
    }
    return readouts;
}

// Whether the Meter shows: the engine then measures its true peak, DC offset, loudness and
// correlation, which nothing else reads; else its levels alone, for the Invalid samples chip and
// for the Meter as it is switched on (FR-7).
bool Analyzer::metered() const
{
    return frameSettings.enumValue<protocol::Toggle>(SettingId::kMeter) == protocol::Toggle::kOn;
}

bool Analyzer::updateMeter(MeterBlocks& meter, const SweepImage& sweep, ChannelView view,
                           int channels, bool fresh, std::size_t block, double sampleRate)
{
    const MeterBlocks::Reader read = meterReader(sweep, channels, view, 0);
    const auto length = static_cast<std::size_t>(sweep.length());
    meterJoins.assign(sweep.joins().begin(), sweep.joins().end());
    std::ranges::sort(meterJoins);
    if (fresh) {
        meter.reset(length, block, sampleRate, std::min(channels, 2), 0,
                    static_cast<std::ptrdiff_t>(length));
        return meter.update(0, length, meterJoins, read);
    }
    bool measured = true;
    for (const SweepImage::Range& range : sweep.changes()) {
        measured = meter.update(static_cast<std::size_t>(range.first),
                                static_cast<std::size_t>(range.end), meterJoins, read) &&
                   measured;
    }
    return measured;
}

MeterBlocks::Reader Analyzer::meterReader(const SampleSource& source, int channels,
                                          ChannelView view, std::int64_t origin)
{
    return [this, &source, channels, view, origin](std::ptrdiff_t first, std::span<float> into,
                                                   std::span<float> left, std::span<float> right) {
        const std::int64_t at = origin + first;
        if (at < 0) {
            return false;
        }
        const auto from = static_cast<std::uint64_t>(at);
        readScratch.resize(std::max({readScratch.size(), into.size(), left.size()}));
        // Each as asked for: the readout channel alone while the levels alone are measured, Left
        // and Right alone where only the K-weighting is.
        return (into.empty() || readSamples(source, channels, view, from, into, readScratch)) &&
               (left.empty() ||
                readSamples(source, channels, ChannelView::kLeft, from, left, readScratch)) &&
               (right.empty() ||
                readSamples(source, channels, ChannelView::kRight, from, right, readScratch));
    };
}

std::optional<Readouts> Analyzer::measureWindow(const SampleSource& source, int channels,
                                                ChannelView view, std::uint64_t first,
                                                std::uint64_t end, double sampleRate)
{
    const std::uint64_t newest = source.endIndex();
    if (first >= end) {
        return Readouts{};
    }
    if (end > newest) {
        return std::nullopt;
    }
    const auto count = static_cast<std::size_t>(end - first);
    if (!metered()) {
        // The Meter is off: the visible part's levels, from its own samples.
        streamMeter.clear();
        spotMeter.clear();
        meterView.resize(count);
        readScratch.resize(std::max(readScratch.size(), count));
        if (!readSamples(source, channels, view, first, meterView, readScratch)) {
            return std::nullopt;
        }
        const BlockStats::Block levels = measureBlock(meterView);
        Readouts readouts;
        readouts.invalid = levels.invalid;
        if (count > levels.invalid) {
            readouts.peak = levels.peak;
            readouts.rms = static_cast<float>(
                std::sqrt(levels.sumSquares / static_cast<double>(count - levels.invalid)));
        }
        return readouts;
    }
    // The Meter is on: its statistics per block of this track's audio, which follow it, so that a
    // readout measures the audio since the last one and the samples at the view's edges.
    const std::size_t block = statsBlock(sampleRate);
    const int measuredChannels = channels > 1 ? 2 : 1;
    const std::uint64_t held = source.beginIndex();
    const auto settle = static_cast<std::uint64_t>(std::llround(kMeterSettleSeconds * sampleRate));
    const auto keep = static_cast<std::uint64_t>(std::llround(kMeterKeepSeconds * sampleRate));
    const auto slack =
        static_cast<std::uint64_t>(std::llround(protocol::kPacingDelayCapSeconds * sampleRate));
    const std::uint64_t reach = newest - first;
    if (reach > keep + slack) {
        // A trace held from before what the blocks keep (Normal's, while no trigger comes): from
        // statistics of its own, over it alone, the filters settled on the audio before it.
        const std::uint64_t readable = first > held ? std::min(first - held, settle) : 0;
        const std::uint64_t after = std::min<std::uint64_t>(newest - end, TruePeak::kAfter + 1);
        const MeterBlocks::Reader read =
            meterReader(source, channels, view, static_cast<std::int64_t>(first));
        spotMeter.reset(count, block, sampleRate, measuredChannels,
                        -static_cast<std::ptrdiff_t>(readable),
                        static_cast<std::ptrdiff_t>(count + after));
        if (!spotMeter.prime(static_cast<std::size_t>(readable), read) ||
            !spotMeter.update(0, count, {}, read)) {
            return std::nullopt;
        }
        return spotMeter.measure(0, count, {}, read);
    }
    // The oldest audio kept: what a view can show or be panned back to, and the start of a window
    // placed a moment ago, since when the audio thread wrote on.
    const std::uint64_t wanted = std::max(keep, reach);
    const std::uint64_t oldest = std::max(held, newest > wanted ? newest - wanted : 0);
    const auto readableBefore = [&](std::uint64_t origin, std::uint64_t most) {
        return -static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(origin - held, most));
    };
    bool followed = false;
    if (streamMeter.fits(streamMeter.sampleCount(), block, sampleRate, measuredChannels) &&
        streamView == view && streamChannels == channels &&
        oldest < streamOrigin + streamMeter.sampleCount()) {
        // Whole blocks before the oldest audio kept go; one more where the first would start
        // before what the source holds, whose samples could not be read.
        std::uint64_t dropped = oldest > streamOrigin ? (oldest - streamOrigin) / block : 0;
        if (streamOrigin + (dropped * block) < held) {
            ++dropped;
        }
        const std::uint64_t origin = streamOrigin + (dropped * block);
        if (origin <= first && origin < streamOrigin + streamMeter.sampleCount()) {
            const auto readableEnd = static_cast<std::ptrdiff_t>(newest - origin);
            followed = streamMeter.follow(
                static_cast<std::size_t>(dropped), static_cast<std::size_t>(newest - origin),
                readableBefore(origin, TruePeak::kBefore), readableEnd,
                meterReader(source, channels, view, static_cast<std::int64_t>(origin)));
            streamOrigin = origin;
        }
    }
    if (!followed) {
        // Built afresh from the oldest audio kept, the filters settled on the audio before it.
        const auto length = static_cast<std::size_t>(newest - oldest);
        const std::ptrdiff_t before = readableBefore(oldest, settle);
        const MeterBlocks::Reader read =
            meterReader(source, channels, view, static_cast<std::int64_t>(oldest));
        streamMeter.reset(length, block, sampleRate, measuredChannels, before,
                          static_cast<std::ptrdiff_t>(length));
        streamOrigin = oldest;
        streamView = view;
        streamChannels = channels;
        if (!streamMeter.prime(static_cast<std::size_t>(-before), read) ||
            !streamMeter.update(0, length, {}, read)) {
            streamMeter.clear();
            return std::nullopt;
        }
    }
    return streamMeter.measure(
        static_cast<std::size_t>(first - streamOrigin),
        static_cast<std::size_t>(end - streamOrigin), {},
        meterReader(source, channels, view, static_cast<std::int64_t>(streamOrigin)));
}

// The tracks of the multi-track view: this instance's own and the others it shows, in the host's
// order, at most kMaxShownTracks (the Compare view adds one lane, their sum). Publishes their
// labels and the list of every instance, keeps each lane's state across frames, and holds each
// lane's audio while frozen.
// Returns the own lane's serial.
std::uint64_t Analyzer::selectLanes(const Settings& settings)
{
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    std::vector<Member> members = registry.members();
    auto own = std::ranges::find_if(
        members, [&](const Member& member) { return member.engine.get() == &engine; });
    if (own == members.end()) {
        // Not registered (a host without the registry entry): show this track alone. The
        // analyzer never outlives its engine, so the reference needs no ownership.
        members.insert(members.begin(),
                       {.engine = std::shared_ptr<Engine>(std::shared_ptr<Engine>(), &engine),
                        .track = engine.trackInfo(),
                        .serial = 0});
        own = members.begin();
    }
    const std::uint64_t ownSerial = own->serial;

    // The tracks shown: this one, and the others asked for, in the host's order. An instance
    // that went away leaves the selection, so that one the host brings back (a new instance, with
    // a new id) is not shown unasked. At most kMaxShownTracks: the editor asks for no more, and a
    // selection that has more (asked by an older page) is cut to the first ones.
    std::vector<std::uint64_t> live;
    live.reserve(members.size());
    for (const Member& member : members) {
        live.push_back(member.serial);
    }
    engine.keepShown(live);
    const std::vector<std::uint64_t> asked = engine.shown();
    std::vector<TrackEntry> tracks;
    tracks.reserve(members.size());
    std::size_t others = 0;
    for (const Member& member : members) {
        const bool isOwn = member.serial == ownSerial;
        const bool shown =
            isOwn || (contains(asked, member.serial) && others < kMaxShownTracks - 1);
        if (shown && !isOwn) {
            ++others;
        }
        tracks.push_back({.label = {.id = member.serial,
                                    .name = member.track.name,
                                    .colour = member.track.colour,
                                    .own = isOwn,
                                    .instance = member.instance},
                          .shown = shown});
    }
    std::vector<LaneLabel> labels;
    labels.reserve(others + 1);
    for (const TrackEntry& track : tracks) {
        if (track.shown) {
            labels.push_back(track.label);
        }
    }
    // Compare takes every track shown, this one as the reference; with no other there is nothing
    // to compare.
    comparing = compareShown(settings) && labels.size() > 1;
    publishLanes(std::move(labels), tracks);

    // Lanes there were before this call (none outside the multi-track view, which frees them):
    // only then is a new lane a track shown while this view was frozen.
    const bool hadLanes = !lanes.empty();
    std::map<std::uint64_t, std::unique_ptr<Lane>> next;
    laneOrder.clear();
    for (std::size_t index = 0; index < members.size(); ++index) {
        Member& member = members[index];
        if (!tracks[index].shown) {
            continue;
        }
        const auto found = lanes.find(member.serial);
        // This track's image holds its captured channels, which the Meter reads Left and Right
        // from: both of a stereo track, whether or not the Meter shows, so that switching it on
        // reads the sweep as drawn (an image built for it then would hold only what the capture
        // still has); a mono track's one, as its held audio has no other. A lane whose image
        // holds another count (the track's channels changed) starts over, once live: frozen, it
        // keeps showing the audio held.
        const int ownChannels = member.serial == ownSerial && member.engine->inputChannels() > 1
                                    ? CaptureRing::kMaxChannels
                                    : 1;
        const bool isNew =
            found == lanes.end() || (!isFrozen && found->second->image.channels() != ownChannels);
        auto lane = isNew ? std::make_unique<Lane>(ownChannels) : std::move(found->second);
        lane->engine = std::move(member.engine);
        const CaptureRing& ring = lane->engine->captureRing();
        if (!isFrozen) {
            lane->heldNothing = false;
        } else if (isNew && hadLanes && member.serial != ownSerial) {
            // Shown while frozen: its audio was not held at the freeze, and what it has now is
            // from later, so its lane stays empty until the display resumes (FR-6).
            lane->heldNothing = true;
        }
        if (isFrozen && !lane->hold.held() && !lane->heldNothing) {
            // This track's holds every channel, so that a channel chosen while frozen is drawn
            // from it, and its lane's image, which holds both channels of a stereo track, can be
            // built from it: in Beats, and in Trigger and Free too (where its lane reads the
            // HoldBuffer), for Beats chosen while frozen. Another track's holds its lane channel
            // alone.
            const bool allChannels = member.serial == ownSerial;
            // A live Beats lane is held where it was placed at the paced playhead (each step
            // places it), as Single's sweep is (freeze); not paced, to its newest audio.
            const std::optional<std::uint64_t> placed = synced && lane->window && pacedEdge.paced()
                                                            ? std::optional(lane->end)
                                                            : std::nullopt;
            if (lane->hold.capture(*lane->engine, laneView(*lane, laneChannel(settings)),
                                   frozenLength(ring, lane->engine->sampleRate(), synced, true),
                                   scratch, allChannels, placed) &&
                synced) {
                lane->hold.placeBy(
                    clockAt(lane->engine->timeline(), lane->hold.endIndex(), lane->hold.clock()));
                catchUpToHold(*lane);
            }
        } else if (!isFrozen && lane->hold.held()) {
            lane->hold.release();
        }
        laneOrder.push_back(member.serial);
        next.insert_or_assign(member.serial, std::move(lane));
    }
    lanes = std::move(next);
    ownLane = ownSerial;
    return ownSerial;
}

ChannelView Analyzer::triggerViewFor(const Settings& settings, bool multi) const
{
    // The multi-track view shows each track's lane channel, and triggers on this track's.
    if (multi) {
        return ownLaneView(settings);
    }
    return effectiveTriggerSource(
        settings.enumValue<ChannelView>(SettingId::kTriggerSource),
        effectiveViews(static_cast<std::uint8_t>(settings.value(SettingId::kChannelViews)),
                       engine.inputChannels()));
}

ChannelView Analyzer::laneChannel(const Settings& settings) const
{
    return settings.enumValue<ChannelView>(SettingId::kLaneChannel);
}

ChannelView Analyzer::ownLaneView(const Settings& settings) const
{
    return engine.inputChannels() < 2 ? ChannelView::kLeft : laneChannel(settings);
}

ChannelView Analyzer::laneView(const Lane& lane, ChannelView channel)
{
    return lane.engine->inputChannels() < 2 ? ChannelView::kLeft : channel;
}

// Live, the channel count and sample rate of this track and of each lane shown, against those
// of the last frame drawn live: a track turning mono, or a new sample rate with the same capture
// size, changes what a frame shows though no audio is new and no setting changed.
bool Analyzer::formatsChanged(bool multi)
{
    liveFormats.clear();
    const auto add = [&](const Engine& source) {
        liveFormats.push_back(static_cast<std::uint64_t>(source.inputChannels()));
        liveFormats.push_back(std::bit_cast<std::uint64_t>(source.sampleRate()));
    };
    add(engine);
    if (multi) {
        for (const std::uint64_t serial : laneOrder) {
            add(*lanes.at(serial)->engine);
        }
    }
    return liveFormats != lastLiveFormats;
}

// Whether anything that decides a frame, other than new audio, differs from the last frame's, and
// recording them once a frame is made: one list for every mode and time base, read and written in
// one place, so that a new input is never missed in one of them.
void Analyzer::rememberInputs(const Settings& settings, const Commands& command,
                              std::uint32_t laneSetId)
{
    lastSettings = settings;
    lastColumns = command.columns;
    lastPan = command.panSeconds;
    lastViewStart = command.viewStart;
    lastViewEnd = command.viewEnd;
    lastFrozen = isFrozen;
    lastLaneSet = laneSetId;
    lastPreviewFix = command.previewFix;
    lastRedraws = command.redraws;
}

bool Analyzer::inputsChanged(const Settings& settings, const Commands& command,
                             std::uint32_t laneSetId) const
{
    return settings != lastSettings || command.columns != lastColumns ||
           command.panSeconds != lastPan || isFrozen != lastFrozen ||
           command.viewStart != lastViewStart || command.viewEnd != lastViewEnd ||
           laneSetId != lastLaneSet || command.previewFix != lastPreviewFix ||
           command.redraws != lastRedraws;
}

bool Analyzer::placeLanes(const Settings& settings, protocol::BeatSpan span)
{
    placedSpan = span;
    bool fresh = false;
    const ChannelView channel = laneChannel(settings);
    // The own lane first: the others take its length.
    std::vector<std::uint64_t> order{ownLane};
    std::ranges::copy_if(laneOrder, std::back_inserter(order),
                         [&](std::uint64_t serial) { return serial != ownLane; });
    for (const std::uint64_t serial : order) {
        Lane& lane = *lanes.at(serial);
        const CaptureRing& ring = lane.engine->captureRing();
        const std::uint64_t end =
            serial == ownLane ? ownSweepEdge(span) : laneSweepEdge(lane, *lanes.at(ownLane));
        if (end != lane.end || !lane.window || settings != lastSettings) {
            const double rate = lane.engine->sampleRate();
            const auto placeBefore = [&](const MusicalStamp& previous, std::uint64_t until) {
                if (const auto before =
                        sweepWindow(previous, until, rate, span, kMaxSweepSamples)) {
                    ++workDone.sweepUpdates;
                    (void)lane.image.update(ring, *before, false, lane.engine->inputChannels(),
                                            sharedLength(serial, *before), laneView(lane, channel));
                }
            };
            const auto sweep =
                sweepWindow(placeThroughChanges(lane.engine->timeline(),
                                                std::max(lane.image.placedTo(), ring.beginIndex()),
                                                end, lane.engine->timeline().latest(), placeBefore),
                            end, rate, span, kMaxSweepSamples);
            ++workDone.sweepUpdates;
            const std::uint64_t length = sweep ? sharedLength(serial, *sweep) : 0;
            // Rebuilt only to take the shared length; otherwise the image keeps its own through
            // small changes.
            const bool adopt =
                sweep && length != sweep->sampleCount && lane.image.length() != length;
            if (sweep && lane.image.update(ring, *sweep, adopt, lane.engine->inputChannels(),
                                           length, laneView(lane, channel))) {
                lane.window = sweep;
                fresh = true;
            }
            lane.end = end;
        }
    }
    return fresh;
}

// Where this track's live sweep is placed to: its paced edge, which moves evenly between blocks a
// delay behind the newest audio (the newest sample while pacing does not apply).
std::uint64_t Analyzer::ownSweepEdge(protocol::BeatSpan span)
{
    pacedEdge.observe(engine.blockStamp().latest(), engine.sampleRate());
    const std::uint64_t newest = engine.captureRing().endIndex();
    // A sweep shorter than pacedSpanMinimumSeconds is new across its width every frame: shown at
    // the newest audio, as a short Seconds span is.
    const MusicalStamp stamp = engine.timeline().latest();
    const double seconds =
        beatSpanQuarterNotes(span, Timeline::barQuarterNotes(stamp)) * 60.0 / stamp.tempo;
    if (seconds < protocol::kPacedSpanMinimumSeconds) {
        return pacedEdge.showNewest(clockNanos(), newest);
    }
    return pacedEdge.edge(clockNanos(), newest, protocol::kPacingDelayCapSeconds);
}

// Where another track's lane is placed to: where this track's playhead stands, in the lane's
// capture, so the lanes' playheads move together though each track's blocks arrive at their own
// moments; never past the lane's audio, nor back. The two captures started at different moments,
// so the same audio has indices a constant apart, measured from the tracks' musical stamps
// (AD-15). A measurement more than a millisecond off means one track has seen a transport jump
// the other has not yet: it is taken only when it lasts (the offset itself changed, as when a
// track's processing restarted). Any rates: this track's indices are scaled to the lane's. The
// lane's newest sample while this track's blocks have stopped, or until both have played with a
// position.
std::uint64_t Analyzer::laneSweepEdge(Lane& lane, const Lane& own)
{
    constexpr double kOffsetToleranceSeconds = 0.001;
    // Placements, each as audio arrives and for each frame: about 0.6 s with 512-sample blocks at
    // 48 kHz, longer with longer blocks.
    constexpr int kOffsetPersistence = 100;
    const std::uint64_t newest = lane.engine->captureRing().endIndex();
    const double rate = lane.engine->sampleRate();
    const MusicalStamp ownLatest = own.engine->timeline().latest();
    const MusicalStamp laneLatest = lane.engine->timeline().latest();
    // Each lane follows its own newest audio while this track's blocks have stopped (its playhead
    // then stands still: nothing to map from; before its clock locks, or while a short sweep is
    // unpaced, its playhead is its newest audio, which maps as well), and until both have played
    // with a host position: before that, each timeline runs on from where it started, so
    // positions do not say which audio was played together. After it, also while stopped, every
    // clock runs on from the same last position (AD-15).
    if (pacedEdge.stalled(clockNanos()) || !ownLatest.anchored || !laneLatest.anchored) {
        lane.indexOffset.reset();
        return newest;
    }
    // At any rates (a track in an oversampled FX chain runs at a multiple of the others'): this
    // track's indices count at its rate, so they are scaled to the lane's before the offset,
    // which is in the lane's samples, is added.
    const double ownRate = own.engine->sampleRate();
    const double scale = rate / ownRate;
    const std::int64_t measured =
        static_cast<std::int64_t>(laneLatest.sampleIndex) -
        std::llround(Timeline::sampleIndexAt(ownLatest, laneLatest.quarterNotes, ownRate) * scale);
    if (!lane.indexOffset ||
        std::llabs(measured - *lane.indexOffset) <= std::llround(kOffsetToleranceSeconds * rate) ||
        ++lane.offsetDisagreements > kOffsetPersistence) {
        lane.indexOffset = measured;
        lane.offsetDisagreements = 0;
    }
    const auto mapped = std::llround(static_cast<double>(own.end) * scale) + *lane.indexOffset;
    // A lane whose audio has not arrived as far as this track's playhead (its blocks come later)
    // stops at its newest sample; the delay rises to cover it, and stays where the lane needs it,
    // so the lanes stay together. Not for a track whose blocks stopped (suspended or bypassed): it
    // would hold every lane back by the largest delay.
    const auto laneBlock = lane.engine->blockStamp().latest();
    if (laneBlock && !pacedEdge.stopped(laneBlock->nanos, clockNanos())) {
        // The shortfall in this track's samples, which the delay counts (negative: to spare).
        pacedEdge.coverShortfall(static_cast<double>(mapped - static_cast<std::int64_t>(newest)) /
                                 scale);
    }
    const auto edge = static_cast<std::uint64_t>(std::max<std::int64_t>(mapped, 0));
    return std::clamp(edge, std::min(lane.end, newest), newest);
}

// Positions come from the timeline's records, made where the audio thread saw them, so audio
// placed a delay behind the newest is placed by the position it was played at, whatever this
// sweep was doing when it arrived (frozen, in Trigger or Free, in the other display mode): a
// transport jump (a loop, a relocation, stop and start) moves the sweep only when the playhead
// reaches it, and never moves audio played before it (FR-9.2). Callers start no earlier than the
// source's oldest sample: a fresh image would otherwise replay every record kept, for audio long
// gone.
MusicalStamp Analyzer::placeThroughChanges(
    const Timeline& timeline, std::uint64_t from, std::uint64_t to, const MusicalStamp& fallback,
    const std::function<void(const MusicalStamp&, std::uint64_t)>& placeBefore)
{
    const std::size_t count = timeline.positionsSince(from, positions);
    if (count == 0) {
        return fallback;
    }
    std::size_t current = 0;
    for (std::size_t next = 1; next < count && positions[next].sampleIndex <= to; ++next) {
        if (positions[next].sampleIndex > from) {
            placeBefore(positions[current], positions[next].sampleIndex);
        }
        current = next;
    }
    return positions[current];
}

MusicalStamp Analyzer::clockAt(const Timeline& timeline, std::uint64_t end,
                               const MusicalStamp& fallback)
{
    // The one in effect at `end` comes first, then each recorded after it. With none after, the
    // newest position places the hold as well (and as before, to the last rounding).
    return timeline.positionsSince(end, positions) >= 2 ? positions[0] : fallback;
}

// Lanes are drawn on the own lane's grid, so their sweeps must hold as many samples. A tempo
// change reaches each track's clock at a slightly different moment, and each image keeps its
// length through small changes (see sweepLengthsSimilar), so on their own the lengths would part
// and stay apart. A lane's image takes the own image's length when its sweep is within that
// tolerance of it, and the sweep's own length otherwise.
std::uint64_t Analyzer::sharedLength(std::uint64_t serial, const SweepWindow& sweep) const
{
    const auto found = lanes.find(ownLane);
    if (serial == ownLane || found == lanes.end()) {
        return sweep.sampleCount;
    }
    // Within a sample too: a short sweep's image keeps no drift of its own, but its lane would
    // stay blank for being one sample off this track's.
    const std::uint64_t ownLength = found->second->image.length();
    const std::uint64_t apart =
        std::max(sweep.sampleCount, ownLength) - std::min(sweep.sampleCount, ownLength);
    return ownLength != 0 && (apart <= 1 || sweepLengthsSimilar(sweep.sampleCount, ownLength))
               ? ownLength
               : sweep.sampleCount;
}

void Analyzer::advance()
{
    // The block clock sees every block's stamp it can, also between frames.
    pacedEdge.observe(engine.blockStamp().latest(), engine.sampleRate());
    const Settings settings = engine.settings().get();
    if (resumeAt) {
        // Unfreezes through setFrozen, whose command the next frame takes.
        Commands untaken;
        resume(settings, untaken);
    }
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    if (isFrozen || !synced) {
        return; // Frozen sweeps stay as they are; windows are placed when a frame is made.
    }
    const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
    const bool multi = settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) !=
                       protocol::DisplayMode::kSingle;
    if (multi) {
        selectLanes(settings);
    }
    // Between frames, the sweeps are placed only as audio arrives, so none leaves the capture
    // unplaced. The paced edge moves on every step, and each frame places them up to it.
    std::uint64_t captured = engine.captureRing().endIndex();
    if (multi) {
        captured = 0;
        for (const auto& [serial, lane] : lanes) {
            captured += lane->engine->captureRing().endIndex();
        }
    }
    if (std::exchange(advancedCaptured, captured) == captured) {
        return;
    }
    if (multi) {
        lanesPlaced = placeLanes(settings, beatSpan) || lanesPlaced;
        return;
    }
    const CaptureRing& ring = engine.captureRing();
    const std::uint64_t end = ownSweepEdge(beatSpan);
    const auto placeBefore = [&](const MusicalStamp& previous, std::uint64_t until) {
        if (const auto before =
                sweepWindow(previous, until, engine.sampleRate(), beatSpan, kMaxSweepSamples)) {
            ++workDone.sweepUpdates;
            (void)image.update(ring, *before, false, engine.inputChannels());
        }
    };
    const auto sweep = sweepWindow(
        placeThroughChanges(engine.timeline(), std::max(image.placedTo(), ring.beginIndex()), end,
                            engine.timeline().latest(), placeBefore),
        end, engine.sampleRate(), beatSpan, kMaxSweepSamples);
    ++workDone.sweepUpdates;
    if (sweep && image.update(ring, *sweep, false, engine.inputChannels())) {
        imageWindow = sweep;
        placedSpan = beatSpan;
    }
}

// The multi-track view in Beats mode: one lane per track shown, each the track's lane
// channel as a persistent sweep on its own musical clock, so that lanes line up on bar lines. While
// frozen the lanes are not updated, so all of them hold the same moment.
bool Analyzer::produceLanes(const Settings& settings, const Commands& command,
                            std::vector<std::byte>& frame)
{
    const std::uint64_t ownSerial = selectLanes(settings);
    const std::uint32_t laneSetId = laneSet().id;

    if (image.length() > 0) {
        image.clear(); // Single mode's sweep is not needed here; free its memory.
        imageWindow.reset();
    }
    const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
    const Lane& own = *lanes.at(ownSerial);
    const ChannelView channel = laneChannel(settings);

    // Live, each lane follows its own capture and clock, and only new audio is placed. Frozen,
    // the lanes stay as they are; the own lane decides, as in Single mode, whether a span is
    // cropped from them or rebuilt from each lane's held audio.
    bool fresh = false;
    std::optional<SweepWindow> shown;
    double subStart = 0.0;
    double subEnd = 1.0;
    const bool placedBefore = std::exchange(lanesPlaced, false);
    if (!isFrozen) {
        fresh = placeLanes(settings, beatSpan) || placedBefore;
        shown = own.window;
    } else if (own.hold.held()) {
        // A channel chosen while frozen: each lane is drawn again from its hold, this track's
        // first, where the hold shows it, over the window it was drawn with; another track's
        // lane held in another channel is empty, and drawn again if that channel comes back.
        std::vector<std::uint64_t> byOwn{ownSerial};
        std::ranges::copy_if(laneOrder, std::back_inserter(byOwn),
                             [&](std::uint64_t serial) { return serial != ownSerial; });
        const std::uint64_t ownLength = own.image.length();
        for (const std::uint64_t serial : byOwn) {
            Lane& lane = *lanes.at(serial);
            const ChannelView view = laneView(lane, channel);
            // A lane that stays, as a mono track's, is drawn again too when this track's was
            // redrawn at another length, as after a tempo change too small to resize the sweep.
            const bool resized = serial != ownSerial && own.image.length() != ownLength &&
                                 lane.image.length() > 0 && lane.window &&
                                 lane.image.length() != sharedLength(serial, *lane.window);
            if (!lane.window || (lane.image.view() == view && !resized)) {
                continue;
            }
            if (!lane.hold.held() || !lane.hold.shows(view)) {
                // Emptied once: an empty image holds no view, and stays empty.
                if (lane.image.length() > 0) {
                    lane.image.clear();
                    fresh = true;
                }
            } else {
                (void)lane.image.update(lane.hold, *lane.window, true, lane.hold.channels(),
                                        sharedLength(serial, *lane.window), view);
                fresh = true;
            }
        }
        const auto frozen =
            frozenSweep(own.hold.clock(), own.hold.endIndex(), own.hold.sampleRate(), beatSpan,
                        own.window, own.image.length());
        if (frozen && frozen->rebuild) {
            std::vector<std::uint64_t> order{ownSerial};
            std::ranges::copy_if(laneOrder, std::back_inserter(order),
                                 [&](std::uint64_t serial) { return serial != ownSerial; });
            for (const std::uint64_t serial : order) {
                Lane& lane = *lanes.at(serial);
                const LaneHold& laneHold = lane.hold;
                const auto held =
                    laneHold.held() ? sweepWindow(laneHold.clock(), laneHold.endIndex(),
                                                  laneHold.sampleRate(), beatSpan, kMaxSweepSamples)
                                    : std::nullopt;
                const ChannelView view = laneView(lane, channel);
                if (held && !laneHold.shows(view)) {
                    // Held in another channel: empty until the display resumes, on this span.
                    lane.image.clear();
                    lane.window = held;
                    fresh = true;
                } else if (held && lane.image.update(laneHold, *held, true, laneHold.channels(),
                                                     sharedLength(serial, *held), view)) {
                    lane.window = held;
                    fresh = true;
                }
            }
            shown = own.window;
        } else if (frozen) {
            shown = frozen->window;
            subStart = frozen->start;
            subEnd = frozen->end;
            fresh = showFrozenParts(beatSpan, subStart, subEnd);
        }
    } else {
        shown = own.window;
    }
    if (!shown) {
        return false;
    }

    // Checked whatever else changed, so that the formats drawn are always the ones remembered.
    const bool newFormat = !isFrozen && formatsChanged(true);
    const bool inputs = fresh || newFormat || inputsChanged(settings, command, laneSetId);
    resultsOnly = !inputs;
    bool changed = inputs || (comparing && matchesPending && !searchingSerial);
    // A search running over frames is stepped without drawing until it ends (see produce); sums
    // still being recomputed go on in the frames drawn between searches.
    if (!changed && comparing) {
        changed = searchingSerial ? stepSpreadWork(engine.sampleRate()) : sumsRefreshing();
    }
    if (!changed) {
        return false;
    }

    // Lanes share the own track's window and length (see sharedLength). A lane whose sweep is
    // further off (its own tempo or span differs, or frozen, it was held at another rate) is drawn
    // silent until it matches.
    const SweepWindow& window = *shown;
    const std::uint64_t count = own.image.length();
    const double shownPart = subEnd - subStart;
    const std::size_t columns = command.columns;
    // Neighbours past the sweep's ends come from round its wrap.
    const auto [crop, factor] =
        viewCropOf(static_cast<double>(count), 0.0, subStart + command.viewStart * shownPart,
                   subStart + command.viewEnd * shownPart, columns, true);
    // The rate of the own lane's audio: frozen, the one it was held at.
    const double sampleRate =
        isFrozen && own.hold.held() ? own.hold.sampleRate() : engine.sampleRate();
    const std::size_t block = statsBlock(sampleRate);
    frameData.clear();
    FrameHeaderValues header{
        .flags = static_cast<std::uint8_t>(flag(FrameFlag::kSynced) |
                                           (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                           (window.limited ? flag(FrameFlag::kLimited) : 0U)),
        .laneCount = static_cast<std::uint8_t>(laneOrder.size()),
        .startFraction = static_cast<float>(crop.startFraction - static_cast<double>(crop.lead)),
        .cursor = window.cursor,
        .cycleStart = window.cycleStart,
        .spanQuarterNotes = static_cast<float>(window.spanQuarterNotes),
        .barQuarterNotes = static_cast<float>(window.barQuarterNotes),
        .cycleSeconds = window.cycleSeconds,
        .viewStart = command.viewStart,
        .viewEnd = command.viewEnd,
        .visibleSamples = static_cast<float>(crop.visibleSamples),
        .laneSet = laneSetId,
        .sampleRate = static_cast<float>(sampleRate)};
    // This track's lane names what its readouts measure and what the trigger would read.
    header.readoutView = ownLaneView(settings);
    header.triggerView = header.readoutView;
    // Each lane keeps its columns from frame to frame; only those holding samples placed since
    // the last frame are reduced again.
    const std::size_t visibleCount = crop.end - crop.first;
    // Each lane's image holds its own joins (the newest pass's end, a transport jump's).
    lanesSeams.resize(laneOrder.size());
    std::size_t pointCount = 0;
    bool partial = false;
    frameChanges.clear();
    ++lanesFrame;
    // The playhead, which an aligned lane and the difference are empty around (blankUnheld). An
    // aligned lane is drawn from where this track's audio changed too: that moves the playhead.
    sweepPlayhead = static_cast<std::size_t>(
        std::llround((subStart + window.cursor * shownPart) * static_cast<double>(count)));
    ownChanges.assign(own.image.changes().begin(), own.image.changes().end());
    // Lanes drawn from another feed than last frame (aligned, or no longer) are drawn again over
    // the next frames, sharing a frame's budget with each other and the sums.
    feedShares = 0;
    Reach feedReach;
    for (std::size_t laneIndex = 0; laneIndex < laneOrder.size(); ++laneIndex) {
        Lane& lane = *lanes.at(laneOrder[laneIndex]);
        const SumPart feed = laneFeed(laneIndex);
        feedKey(feed, feedScratch);
        if (feedScratch != lane.reducedFeed) {
            lane.reducedFeed = feedScratch;
            lane.feedRefresh = {.cursor = 0, .remaining = visibleCount};
        }
        feedShares += lane.feedRefresh.remaining > 0 ? 1 : 0;
        onePart.assign(1, feed);
        const Reach reach = feed.aligned ? reachOf(onePart) : Reach{};
        feedReach = {.before = std::max(feedReach.before, reach.before),
                     .after = std::max(feedReach.after, reach.after)};
    }
    // What this frame's new audio reaches of each track lined up between samples, filtered once
    // for everything drawn and measured from it: with the whole blocks of levels and the whole
    // columns (and their neighbours) read around a change.
    const std::size_t slack =
        std::max(block, columns > 0 ? (visibleCount / columns) + 1 : 0) + kColumnReach;
    for (std::size_t laneIndex = 0; laneIndex < laneOrder.size(); ++laneIndex) {
        lineUpChanges(laneIndex, static_cast<std::size_t>(count), feedReach, slack);
    }
    if (!comparing) {
        sumShares = 0;
    }
    for (std::size_t laneIndex = 0; laneIndex < laneOrder.size(); ++laneIndex) {
        const std::uint64_t serial = laneOrder[laneIndex];
        Lane& lane = *lanes.at(serial);
        ColumnReduction& reduction = lane.reduction;
        const SumPart feed = laneFeed(laneIndex);
        // The lane channel, read from the image (another track's holds it alone; this track's
        // may hold both captured channels); aligned, through its alignment, as the difference
        // reads it.
        const auto read = [&](std::size_t first, std::span<float> destination) {
            workDone.samplesReduced += destination.size();
            const std::int64_t position = blockStart(crop) + static_cast<std::int64_t>(first);
            if (feed.aligned) {
                onePart.assign(1, feed);
                readSumBlock(onePart, static_cast<std::size_t>(count), position, destination);
                return true;
            }
            laneScratch.resize(std::max(laneScratch.size(), destination.size()));
            return readWrapped(
                [&](std::uint64_t from, std::span<float> part) {
                    return readSamples(lane.image,
                                       imageChannels(lane.image, lane.engine->inputChannels()),
                                       channel, from, part, laneScratch);
                },
                count, position, destination);
        };
        // Each lane's image joins its newest pass to the one before at its own newest sample, which
        // can lie a block from this track's playhead, and has its own transport jumps: no wave
        // across any of them. Aligned, they lie where the shift puts them.
        laneSeams.clear();
        addJoins(lane.image, crop, laneSeams, feed.aligned ? feed.offset : 0);
        lanesSeams[laneIndex].assign(laneSeams.begin(), laneSeams.end());
        bool updated = false;
        if (lane.image.length() == count) {
            if (!reduction.fits(visibleCount, columns, crop.lead, crop.trail) ||
                lane.reducedFirst != crop.first) {
                reduction.reset(visibleCount, columns, false, crop.lead, crop.trail);
                lane.reducedFirst = crop.first;
                updated = reduction.update(0, visibleCount, read, laneSeams);
                lane.feedRefresh = {};
            } else {
                updated = true;
                onePart.assign(1, feed);
                const Reach reach = feed.aligned ? reachOf(onePart) : Reach{};
                const auto redo = [&](const SweepImage::Range& range) {
                    forEachInBlock(static_cast<std::int64_t>(range.first) -
                                       static_cast<std::int64_t>(reach.before),
                                   static_cast<std::int64_t>(range.end + reach.after), crop,
                                   visibleCount, count, [&](std::size_t from, std::size_t to) {
                                       updated =
                                           reduction.update(from, to, read, laneSeams) && updated;
                                   });
                };
                // Aligned, also where this track's audio changed, which moves the playhead the
                // lane is empty around: the two mostly cover the same positions, drawn once.
                feedChanges.assign(lane.image.changes().begin(), lane.image.changes().end());
                if (feed.aligned) {
                    feedChanges.insert(feedChanges.end(), ownChanges.begin(), ownChanges.end());
                    mergeRanges(feedChanges);
                }
                std::ranges::for_each(feedChanges, redo);
                continueRefresh(lane.feedRefresh, visibleCount,
                                refreshStep(feed.taps ? 4 : 1, feedShares + sumShares),
                                [&](std::size_t from, std::size_t to) {
                                    updated =
                                        reduction.update(from, to, read, laneSeams) && updated;
                                });
            }
            // The lane's levels per block, for summing gains; this track's, the Meter's statistics.
            updateStats(lane.stats, lane.image, channel,
                        imageChannels(lane.image, lane.engine->inputChannels()),
                        !lane.stats.fits(static_cast<std::size_t>(count), block), block);
            if (&lane != &own) {
                // Another track's lane has no Meter.
            } else if (!metered()) {
                lane.meter.clear();
            } else {
                const int captured = imageChannels(lane.image, lane.engine->inputChannels());
                const int channels = std::min(captured, 2);
                (void)updateMeter(
                    lane.meter, lane.image, channel, captured,
                    !lane.meter.fits(static_cast<std::size_t>(count), block, sampleRate, channels),
                    block, sampleRate);
            }
            header.invalidLanes |= holdsInvalid(lane.stats, crop.first + crop.lead - crop.offset,
                                                crop.end - crop.trail - crop.offset)
                                       ? laneBit(laneIndex)
                                       : 0U;
            partial = partial || !(lane.image.complete() || lane.image.partComplete());
            if (comparing) {
                frameChanges.insert(frameChanges.end(), lane.image.changes().begin(),
                                    lane.image.changes().end());
            }
            lane.image.clearChanges();
        } else if (lane.stats.fits(static_cast<std::size_t>(count), block)) {
            // Emptied, as by a channel chosen while frozen that its hold lacks: its levels go, and
            // the sums that held them are summed again.
            lane.stats = {};
            if (comparing) {
                frameChanges.push_back({.first = 0, .end = count});
            }
        }
        if (!updated) {
            // Not on this sweep's length (yet): silent, and updated afresh once it is.
            reduction.reset(visibleCount, columns, false, crop.lead, crop.trail);
            reduction.invalidate();
            lane.feedRefresh = {};
        }
        header.mode = reduction.mode();
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visibleCount;
        frameData.insert(frameData.end(), reduction.values().begin(), reduction.values().end());
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    mergeRanges(frameChanges);

    if (partial) {
        header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kPartial));
    }
    if (comparing) {
        header.invalidLanes |= sumLaneBit(header.invalidLanes);
        appendComparison(header, crop, count, columns, command.previewFix, sweepPlayhead,
                         sampleRate);
    }
    // Each lane's joins, and the sum's (appendComparison).
    reconstruct(header, header.laneCount, crop, factor, [&](std::size_t block) {
        return block < lanesSeams.size() ? std::span<const std::size_t>(lanesSeams[block])
                                         : std::span<const std::size_t>(sumSeams);
    });

    // Readouts measure the own track's lane channel over the visible part, at most every 100 ms
    // of audio and whenever the zoom changes: from the Meter's statistics per block while it is
    // on, else from the lane's levels per block, which the summing gains read too.
    const std::uint64_t ownEnd = own.engine->captureRing().endIndex();
    const ChannelView ownView = ownLaneView(settings);
    const ReadoutView zoom{.viewStart = command.viewStart, .viewEnd = command.viewEnd};
    const int ownCaptured = imageChannels(own.image, own.engine->inputChannels());
    const auto [first, last] = meterRange(crop, static_cast<std::size_t>(count));
    if (!readoutsDue(ownEnd, ownView, zoom)) {
        // Measured within the last 100 ms of audio, on this view.
    } else if (!metered()) {
        // The own lane's levels, read through the lanes they are kept in.
        BlockStats& stats = lanes.at(ownSerial)->stats;
        if (stats.fits(static_cast<std::size_t>(count), block)) {
            storeReadouts(
                levelsOf(stats, first, last, statsReader(own.image, ownCaptured, ownView)), ownEnd,
                ownView, zoom);
        }
    } else if (MeterBlocks& meter = lanes.at(ownSerial)->meter; meter.fits(
                   static_cast<std::size_t>(count), block, sampleRate, std::min(ownCaptured, 2))) {
        meterJoins.assign(own.image.joins().begin(), own.image.joins().end());
        std::ranges::sort(meterJoins);
        storeReadouts(
            meter.measure(first, last, meterJoins, meterReader(own.image, ownCaptured, ownView, 0)),
            ownEnd, ownView, zoom);
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    // A Beats sweep was shown: a later Seconds frame has no window of its own to go on, and a
    // later freeze no live one. A freeze made in Trigger or Free keeps its window, as in Single
    // mode.
    lastShown = Window{.synced = true};
    if (!isFrozen) {
        lastLive = lastShown;
    }
    if (!isFrozen) {
        lastLiveFormats = liveFormats;
        // For benchmarks: where this track's playhead stood.
        engine.recordEdge(
            {.nanos = clockNanos(), .edge = own.end, .newest = engine.captureRing().endIndex()});
    }
    rememberInputs(settings, command, laneSetId);
    return true;
}

// Single mode's sweep, as the lanes' (see catchUpToHold): drawn up to where the freeze holds the
// audio, on the span it was drawn with.
void Analyzer::catchUpImageToHold()
{
    if (image.length() == 0 || !imageWindow) {
        return;
    }
    // Through the positions not yet reached, as live: audio is placed where it was played.
    const auto placeBefore = [&](const MusicalStamp& previous, std::uint64_t until) {
        if (const auto before =
                sweepWindow(previous, until, frozenRate, placedSpan, kMaxSweepSamples)) {
            ++workDone.sweepUpdates;
            (void)image.update(hold, *before, false, engine.inputChannels(), image.length());
        }
    };
    const auto held = sweepWindow(placeThroughChanges(engine.timeline(),
                                                      std::max(image.placedTo(), hold.beginIndex()),
                                                      frozenEnd, frozenStamp, placeBefore),
                                  frozenEnd, frozenRate, placedSpan, kMaxSweepSamples);
    if (!held) {
        return;
    }
    if (image.update(hold, *held, false, engine.inputChannels(), image.length())) {
        imageWindow = held;
    }
}

// The lanes were last placed a frame before the freeze, which holds the audio up to its moment:
// what the hold has since is placed too, so that a frozen sweep ends, and its playhead stands,
// where the hold does. On the span the lanes were drawn with, also when another is chosen at the
// same moment: it is then cropped or rebuilt from them as drawn.
void Analyzer::catchUpToHold(Lane& lane)
{
    const std::uint64_t length = lane.image.length();
    if (length == 0 || !lane.window) {
        return;
    }
    // Through the positions not yet reached, as live: audio is placed where it was played.
    const double rate = lane.hold.sampleRate();
    const auto placeBefore = [&](const MusicalStamp& previous, std::uint64_t until) {
        if (const auto before = sweepWindow(previous, until, rate, placedSpan, kMaxSweepSamples)) {
            ++workDone.sweepUpdates;
            (void)lane.image.update(lane.hold, *before, false, lane.hold.channels(), length,
                                    laneView(lane, laneChannel(frameSettings)));
        }
    };
    const auto held =
        sweepWindow(placeThroughChanges(lane.engine->timeline(),
                                        std::max(lane.image.placedTo(), lane.hold.beginIndex()),
                                        lane.hold.endIndex(), lane.hold.clock(), placeBefore),
                    lane.hold.endIndex(), rate, placedSpan, kMaxSweepSamples);
    if (!held) {
        return;
    }
    if (lane.image.update(lane.hold, *held, false, lane.hold.channels(), length,
                          laneView(lane, laneChannel(frameSettings)))) {
        lane.window = held;
    }
}

// Frozen, a span cropped from the lanes, over [start, end) of the own lane's image: each lane
// still filling in shows that span rebuilt from its held audio (see showHeldPart); a whole lane
// keeps the crop, with all it drew. True if any lane's part changed.
bool Analyzer::showFrozenParts(protocol::BeatSpan span, double start, double end)
{
    const auto count = static_cast<double>(lanes.at(ownLane)->image.length());
    const auto first = static_cast<std::uint64_t>(std::llround(start * count));
    const auto last = static_cast<std::uint64_t>(std::llround(end * count));
    bool changed = false;
    for (const auto& [serial, lane] : lanes) {
        if (!lane->hold.held() || static_cast<double>(lane->image.length()) != count) {
            changed = changed || lane->image.hasPart();
            lane->image.clearPart();
            continue;
        }
        changed = showHeldPart(lane->image, lane->hold, lane->hold.clock(), lane->hold.endIndex(),
                               lane->hold.sampleRate(), span, lane->hold.channels(),
                               laneView(*lane, laneChannel(frameSettings)), first, last) ||
                  changed;
    }
    return changed;
}

// The sum lane's bit in a frame's `invalidLanes`: set when a compared lane's is, since the sum
// of a sample that is not finite is not finite either.
std::uint32_t Analyzer::sumLaneBit(std::uint32_t laneBits) const
{
    const std::uint32_t tracks = laneBit(laneOrder.size()) - 1U;
    return comparing && (laneBits & tracks) != 0 ? laneBit(laneOrder.size()) : 0U;
}

// The multi-track view in Trigger or Free: every lane shows this track's window (its
// trigger, free-running or frozen window), placed in each other track's capture through the musical
// clock that all instances share with the host.
bool Analyzer::renderLanes(const SampleSource& ownSource, const Window& window,
                           std::uint64_t sampleCount, double sampleRate, const Commands& command,
                           std::vector<std::byte>& frame)
{
    const std::size_t columns = command.columns;
    const double first = std::floor(window.start);
    const auto start = static_cast<std::uint64_t>(std::max(first, 0.0));
    const MusicalStamp ownClock = isFrozen ? frozenStamp : engine.timeline().latest();
    const double startQuarterNotes = Timeline::quarterNotesAt(ownClock, start, sampleRate);
    // Each lane's neighbours past the window's ends come from the audio around it.
    const auto [crop, factor] = viewCropOf(static_cast<double>(sampleCount), window.start - first,
                                           window.cropStart, window.cropEnd, columns, true);
    viewSamples.resize(crop.end - crop.first);
    scratch.resize(std::max(scratch.size(), viewSamples.size()));

    FrameHeaderValues header{
        .flags =
            static_cast<std::uint8_t>((window.triggered ? flag(FrameFlag::kTriggered) : 0U) |
                                      (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                      (window.freeRunning ? flag(FrameFlag::kFreeRunning) : 0U) |
                                      (window.limited ? flag(FrameFlag::kLimited) : 0U)),
        .laneCount = static_cast<std::uint8_t>(laneOrder.size()),
        .startFraction = static_cast<float>(crop.startFraction - static_cast<double>(crop.lead)),
        .viewStart = window.viewStart,
        .viewEnd = window.viewEnd,
        .visibleSamples = static_cast<float>(crop.visibleSamples),
        .laneSet = laneSet().id,
        .panReach = static_cast<float>(window.panReach),
        .sampleRate = static_cast<float>(sampleRate),
        .spanSeconds = window.synced ? 0.0f : static_cast<float>(window.span / sampleRate),
        .triggerPosition = triggerPositionOf(window)};
    header.readoutView = ownLaneView(frameSettings);
    header.triggerView = header.readoutView;
    // Where each lane's part of the window starts in its own capture (or hold), and how many
    // channels it is read with: this track's capture or hold, and a live capture, hold them all;
    // another track's hold holds the lane channel alone.
    const auto sourceOf = [&](std::uint64_t serial) -> LaneSource {
        const Lane& lane = *lanes.at(serial);
        if (serial == ownLane) {
            return {.source = &ownSource, .start = start, .channels = engine.inputChannels()};
        }
        const MusicalStamp clock = isFrozen ? lane.hold.clock() : lane.engine->timeline().latest();
        const SampleSource* source =
            isFrozen ? static_cast<const SampleSource*>(&lane.hold) : &lane.engine->captureRing();
        const int channels = isFrozen ? lane.hold.channels() : lane.engine->inputChannels();
        // Frozen, a lane held at another rate than this track's audio (first held after the host
        // changed it) holds none of this window's samples, and one held in another channel than
        // the one now chosen none of this channel: it is drawn silent from the end of its hold,
        // as a Beats lane on another sweep length is.
        if (isFrozen && (lane.hold.sampleRate() != sampleRate ||
                         !lane.hold.shows(laneView(lane, laneChannel(frameSettings))))) {
            return {.source = source,
                    .start = lane.hold.endIndex(),
                    .channels = channels,
                    .none = true};
        }
        return {
            .source = source,
            .start = static_cast<std::uint64_t>(std::max(
                std::round(Timeline::sampleIndexAt(clock, startQuarterNotes, sampleRate)), 0.0)),
            .channels = channels};
    };
    // A free-running window falls into columns on a grid fixed to sample indices: each lane keeps
    // its columns, on the same grid from where its part of the window starts, and reduces only
    // the new ones.
    const bool rolling =
        window.freeRunning && window.onGrid && window.cropStart == 0.0 && window.cropEnd == 1.0;
    // Where a lane's grid starts, from where its part of the window starts (wrapping arithmetic:
    // the lane's columns are read from this plus the grid's boundaries).
    const auto gridOrigin = [&](std::uint64_t laneStart) {
        return laneStart - window.grid.boundary(window.firstColumn);
    };
    // Columns rolled from another channel are not this one's: all reduced again.
    const ChannelView channel = laneChannel(frameSettings);
    if (std::exchange(rolledLaneChannel, channel) != channel) {
        for (const auto& [serial, lane] : lanes) {
            lane->roll.clear();
        }
        sumRoll.clear();
    }
    if (comparing) {
        laneSources.clear();
        for (const std::uint64_t serial : laneOrder) {
            laneSources.push_back(sourceOf(serial));
        }
        renderCompare(header, window, start, sampleCount, crop, factor, rolling, sampleRate,
                      command, frame);
        return true;
    }
    frameData.clear();
    std::size_t pointCount = 0;
    for (std::size_t index = 0; index < laneOrder.size(); ++index) {
        const std::uint64_t serial = laneOrder[index];
        Lane& lane = *lanes.at(serial);
        const LaneSource laneSource = sourceOf(serial);
        const SampleSource* const source = laneSource.source;
        const std::uint64_t laneStart = laneSource.start;
        const int channels = laneSource.channels;
        if (!lane.rolledFeed.empty()) {
            // Rolled through an alignment, while compared: as recorded now, all of them again.
            lane.roll.clear();
            lane.rolledFeed.clear();
        }
        if (rolling) {
            const auto values = lane.roll.update(
                gridOrigin(laneStart), window.firstColumn, window.grid,
                [&](std::uint64_t first, std::span<float> destination) {
                    workDone.samplesReduced += destination.size();
                    readScratch.resize(std::max(readScratch.size(), destination.size()));
                    return readSamples(*source, channels, channel, first, destination, readScratch);
                });
            header.mode = protocol::FrameMode::kMinMax;
            pointCount = columns;
            frameData.insert(frameData.end(), values.begin(), values.end());
            header.invalidLanes |= lane.roll.holdsInvalid() ? laneBit(index) : 0U;
            continue;
        }
        lane.roll.clear();
        // What the lane holds of the window, silence where it does not (yet, or any more); past
        // the window's ends, its neighbours as held, none where it holds nothing.
        (void)fillBlock(
            *source, channels, channel, static_cast<std::int64_t>(laneStart),
            static_cast<std::int64_t>(sampleCount),
            static_cast<std::int64_t>(crop.first) - static_cast<std::int64_t>(crop.offset),
            viewSamples, scratch,
            [&](std::int64_t from, std::span<float> destination) {
                readScratch.resize(std::max(readScratch.size(), destination.size()));
                (void)readSamplesHeld(*source, channels, channel,
                                      laneStart + static_cast<std::uint64_t>(from), destination,
                                      readScratch);
                return true;
            },
            laneSource.none);
        const std::span<const float> visible = viewSamples;
        header.invalidLanes |= holdsInvalid(shownPart(visible, crop)) ? laneBit(index) : 0U;
        workDone.samplesReduced += visible.size();
        header.mode = reduceToColumns(visible, crop.lead, crop.trail, columns, reduced, kNoSeam,
                                      &columnPeaks);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visible.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    reconstruct(header, laneOrder.size(), crop, factor,
                [](std::size_t) { return std::span<const std::size_t>(); });

    // Readouts measure this track's lane channel over the visible part, at most every 100 ms of
    // audio and whenever the zoom changes.
    const std::uint64_t ownEnd = engine.captureRing().endIndex();
    const ChannelView ownView = ownLaneView(frameSettings);
    const ReadoutView shown{.viewStart = window.viewStart, .viewEnd = window.viewEnd};
    if (readoutsDue(ownEnd, ownView, shown)) {
        const auto [shownFirst, shownEnd] = meterRange(crop, static_cast<std::size_t>(sampleCount));
        storeReadouts(measureWindow(ownSource, engine.inputChannels(), ownView, start + shownFirst,
                                    start + shownEnd, sampleRate),
                      ownEnd, ownView, shown);
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    return true;
}

// The compared track whose match to search next: a track without a match first, else the
// one with the oldest match that is due, provided kMatchSpacingSeconds of audio passed since the
// last search. A change of the tracks compared, the view or the span, or a freeze or its end,
// makes every match due, but keeps it on show until its search; so does panning a frozen view
// (`shownFrom`, where its window starts). Frozen, or with the transport stopped, no audio may
// arrive to space those searches by: they come one after another instead, and the later
// refreshes are spaced again; frozen, the audio is held, so there are none. None while a search
// runs. Notes whether matches remain to search.
std::optional<std::size_t> Analyzer::matchToSearch(std::uint32_t laneSet, std::size_t first,
                                                   std::size_t end, std::size_t length,
                                                   std::uint64_t now, std::uint64_t shownFrom,
                                                   double viewStart, double viewEnd)
{
    // A zoomed live free-running window draws only its visible part, so a pan of the same width
    // changes no crop or length: the view itself says where it is.
    const ChannelView channel = laneChannel(frameSettings);
    // The sum's matches describe the part of the audio shown, so they follow the view. Aligning,
    // a match is the routing's (an effect's latency), searched over audio that does not depend on
    // the view, and stays through any zoom or pan: only the lanes, a freeze, the channel or the
    // kind of search make it due again.
    const bool aligning = differenceShown();
    const bool sameSearch = laneSet == matchLaneSet && isFrozen == matchFrozen &&
                            channel == matchLaneChannel && aligning == matchAligning;
    const bool sameView = sameSearch && first == matchFirst && end == matchEnd &&
                          length == matchCount && viewStart == matchViewStart &&
                          viewEnd == matchViewEnd;
    if (channel != matchLaneChannel) {
        // Matches, alignments and gains measured on another channel are not this one's: none on
        // show until measured on it.
        matchStates.clear();
        alignments.clear();
        comparisonCache.tracks.clear();
        matchLaneChannel = channel;
    }
    // Panned and nothing else: a drag moves the view a little each frame.
    const bool panned = sameView && isFrozen && shownFrom != matchShownFrom;
    panning = false;
    if (!sameView || panned) {
        if (!aligning || !sameSearch) {
            for (auto& [serial, state] : matchStates) {
                state.wait = 0;
                state.failedReads = 0;
            }
            searchAtOnce = isFrozen || !engine.timeline().latest().playing;
            // A search of the audio shown before measures what is no longer shown: forgotten, it
            // is not stepped again, and the next search starts afresh.
            searchingSerial.reset();
        }
        // The gains measure the visible part either way.
        comparisonCache.valid = false;
        matchLaneSet = laneSet;
        matchFirst = first;
        matchEnd = end;
        matchCount = length;
        matchFrozen = isFrozen;
        matchAligning = aligning;
        matchShownFrom = shownFrom;
        matchViewStart = viewStart;
        matchViewEnd = viewEnd;
        gainJob.active = false;
        if (panned) {
            // Nothing begins until the view stays where it is for a frame: while it is dragged,
            // each search and measurement would be ended by the next frame's move.
            panning = true;
            matchesPending = true;
            return std::nullopt;
        }
    }
    const auto gone = [&](const auto& entry) {
        return std::ranges::find(laneOrder, entry.first) == laneOrder.end();
    };
    std::erase_if(matchStates, gone);
    std::erase_if(alignments, gone);
    // One search at a time: frames go on meanwhile, as they do while matches remain to search.
    if (searchingSerial) {
        matchesPending = true;
        return std::nullopt;
    }
    const double sampleRate = engine.sampleRate();
    std::optional<std::size_t> chosen;
    std::uint64_t chosenAt = 0;
    bool missing = false;
    std::size_t candidates = 0;
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        const std::uint64_t serial = laneOrder[lane];
        if (!comparing || serial == ownLane) {
            continue;
        }
        const auto found = matchStates.find(serial);
        if (found == matchStates.end()) {
            ++candidates;
            if (!missing) {
                chosen = lane;
                missing = true;
            }
        } else if (found->second.wait == 0 ||
                   (!isFrozen && (now < found->second.analysedAt ||
                                  now - found->second.analysedAt >= found->second.wait))) {
            // Made due by a change of view, or by the audio played since its search. Frozen,
            // the audio shown is held and does not change: a match found there stays.
            ++candidates;
            if (!missing && (!chosen || found->second.analysedAt < chosenAt)) {
                chosen = lane;
                chosenAt = found->second.analysedAt;
            }
        }
    }
    const auto spacing = static_cast<std::uint64_t>(std::lround(kMatchSpacingSeconds * sampleRate));
    if (!chosen || (!missing && !searchAtOnce && now - lastMatchSearch < spacing)) {
        matchesPending = chosen.has_value();
        searchAtOnce = searchAtOnce && chosen.has_value();
        return std::nullopt;
    }
    matchesPending = candidates > 1;
    searchAtOnce = searchAtOnce && matchesPending;
    lastMatchSearch = now;
    return chosen;
}

void Analyzer::buildParts()
{
    rawParts.clear();
    fixedParts.clear();
    differenceParts.clear();
    if (!comparing) {
        return;
    }
    // Every lane is a part: Compare takes every track shown.
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        const std::uint64_t serial = laneOrder[lane];
        SumPart fixed{.lane = lane};
        if (const auto found = matchStates.find(serial); found != matchStates.end()) {
            fixed.offset = found->second.analysis.offset;
            fixed.gain = found->second.analysis.inverted ? -1.0f : 1.0f;
        }
        rawParts.push_back({.lane = lane});
        fixedParts.push_back(fixed);
        differenceParts.push_back(serial == ownLane ? SumPart{.lane = lane}
                                                    : alignedPart(lane, -1.0f));
    }
}

bool Analyzer::differenceShown() const
{
    return frameSettings.enumValue<protocol::CompareLane>(SettingId::kCompareLane) ==
           protocol::CompareLane::kDifference;
}

const std::vector<Analyzer::SumPart>& Analyzer::shownParts(bool previewFix) const
{
    // Preview fix is the sum's: the difference is drawn as it is.
    return differenceShown() ? differenceParts : previewFix ? fixedParts : rawParts;
}

std::size_t Analyzer::ownPart(const std::vector<SumPart>& parts) const
{
    const auto own = std::ranges::find_if(
        parts, [&](const SumPart& part) { return laneOrder[part.lane] == ownLane; });
    return static_cast<std::size_t>(own - parts.begin());
}

void Analyzer::followLane()
{
    const protocol::CompareLane lane =
        differenceShown() ? protocol::CompareLane::kDifference : protocol::CompareLane::kSum;
    // Align switched: the depth is measured again, the last one on show until it is.
    const bool aligned = differenceShown() && frameSettings.enumValue<protocol::Toggle>(
                                                  SettingId::kAlign) == protocol::Toggle::kOn;
    if (std::exchange(comparisonCache.aligned, aligned) != aligned) {
        comparisonCache.valid = false;
        gainJob.active = false;
    }
    if (comparisonCache.lane == lane) {
        return;
    }
    // The other lane's figures are not this one's: none until they are measured, at once.
    comparisonCache.lane = lane;
    comparisonCache.valid = false;
    comparisonCache.sumGain = std::numeric_limits<float>::quiet_NaN();
    comparisonCache.fixedGain = std::numeric_limits<float>::quiet_NaN();
    comparisonCache.nullDepth = std::numeric_limits<float>::quiet_NaN();
    gainJob.active = false;
}

void Analyzer::packMatches()
{
    std::vector<float>& matchValues = comparisonCache.matches;
    assignNoMatches(matchValues, laneOrder.size());
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        const std::uint64_t serial = laneOrder[lane];
        const auto found = matchStates.find(serial);
        if (!comparing || found == matchStates.end()) {
            continue; // This track has none.
        }
        const PairAnalysis& match = found->second.analysis;
        float* const values = matchValues.data() + lane * protocol::kMatchValues;
        values[0] = match.measured ? static_cast<float>(match.correlation)
                                   : std::numeric_limits<float>::quiet_NaN();
        // A search made to align the tracks gives the delay to a fraction of a sample; one for
        // a fix, the whole samples a fix shifts by.
        const bool aligned = found->second.aligned;
        values[1] = !match.related ? std::numeric_limits<float>::quiet_NaN()
                    : aligned      ? static_cast<float>(match.delay)
                                   : static_cast<float>(match.offset);
        values[2] = !match.related ? 0.0f : match.inverted ? -1.0f : 1.0f;
    }
}

std::pair<std::ptrdiff_t, std::ptrdiff_t> Analyzer::readsOf(const SumPart& part)
{
    // A filter's point is read from the samples 1 - reach to reach around it (weightsAt).
    const auto reach = static_cast<std::ptrdiff_t>(part.taps ? part.taps->size() / 2 : 0);
    return {part.offset + (reach > 0 ? 1 - reach : 0), part.offset + reach};
}

std::pair<std::ptrdiff_t, std::ptrdiff_t> Analyzer::fixedReach(const std::vector<SumPart>& parts)
{
    std::ptrdiff_t earliest = 0;
    std::ptrdiff_t latest = 0;
    for (const SumPart& part : parts) {
        const auto [first, last] = readsOf(part);
        earliest = std::min(earliest, first);
        latest = std::max(latest, last);
    }
    return {earliest, latest};
}

Analyzer::Reach Analyzer::reachOf(const std::vector<SumPart>& parts)
{
    // A sample that changed at position c is read by the positions c - latest to c - earliest;
    // and where a part has no audio around the playhead moves with it, as far.
    const auto [earliest, latest] = fixedReach(parts);
    return {.before = static_cast<std::size_t>(latest),
            .after = static_cast<std::size_t>(-earliest)};
}

Analyzer::SumPart Analyzer::laneFeed(std::size_t lane) const
{
    const std::uint64_t serial = laneOrder[lane];
    if (!comparing || serial == ownLane) {
        return {.lane = lane};
    }
    return alignedPart(lane, 1.0f);
}

void Analyzer::feedKey(const SumPart& feed, std::vector<std::int64_t>& key)
{
    key.clear();
    if (feed.aligned) {
        key.assign({feed.offset, gainKey(feed.gain), gainKey(feed.fraction)});
    }
}

bool Analyzer::sumsRefreshing() const
{
    if (sumRefresh.remaining > 0 || rawSumRefresh.remaining > 0 || fixedSumRefresh.remaining > 0 ||
        differenceRefresh.remaining > 0) {
        return true;
    }
    for (std::size_t index = 0; index < laneOrder.size(); ++index) {
        const Lane& lane = *lanes.at(laneOrder[index]);
        const SumPart feed = laneFeed(index);
        const std::vector<std::int64_t>& drawn = lane.reducedFeed;
        const bool same = feed.aligned ? drawn.size() == 3 && drawn[0] == feed.offset &&
                                             drawn[1] == gainKey(feed.gain) &&
                                             drawn[2] == gainKey(feed.fraction)
                                       : drawn.empty();
        if (lane.feedRefresh.remaining > 0 || !same) {
            return true;
        }
    }
    return false;
}

void Analyzer::blankUnheld(const SumPart& part, std::size_t length, std::size_t position,
                           std::span<float> into) const
{
    const auto [first, last] = readsOf(part);
    const std::ptrdiff_t earliest = std::min<std::ptrdiff_t>(first, 0);
    const std::ptrdiff_t latest = std::max<std::ptrdiff_t>(last, 0);
    const auto size = static_cast<std::ptrdiff_t>(into.size());
    const auto at = static_cast<std::ptrdiff_t>(position);
    const auto end = static_cast<std::ptrdiff_t>(length);
    // Screen positions [from, to), as far as `into` shows them.
    const auto blank = [&](std::ptrdiff_t from, std::ptrdiff_t to) {
        const std::ptrdiff_t begin = std::clamp<std::ptrdiff_t>(from - at, 0, size);
        const std::ptrdiff_t stop = std::clamp<std::ptrdiff_t>(to - at, begin, size);
        std::fill(into.begin() + begin, into.begin() + stop, 0.0f);
    };
    blank(0, -earliest);
    blank(end - latest, end);
    if (sweepPlayhead <= length) {
        // Before the playhead the sweep shows the newest pass, from it on the one before: a
        // position has the part's audio when everything it reads lies on its own side.
        const auto playhead = static_cast<std::ptrdiff_t>(sweepPlayhead);
        blank(playhead - latest, playhead - earliest);
    }
}

void Analyzer::lineUpChanges(std::size_t laneIndex, std::size_t length, Reach reach,
                             std::size_t slack)
{
    Lane& lane = *lanes.at(laneOrder[laneIndex]);
    Lane::Lined& lined = lane.lined;
    lined.frame = 0;
    const SumPart feed = laneFeed(laneIndex);
    if (!feed.taps) {
        return;
    }
    std::uint64_t from = length;
    std::uint64_t to = 0;
    const auto cover = [&](const std::vector<SweepImage::Range>& ranges) {
        for (const SweepImage::Range& range : ranges) {
            from = std::min(from, range.first);
            to = std::max(to, range.end);
        }
    };
    cover(lane.image.changes());
    cover(ownChanges);
    const std::size_t before = reach.before + slack;
    const auto first = static_cast<std::size_t>(from > before ? from - before : 0);
    const auto end =
        static_cast<std::size_t>(std::min<std::uint64_t>(to + reach.after + slack, length));
    if (from >= to || end - first > kLinedSamples) {
        return;
    }
    // As readSum reads it: the samples around each position, silent beyond the sweep.
    const auto [earliest, latest] = readsOf(feed);
    const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(first) + earliest;
    const std::size_t reads = (end - first) + static_cast<std::size_t>(latest - earliest);
    const std::ptrdiff_t begin = std::max<std::ptrdiff_t>(0, -source);
    const std::ptrdiff_t stop = std::min<std::ptrdiff_t>(
        static_cast<std::ptrdiff_t>(reads), static_cast<std::ptrdiff_t>(length) - source);
    lined.values.assign(end - first, 0.0f);
    if (begin < stop) {
        alignScratch.assign(reads, 0.0f);
        const std::span<float> read =
            std::span(alignScratch)
                .subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(stop - begin));
        workDone.samplesSummed += read.size();
        readLaneImage(laneIndex, length, static_cast<std::uint64_t>(source + begin), read);
        addFiltered(*feed.taps, alignScratch.data(), 1.0f, lined.values);
    }
    lined.offset = feed.offset;
    lined.fraction = feed.fraction;
    lined.length = length;
    lined.first = first;
    lined.frame = lanesFrame;
}

Analyzer::Positions Analyzer::addAlignedPart(const LaneSource& lane, std::uint64_t laneStart,
                                             const SumPart& part, std::int64_t position,
                                             std::span<float> into)
{
    if (lane.none) {
        return {};
    }
    const auto [first, last] = readsOf(part);
    const auto size = static_cast<std::int64_t>(into.size());
    // The sample position i reads first, in the lane's own indices, and how many it reads.
    const std::int64_t base = static_cast<std::int64_t>(laneStart) + position + first;
    const std::int64_t reads = last - first;
    const auto oldest = static_cast<std::int64_t>(lane.source->beginIndex());
    const auto newest = static_cast<std::int64_t>(lane.source->endIndex());
    const std::int64_t begin = std::clamp<std::int64_t>(oldest - base, 0, size);
    const std::int64_t stop = std::clamp<std::int64_t>(newest - reads - base, begin, size);
    if (begin >= stop) {
        return {};
    }
    const auto count = static_cast<std::size_t>(stop - begin);
    alignScratch.resize(count + static_cast<std::size_t>(reads));
    readScratch.resize(std::max(readScratch.size(), alignScratch.size()));
    workDone.samplesSummed += alignScratch.size();
    // All of it or none: a capture that moved on over it during the copy has it no more.
    if (!readSamples(*lane.source, lane.channels, laneChannel(frameSettings),
                     static_cast<std::uint64_t>(base + begin), alignScratch, readScratch)) {
        return {};
    }
    const std::span<float> held = into.subspan(static_cast<std::size_t>(begin), count);
    if (part.taps) {
        addFiltered(*part.taps, alignScratch.data(), part.gain, held);
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            held[i] += part.gain * alignScratch[i];
        }
    }
    return {.first = static_cast<std::size_t>(begin), .end = static_cast<std::size_t>(stop)};
}

bool Analyzer::continueMatchSearch(double liveRate)
{
    if (!searchingSerial) {
        return false;
    }
    const std::uint64_t before = matchSearch.work();
    const bool ended = matchSearch.step(kMatchSearchWork);
    workDone.searchWork += matchSearch.work() - before;
    if (!ended) {
        matchesPending = true; // Frames go on until it ends, frozen too.
        return false;
    }
    matchStates.insert_or_assign(*searchingSerial,
                                 MatchState{.analysis = matchSearch.result(),
                                            .aligned = searchAligning,
                                            .analysedAt = searchStartedAt,
                                            .wait = static_cast<std::uint64_t>(
                                                std::lround(kMatchRefreshSeconds * liveRate))});
    if (searchAligning) {
        adoptAlignment(*searchingSerial, matchSearch.result());
    }
    searchingSerial.reset();
    return true;
}

// The alignment a search found for the track `serial`, taken into use unless it is the one in
// use within rounding (a ten-thousandth of a sample, and the same polarity), so that nothing
// drawn from it is redone while the routing holds. None for a track not related.
void Analyzer::adoptAlignment(std::uint64_t serial, const PairAnalysis& match)
{
    if (!match.related) {
        alignments.erase(serial);
        return;
    }
    const double whole = std::floor(match.delay);
    Alignment next{.offset = static_cast<std::ptrdiff_t>(whole),
                   .fraction = static_cast<float>(match.delay - whole),
                   .flipped = match.inverted};
    if (const auto found = alignments.find(serial); found != alignments.end()) {
        const Alignment& inUse = found->second;
        const double moved =
            std::abs(static_cast<double>(inUse.offset) + inUse.fraction - match.delay);
        if (moved <= 1e-4 && inUse.flipped == next.flipped) {
            return;
        }
    }
    if (next.fraction > 0.0f) {
        auto taps = std::make_shared<std::vector<float>>();
        alignFilter.weightsAt(next.fraction, *taps);
        next.taps = std::move(taps);
    }
    alignments.insert_or_assign(serial, std::move(next));
}

Analyzer::SumPart Analyzer::alignedPart(std::size_t lane, float sign) const
{
    SumPart part{.lane = lane, .gain = sign};
    const bool on = differenceShown() && frameSettings.enumValue<protocol::Toggle>(
                                             SettingId::kAlign) == protocol::Toggle::kOn;
    if (const auto found = alignments.find(laneOrder[lane]); on && found != alignments.end()) {
        const Alignment& alignment = found->second;
        part.offset = alignment.offset;
        part.fraction = alignment.fraction;
        part.taps = alignment.taps;
        // The delay and a flip are taken out, which change nothing of the track's own sound; its
        // level is not: a change of level is part of what the effect did.
        part.gain = alignment.flipped ? -sign : sign;
        part.aligned = true;
    }
    return part;
}

bool Analyzer::readSamples(const SampleSource& source, int inputChannels, ChannelView view,
                           std::uint64_t start, std::span<float> destination,
                           std::span<float> working)
{
    workDone.samplesRead += destination.size();
    return readView(source, inputChannels, view, start, destination, working);
}

bool Analyzer::readSamplesHeld(const SampleSource& source, int inputChannels, ChannelView view,
                               std::uint64_t start, std::span<float> destination,
                               std::span<float> working)
{
    workDone.samplesRead += destination.size();
    return readViewHeld(source, inputChannels, view, start, destination, working);
}

void Analyzer::readLaneImage(std::size_t index, std::size_t length, std::uint64_t from,
                             std::span<float> into)
{
    const Lane& lane = *lanes.at(laneOrder[index]);
    // Another track's image holds the lane channel alone; this track's, its captured channels.
    laneScratch.resize(std::max(laneScratch.size(), into.size()));
    if (lane.image.length() != length ||
        !readSamples(lane.image, imageChannels(lane.image, lane.engine->inputChannels()),
                     laneView(lane, laneChannel(frameSettings)), from, into, laneScratch)) {
        std::ranges::fill(into, 0.0f);
    }
}

void Analyzer::readSum(const std::vector<SumPart>& parts, std::size_t length, std::size_t position,
                       std::span<float> into)
{
    std::ranges::fill(into, 0.0f);
    for (const SumPart& part : parts) {
        // Positions position + i show this track's sample at position + i + offset; through a
        // filter, the wave a fraction of a sample after it, read from the samples around it,
        // silent beyond the sweep.
        if (part.taps) {
            // The stretch this frame's new audio reaches is filtered once a frame (Lane::Lined).
            const Lane::Lined& lined = lanes.at(laneOrder[part.lane])->lined;
            if (lined.frame == lanesFrame && lined.offset == part.offset &&
                lined.fraction == part.fraction && lined.length == length &&
                position >= lined.first &&
                position + into.size() <= lined.first + lined.values.size()) {
                const float* const values = lined.values.data() + (position - lined.first);
                for (std::size_t i = 0; i < into.size(); ++i) {
                    into[i] += part.gain * values[i];
                }
                continue;
            }
        }
        const auto [first, last] = readsOf(part);
        const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(position) + first;
        const std::size_t reads = into.size() + static_cast<std::size_t>(last - first);
        const std::ptrdiff_t begin = std::max<std::ptrdiff_t>(0, -source);
        const std::ptrdiff_t stop = std::min<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(reads), static_cast<std::ptrdiff_t>(length) - source);
        if (begin >= stop) {
            continue;
        }
        const std::span<float> read = [&] {
            if (!part.taps) {
                sumScratch.resize(static_cast<std::size_t>(stop - begin));
                return std::span(sumScratch);
            }
            alignScratch.assign(reads, 0.0f);
            return std::span(alignScratch)
                .subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(stop - begin));
        }();
        workDone.samplesSummed += read.size();
        readLaneImage(part.lane, length, static_cast<std::uint64_t>(source + begin), read);
        if (part.taps) {
            addFiltered(*part.taps, alignScratch.data(), part.gain, into);
            continue;
        }
        for (std::ptrdiff_t i = begin; i < stop; ++i) {
            into[static_cast<std::size_t>(i)] +=
                part.gain * sumScratch[static_cast<std::size_t>(i - begin)];
        }
    }
    // Where an aligned part has no audio of the moments shown, nothing is: not the other parts
    // alone, which would read as a large difference.
    for (const SumPart& part : parts) {
        if (part.aligned) {
            blankUnheld(part, length, position, into);
        }
    }
}

void Analyzer::readSumBlock(const std::vector<SumPart>& parts, std::size_t length,
                            std::int64_t position, std::span<float> into)
{
    const auto size = static_cast<std::int64_t>(into.size());
    const std::int64_t inFirst = std::clamp<std::int64_t>(-position, 0, size);
    const std::int64_t inEnd =
        std::clamp<std::int64_t>(static_cast<std::int64_t>(length) - position, inFirst, size);
    if (inFirst < inEnd) {
        readSum(parts, length, static_cast<std::size_t>(position + inFirst),
                into.subspan(static_cast<std::size_t>(inFirst),
                             static_cast<std::size_t>(inEnd - inFirst)));
    }
    // Past the sweep's ends: each part round the wrap, or none.
    const auto pastEnds = [&](std::int64_t from, std::span<float> margin) {
        if (margin.empty()) {
            return;
        }
        std::ranges::fill(margin, 0.0f);
        for (const SumPart& part : parts) {
            const Lane& lane = *lanes.at(laneOrder[part.lane]);
            // Lined up, or shifted by Fix preview (silent where the shift reads off the sweep), or
            // not on this length: no neighbours past the sweep's ends.
            if (part.aligned || part.offset != 0 || lane.image.length() != length) {
                std::ranges::fill(margin, std::numeric_limits<float>::quiet_NaN());
                return;
            }
            marginSamples.resize(margin.size());
            marginScratch.resize(std::max(marginScratch.size(), margin.size()));
            const int channels = imageChannels(lane.image, lane.engine->inputChannels());
            const bool held = readWrapped(
                [&](std::uint64_t first, std::span<float> destination) {
                    return readSamples(lane.image, channels,
                                       laneView(lane, laneChannel(frameSettings)), first,
                                       destination, marginScratch);
                },
                length, from, marginSamples);
            if (!held) {
                std::ranges::fill(margin, std::numeric_limits<float>::quiet_NaN());
                return;
            }
            for (std::size_t i = 0; i < margin.size(); ++i) {
                margin[i] += part.gain * marginSamples[i];
            }
        }
    };
    pastEnds(position, into.first(static_cast<std::size_t>(inFirst)));
    pastEnds(position + inEnd, into.subspan(static_cast<std::size_t>(inEnd)));
}

bool Analyzer::updateSumStats(BlockStats& stats, SumRefresh& refresh,
                              const std::vector<std::int64_t>& key, Kept& kept,
                              const std::vector<SumPart>& parts, Reach reach, std::size_t length,
                              std::size_t block, std::size_t shares)
{
    const auto read = [&](std::size_t from, std::span<float> into) {
        readSum(parts, length, from, into);
        return true;
    };
    if (!stats.fits(length, block)) {
        stats.reset(length, block);
        stats.update(0, length, read);
        (void)kept.follows(key, lanesFrame);
        refresh = {};
        return true;
    }
    if (!kept.follows(key, lanesFrame)) {
        // Other parts, or frames that left these levels out: every block again, over the next
        // frames; meanwhile the blocks not redone yet still hold the levels they had. This
        // frame's changes are among them, and are not summed now: a track just shown changes its
        // whole sweep at once, which would be the whole of every part in one frame.
        refresh.remaining = length;
    } else {
        for (const SweepImage::Range& range : frameChanges) {
            const auto from = static_cast<std::size_t>(
                range.first > reach.before ? range.first - reach.before : 0);
            const auto to =
                static_cast<std::size_t>(std::min<std::uint64_t>(range.end + reach.after, length));
            if (from < to) {
                stats.update(from, to, read);
            }
        }
    }
    const bool refreshing = refresh.remaining > 0;
    continueRefresh(refresh, length, refreshStep(weightOf(parts), shares),
                    [&](std::size_t from, std::size_t to) { stats.update(from, to, read); });
    return refreshing;
}

void Analyzer::collectEnergies(std::span<const Positions> ranges, std::size_t length,
                               std::size_t block, const std::vector<SumPart>& parts,
                               const BlockStats& sumStats, std::vector<std::vector<double>>& tracks,
                               std::vector<std::span<const double>>& spans,
                               std::vector<double>& sums)
{
    tracks.resize(parts.size());
    for (std::vector<double>& energies : tracks) {
        energies.clear();
    }
    sums.clear();
    const bool summed = sumStats.fits(length, block);
    // Positions [from, to) measured from their samples. A position where the sum is not finite,
    // as it is wherever a compared track's sample is not (a broken signal upstream), is left out
    // of the sum and of every part alike, as in Trigger or Free.
    const auto measureExactly = [&](Positions positions) {
        const std::size_t size = positions.end - positions.first;
        pieceSumScratch.resize(size);
        readSum(parts, length, positions.first, pieceSumScratch);
        double sum = 0.0;
        for (const float value : pieceSumScratch) {
            if (std::isfinite(value)) {
                sum += static_cast<double>(value) * value;
            }
        }
        sums.push_back(sum);
        for (std::size_t part = 0; part < parts.size(); ++part) {
            pieceScratch.resize(size);
            onePart.assign(1, parts[part]);
            readSum(onePart, length, positions.first, pieceScratch);
            double energy = 0.0;
            for (std::size_t i = 0; i < size; ++i) {
                if (std::isfinite(pieceSumScratch[i])) {
                    energy += static_cast<double>(pieceScratch[i]) * pieceScratch[i];
                }
            }
            tracks[part].push_back(energy);
        }
    };
    for (const Positions range : ranges) {
        if (range.first >= range.end) {
            continue;
        }
        const std::size_t firstBlock = (range.first + block - 1) / block;
        const std::size_t endBlock = range.end / block;
        const bool wholeBlocks = endBlock > firstBlock;
        if (!wholeBlocks) {
            measureExactly(range);
            continue;
        }
        // The pieces of blocks at the edges of the range, measured from their samples; whole
        // blocks from the levels kept per block, unless a sample in them is not finite.
        if (range.first < firstBlock * block) {
            measureExactly({.first = range.first, .end = firstBlock * block});
        }
        for (std::size_t index = firstBlock; index < endBlock; ++index) {
            if (summed && sumStats.blocks()[index].invalid > 0) {
                measureExactly({.first = index * block, .end = (index + 1) * block});
                continue;
            }
            sums.push_back(summed ? sumStats.blocks()[index].sumSquares : 0.0);
            for (std::size_t part = 0; part < parts.size(); ++part) {
                // A part as recorded has the lane's own levels; one the fix or Align shifts,
                // its levels as shifted.
                const Lane& lane = *lanes.at(laneOrder[parts[part].lane]);
                const BlockStats& stats = changesLevels(parts[part]) ? lane.fixedStats : lane.stats;
                tracks[part].push_back(stats.fits(length, block) ? stats.blocks()[index].sumSquares
                                                                 : 0.0);
            }
        }
        if (endBlock * block < range.end) {
            measureExactly({.first = endBlock * block, .end = range.end});
        }
    }
    spans.clear();
    for (const std::vector<double>& energies : tracks) {
        spans.emplace_back(energies);
    }
}

// The Compare part of a tempo-synced frame whose track lanes are already in `frameData`: the sum
// of the compared tracks as one more lane, then each track's match. Like the track lanes, the sum
// lane keeps its columns, and only those where compared tracks changed (moved by the largest
// correction while the fix is previewed) are summed again; all of them when a correction
// changes, spread over frames (see kSumRefreshSamples). The sums' levels per block are kept the
// same way, so the summing gains, measured with the matches or at most every 100 ms of audio,
// cost a pass over blocks. Matches are searched only when due (see kMatchRefreshSeconds).
void Analyzer::appendComparison(FrameHeaderValues& header, const Crop& crop, std::uint64_t count,
                                std::size_t columns, bool previewFix, std::size_t playhead,
                                double sampleRate)
{
    // The visible part, in sweep positions, which the matches and gains measure; the sum lane is
    // drawn over the crop, its neighbours for reconstruction included, read round the sweep's wrap
    // from blockStart(crop) (drawFirst and drawEnd are the crop's own indices).
    const std::size_t first = crop.first + crop.lead - crop.offset;
    const std::size_t end = crop.end - crop.trail - crop.offset;
    const std::size_t drawFirst = crop.first;
    const std::size_t drawEnd = crop.end;
    // Waits are measured in the capture's new audio, at its rate now.
    const double liveRate = engine.sampleRate();
    // The lane on show decides the search: the sum's, for a fix within its reach, or the
    // difference's, which lines the tracks up over Align's.
    const bool difference = differenceShown();
    const auto maxOffset =
        static_cast<std::size_t>(std::lround(protocol::kMatchReachSeconds * sampleRate));
    const auto alignOffset =
        static_cast<std::size_t>(std::lround(protocol::kAlignReachSeconds * sampleRate));
    const std::size_t block = statsBlock(sampleRate);
    const std::uint64_t now = engine.captureRing().endIndex();
    const auto length = static_cast<std::size_t>(count);
    const auto ownIndex =
        static_cast<std::size_t>(std::ranges::find(laneOrder, ownLane) - laneOrder.begin());

    // The match search: one track at a time, over the audio just before the playhead, continuing
    // at the right edge into the end of the previous pass when the playhead is near the left one
    // (the sweep's positions there are the moments just before), or over the newest part visible
    // when the playhead is elsewhere. Aligning, before the playhead whether it is shown or not:
    // the delay is the routing's, whatever the view is zoomed to.
    const std::size_t windowSamples =
        std::min(static_cast<std::size_t>(std::lround(kMatchWindowSeconds * sampleRate)), length);
    const bool playheadShown =
        (playhead > first && playhead < end) || (difference && playhead <= length);
    const std::size_t searchEnd = playheadShown ? playhead : end;
    const std::size_t searchCount =
        playheadShown ? windowSamples : std::min(windowSamples, end - first);
    // Positions [searchEnd - searchCount, searchEnd), the part before 0 from the right edge.
    const auto readSearch = [&](std::size_t index, std::span<float> into) {
        workDone.samplesSearched += into.size();
        const std::size_t wrapped = searchCount > searchEnd ? searchCount - searchEnd : 0;
        readLaneImage(index, length, length - wrapped, into.first(wrapped));
        readLaneImage(index, length, searchEnd + wrapped - searchCount, into.subspan(wrapped));
    };
    if (const auto lane = matchToSearch(header.laneSet, first, end, length, now, 0, 0.0, 1.0)) {
        matchOwn.resize(searchCount);
        matchTrack.resize(searchCount);
        readSearch(ownIndex, matchOwn);
        readSearch(*lane, matchTrack);
        ++workDone.matchSearches;
        matchSearch.start(matchOwn, matchTrack, difference ? alignOffset : maxOffset, block,
                          difference);
        searchAligning = difference;
        searchingSerial = laneOrder[*lane];
        searchStartedAt = now;
    }
    const bool searched =
        spreadStepped ? std::exchange(searchEnded, false) : continueMatchSearch(liveRate);

    buildParts();
    followLane();
    // The lane on show decides which sums are kept too: the sum's as recorded and as fixed, with
    // each shifted track's levels, or the difference's, with each aligned track's. The others are
    // left as they are, and redone when their lane comes back (Kept).
    // What the incremental sums were built from: a change of tracks, shifts or signs starts them
    // afresh.
    rawSumKey.assign({static_cast<std::int64_t>(length)});
    fixedSumKey.assign({static_cast<std::int64_t>(length)});
    differenceKey.assign({static_cast<std::int64_t>(length)});
    for (const SumPart& part : differenceParts) {
        differenceKey.insert(differenceKey.end(),
                             {static_cast<std::int64_t>(laneOrder[part.lane]), part.offset,
                              gainKey(part.gain), gainKey(part.fraction)});
    }
    for (const SumPart& fixed : fixedParts) {
        const auto serial = static_cast<std::int64_t>(laneOrder[fixed.lane]);
        rawSumKey.push_back(serial);
        fixedSumKey.insert(fixedSumKey.end(), {serial, fixed.offset, gainKey(fixed.gain)});
    }
    // Each track the fix shifts: its levels per block as shifted, which the fixed gain weighs the
    // fixed sum against; in the difference, each track Align shifts, which the null
    // depth's blocks are chosen by. A track summed as recorded (or no longer compared) keeps none
    // pending, and measures them all again once it is not: starting from its levels as recorded
    // and redone over the next frames, as when its match moves.
    const std::vector<SumPart>& shiftedParts = difference ? differenceParts : fixedParts;
    const auto shiftedPart = [&](std::uint64_t serial) {
        const auto part = std::ranges::find_if(shiftedParts, [&](const SumPart& candidate) {
            return laneOrder[candidate.lane] == serial;
        });
        return part == shiftedParts.end() || !changesLevels(*part) ? nullptr : &*part;
    };
    for (const std::uint64_t serial : laneOrder) {
        Lane& lane = *lanes.at(serial);
        if (const SumPart* const part = shiftedPart(serial)) {
            // An aligned part is empty where it has no audio, which one the fix shifts is not.
            lane.fixedKey.assign({static_cast<std::int64_t>(length), part->offset,
                                  gainKey(part->fraction), part->aligned ? 1 : 0});
            if (!lane.fixedStats.fits(length, block) && lane.stats.fits(length, block)) {
                lane.fixedStats = lane.stats;
            }
        } else {
            lane.fixedRefresh = {};
            lane.fixedKey.clear();
            lane.fixedKept = {};
        }
    }

    // The sum lane, with its parts as previewed. Its geometry: the span and visible part, and the
    // columns; its key: each part as summed.
    const std::vector<SumPart>& parts = shownParts(previewFix);
    sumGeometry.assign({static_cast<std::int64_t>(length), static_cast<std::int64_t>(drawFirst),
                        static_cast<std::int64_t>(drawEnd), static_cast<std::int64_t>(columns)});
    sumKey.clear();
    for (const SumPart& part : parts) {
        sumKey.insert(sumKey.end(), {static_cast<std::int64_t>(laneOrder[part.lane]), part.offset,
                                     gainKey(part.gain), gainKey(part.fraction)});
    }
    const std::size_t visibleCount = drawEnd - drawFirst;
    const bool sumRebuilt = sumGeometry != lastSumGeometry ||
                            !sumReduction.fits(visibleCount, columns, crop.lead, crop.trail);

    // The recomputations under way this frame, which share its kSumRefreshSamples, so that many
    // at once (every match moved) take no longer per frame than one.
    const auto pending = [&](const BlockStats& stats, const SumRefresh& refresh,
                             const std::vector<std::int64_t>& key, const Kept& kept) {
        return stats.fits(length, block) && (refresh.remaining > 0 || !kept.holds(key, lanesFrame))
                   ? 1U
                   : 0U;
    };
    std::size_t shares =
        difference ? pending(differenceStats, differenceRefresh, differenceKey, differenceKept)
                   : pending(rawSumStats, rawSumRefresh, rawSumKey, rawSumKept) +
                         pending(fixedSumStats, fixedSumRefresh, fixedSumKey, fixedSumKept);
    for (const std::uint64_t serial : laneOrder) {
        const Lane& lane = *lanes.at(serial);
        if (!lane.fixedKey.empty()) {
            shares += pending(lane.fixedStats, lane.fixedRefresh, lane.fixedKey, lane.fixedKept);
        }
    }
    if (!sumRebuilt && (sumRefresh.remaining > 0 || !sumKept.holds(sumKey, lanesFrame))) {
        ++shares;
    }
    // The aligned lanes being drawn again share the budget too (produceLanes).
    sumShares = shares;
    shares += feedShares;

    // Measured again beyond this frame's changes: the gains follow, so they settle as the
    // recomputation ends.
    // What a change of a compared track's audio reaches in a fixed sum: the fix's reach either
    // way; in the difference, as far as its parts are shifted and their filters read.
    const Reach fixReach{.before = maxOffset, .after = maxOffset};
    const bool differenceMeasured =
        difference &&
        updateSumStats(differenceStats, differenceRefresh, differenceKey, differenceKept,
                       differenceParts, reachOf(differenceParts), length, block, shares);
    const bool rawMeasured =
        !difference && updateSumStats(rawSumStats, rawSumRefresh, rawSumKey, rawSumKept, rawParts,
                                      {}, length, block, shares);
    bool fixedMeasured =
        !difference && updateSumStats(fixedSumStats, fixedSumRefresh, fixedSumKey, fixedSumKept,
                                      fixedParts, fixReach, length, block, shares);
    for (const std::uint64_t serial : laneOrder) {
        Lane& lane = *lanes.at(serial);
        if (lane.fixedKey.empty()) {
            continue;
        }
        onePart.assign(1, *shiftedPart(serial));
        fixedMeasured =
            updateSumStats(lane.fixedStats, lane.fixedRefresh, lane.fixedKey, lane.fixedKept,
                           onePart, difference ? reachOf(onePart) : fixReach, length, block,
                           shares) ||
            fixedMeasured;
    }

    // The matches' values and the gains, when due.
    const auto gainInterval =
        static_cast<std::uint64_t>(protocol::kReadoutIntervalSeconds * liveRate);
    if (searched || rawMeasured || fixedMeasured || differenceMeasured || !comparisonCache.valid ||
        gainsDueByTime(now, gainInterval)) {
        packMatches();
        // The sum as recorded over the visible part; the fix, and the difference of aligned
        // tracks, where every shifted track has samples of the same moments as the others: not
        // past the sweep's edges, nor where a shift reaches across the playhead, beyond which the
        // sweep holds the previous pass.
        const auto [earliest, latest] = fixedReach(shiftedParts);
        const auto [fixedFirst, fixedEnd] = fixedPart(first, end, length, earliest, latest);
        std::array<Positions, 2> fixedRanges{Positions{.first = fixedFirst, .end = fixedEnd},
                                             Positions{.first = fixedEnd, .end = fixedEnd}};
        if (playhead <= length) {
            // Positions p whose shifted reads p + offset lie on the other side of the playhead.
            const auto seamFirst = static_cast<std::size_t>(
                std::max<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(playhead) - latest, 0));
            const std::size_t seamEnd =
                std::min(length, playhead + static_cast<std::size_t>(-earliest));
            fixedRanges[0].end = std::clamp(seamFirst, fixedFirst, fixedEnd);
            fixedRanges[1].first = std::clamp(seamEnd, fixedFirst, fixedEnd);
        }
        const std::array<Positions, 1> visible{Positions{.first = first, .end = end}};
        if (difference) {
            // How deep the difference nulls against this track, over the visible part (as far as
            // the aligned tracks have their audio); the sum's gains are not measured while it is
            // not shown.
            const bool shifted = earliest != 0 || latest != 0;
            collectEnergies(shifted ? std::span<const Positions>(fixedRanges)
                                    : std::span<const Positions>(visible),
                            length, block, differenceParts, differenceStats, trackEnergies,
                            energySpans, sumEnergies);
            comparisonCache.nullDepth = static_cast<float>(
                nullDepthOfBlocks(energySpans, sumEnergies, block, ownPart(differenceParts)));
        } else {
            collectEnergies(visible, length, block, rawParts, rawSumStats, trackEnergies,
                            energySpans, sumEnergies);
            collectEnergies(fixedRanges, length, block, fixedParts, fixedSumStats,
                            fixedTrackEnergies, fixedEnergySpans, fixedSumEnergies);
            comparisonCache.sumGain =
                static_cast<float>(overlapGainOfBlocks(energySpans, sumEnergies, block));
            comparisonCache.fixedGain =
                static_cast<float>(overlapGainOfBlocks(fixedEnergySpans, fixedSumEnergies, block));
        }
        comparisonCache.at = now;
        comparisonCache.valid = true;
    }

    // The sum lane's columns.
    const auto readVisibleSum = [&](std::size_t from, std::span<float> into) {
        workDone.samplesReduced += into.size();
        readSumBlock(parts, length, blockStart(crop) + static_cast<std::int64_t>(from), into);
        return true;
    };
    // The playhead joins the newest pass to the previous one, and each lane's image its own: the
    // sum holds every join, with no wave across any of them. (With Fix preview, a shifted part's
    // join lies its shift away, not marked: DESIGN.md, AD-32.)
    sumSeams.clear();
    addSeam(static_cast<std::int64_t>(playhead), crop, length, sumSeams);
    for (const std::uint64_t serial : laneOrder) {
        addJoins(lanes.at(serial)->image, crop, sumSeams);
    }
    if (sumRebuilt) {
        sumReduction.reset(visibleCount, columns, false, crop.lead, crop.trail);
        sumReduction.update(0, visibleCount, readVisibleSum, sumSeams);
        lastSumGeometry = sumGeometry;
        (void)sumKept.follows(sumKey, lanesFrame);
        sumRefresh = {};
    } else {
        const Reach reach = difference ? reachOf(parts) : previewFix ? fixReach : Reach{};
        if (!sumKept.follows(sumKey, lanesFrame)) {
            // Other parts, or frames that left the sum out: every column again, over the next
            // frames, from left to right; this frame's changes with them (updateSumStats).
            sumRefresh.remaining = visibleCount;
            sumRefresh.cursor = 0;
        } else {
            for (const SweepImage::Range& range : frameChanges) {
                forEachInBlock(static_cast<std::int64_t>(range.first) -
                                   static_cast<std::int64_t>(reach.before),
                               static_cast<std::int64_t>(range.end + reach.after), crop,
                               visibleCount, length, [&](std::size_t from, std::size_t to) {
                                   sumReduction.update(from, to, readVisibleSum, sumSeams);
                               });
            }
        }
        continueRefresh(sumRefresh, visibleCount, refreshStep(weightOf(parts), shares),
                        [&](std::size_t from, std::size_t to) {
                            sumReduction.update(from, to, readVisibleSum, sumSeams);
                        });
    }
    frameData.insert(frameData.end(), sumReduction.values().begin(), sumReduction.values().end());
    frameData.insert(frameData.end(), comparisonCache.matches.begin(),
                     comparisonCache.matches.end());
    header.laneCount = static_cast<std::uint8_t>(laneOrder.size() + 1);
    header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kCompared));
    header.sumGain = comparisonCache.sumGain;
    header.fixedGain = comparisonCache.fixedGain;
    header.compareLane = comparisonCache.lane;
    header.nullDepth = comparisonCache.nullDepth;
}

template <typename Read>
bool Analyzer::addPartFrom(const Read& read, const SumPart& part, std::size_t position,
                           std::uint64_t count, std::span<float> into)
{
    const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(position) + part.offset;
    const std::ptrdiff_t begin = std::max<std::ptrdiff_t>(0, -source);
    const std::ptrdiff_t stop = std::min<std::ptrdiff_t>(
        static_cast<std::ptrdiff_t>(into.size()), static_cast<std::ptrdiff_t>(count) - source);
    if (begin >= stop) {
        return true;
    }
    sumScratch.resize(static_cast<std::size_t>(stop - begin));
    workDone.samplesSummed += sumScratch.size();
    const std::ptrdiff_t from = source + begin;
    const bool held = read(part.lane, static_cast<std::uint64_t>(from), sumScratch);
    for (std::ptrdiff_t i = begin; i < stop; ++i) {
        into[static_cast<std::size_t>(i)] +=
            part.gain * sumScratch[static_cast<std::size_t>(i - begin)];
    }
    return held;
}

bool Analyzer::gainsDueByTime(std::uint64_t now, std::uint64_t interval) const
{
    // Frozen, the audio shown is held and does not change: gains measured there stay until the
    // view or the matches change.
    return !isFrozen && (now < comparisonCache.at || now - comparisonCache.at >= interval);
}

bool Analyzer::gainAudioReplaced() const
{
    // Resumed after the frames stopped for a while (a hidden page asks for none): a track's
    // capture may have moved past audio the gains need since they began.
    for (std::size_t index = 0; index < laneOrder.size(); ++index) {
        if (laneSources[index].source->beginIndex() > gainJob.needFrom[index]) {
            return true;
        }
    }
    return false;
}

bool Analyzer::stepGainJob()
{
    if (gainAudioReplaced()) {
        gainJob.active = false; // Measured again once due, rather than from silence.
        return false;
    }
    // Lane `index`'s lane channel where the window was when the gains began.
    const auto readJobWindow = [&](std::size_t index, std::uint64_t from, std::span<float> into) {
        readScratch.resize(std::max(readScratch.size(), into.size()));
        return readSamplesHeld(*laneSources[index].source, laneSources[index].channels,
                               laneChannel(frameSettings), gainJob.laneStarts[index] + from, into,
                               readScratch);
    };
    laneSamples.resize(laneOrder.size());
    fixedLanes.resize(laneOrder.size());
    // Reads the parts, each lane as recorded and as the fix shifts it, then measures the sum as
    // recorded, then fixed: as much as the frame's budget allows, so a short view is done at once,
    // as it was before it could be spread.
    std::size_t budget = kGainReadWork;
    while (gainJob.active && budget > 0) {
        if (gainJob.next < gainJob.raw.size()) {
            const std::size_t part = gainJob.next++;
            const std::size_t lane = gainJob.raw[part].lane;
            std::vector<float>& samples = laneSamples[lane];
            samples.resize(gainJob.visibleCount);
            readJobWindow(lane, gainJob.first, samples);
            std::vector<float>& fixed = fixedLanes[lane];
            if (gainJob.raw[part].aligned) {
                // Lined up by Align: read as the difference draws it, and measured only where
                // every such part has its audio.
                std::ranges::fill(samples, 0.0f);
                const LaneSource source{.source = laneSources[lane].source,
                                        .start = gainJob.laneStarts[lane],
                                        .channels = laneSources[lane].channels,
                                        .none = laneSources[lane].none};
                const Positions has =
                    addAlignedPart(source, source.start, gainJob.raw[part],
                                   static_cast<std::int64_t>(gainJob.first), samples);
                gainJob.held.first = std::max(gainJob.held.first, has.first);
                gainJob.held.end =
                    std::max(gainJob.held.first, std::min(gainJob.held.end, has.end));
                fixed.clear();
                budget -= std::min(budget, 3 * samples.size());
            } else if (gainJob.difference) {
                // The part as the difference has it (another track negated); nothing as fixed.
                const float gain = gainJob.raw[part].gain;
                for (float& sample : samples) {
                    sample *= gain;
                }
                fixed.clear();
            } else {
                fixed.assign(gainJob.fixedEnd - gainJob.fixedFirst, 0.0f);
                addPartFrom(readJobWindow, gainJob.fixed[part], gainJob.fixedFirst, gainJob.count,
                            fixed);
            }
            budget -= std::min(budget, samples.size() + fixed.size());
            continue;
        }
        if (!gainJob.measuring) {
            std::vector<std::span<const float>>& tracks =
                gainJob.measured == 0 ? rawTracks : fixedTracks;
            tracks.clear();
            for (const SumPart& part : gainJob.raw) {
                if (gainJob.difference) {
                    std::vector<float>& samples = laneSamples[part.lane];
                    std::ranges::fill(std::span(samples).first(gainJob.held.first), 0.0f);
                    std::ranges::fill(std::span(samples).subspan(gainJob.held.end), 0.0f);
                }
                tracks.emplace_back(gainJob.measured == 0 ? laneSamples[part.lane]
                                                          : fixedLanes[part.lane]);
            }
            gainJob.measure.start(tracks, gainJob.block);
            gainJob.measuring = true;
        }
        const std::uint64_t before = gainJob.measure.work();
        const bool ended = gainJob.measure.step(budget);
        const std::uint64_t passed = gainJob.measure.work() - before;
        workDone.gainWork += passed;
        budget -= static_cast<std::size_t>(std::min<std::uint64_t>(budget, passed));
        if (!ended) {
            continue;
        }
        gainJob.measuring = false;
        const auto gain = static_cast<float>(gainJob.measure.result());
        if (gainJob.measured == 0 && !gainJob.difference) {
            gainJob.sumGain = gain;
            gainJob.measured = 1;
        } else {
            // The difference: its depth against this track, and no gains; the sum: its gains as
            // recorded and as fixed, and no depth.
            const float none = std::numeric_limits<float>::quiet_NaN();
            comparisonCache.sumGain = gainJob.difference ? none : gainJob.sumGain;
            comparisonCache.fixedGain = gainJob.difference ? none : gain;
            comparisonCache.nullDepth =
                gainJob.difference ? static_cast<float>(gainJob.measure.depth(gainJob.reference))
                                   : none;
            comparisonCache.tracks = gainJob.tracks;
            // A match that ended meanwhile is measured next: due at once, frozen too.
            comparisonCache.valid = !gainJob.stale;
            comparisonCache.at = gainJob.at;
            gainJob.active = false;
            return true;
        }
    }
    return false;
}

bool Analyzer::stepSpreadWork(double liveRate)
{
    spreadStepped = true;
    bool ended = false;
    if (searchingSerial && continueMatchSearch(liveRate)) {
        searchEnded = true;
        ended = true;
        if (gainJob.active) {
            gainJob.stale = true; // As when the search ends in a frame drawn.
        }
    }
    if (gainJob.active && stepGainJob()) {
        ended = true;
    }
    return ended;
}

// A Compare frame in Trigger or Free: every track's lane over the window, then the sum of
// the compared tracks, then each track's match against this track. Rolling, each lane and the
// sum keep their columns and reduce only those that scrolled in, the sum through a reader that
// adds the compared lanes (shifted and flipped as previewed); otherwise the visible part
// [first, end) is reduced afresh. Matches are searched one at a time, over the newest part of the
// visible part; the summing gains (over the visible part) and the readouts (over the whole
// window) are measured at most every 100 ms of audio, the gains over frames.
void Analyzer::renderCompare(FrameHeaderValues header, const Window& window, std::uint64_t start,
                             std::uint64_t sampleCount, const Crop& crop, std::size_t factor,
                             bool rolling, double sampleRate, const Commands& command,
                             std::vector<std::byte>& frame)
{
    // The visible part, in window positions, which the matches and gains measure; the lanes and
    // the sum are drawn over the crop, their neighbours for reconstruction included, which may lie
    // past the window's ends (from window position blockPosition).
    const std::size_t first = crop.first + crop.lead - crop.offset;
    const std::size_t end = crop.end - crop.trail - crop.offset;
    const std::size_t drawCount = crop.end - crop.first;
    const std::int64_t blockPosition =
        static_cast<std::int64_t>(crop.first) - static_cast<std::int64_t>(crop.offset);
    // Waits are measured in the capture's new audio, at its rate now.
    const double liveRate = engine.sampleRate();
    // The lane on show decides the search: the sum's, for a fix within its reach, or the
    // difference's, which lines the tracks up over Align's.
    const bool difference = differenceShown();
    const auto maxOffset = static_cast<std::size_t>(std::lround(
        (difference ? protocol::kAlignReachSeconds : protocol::kMatchReachSeconds) * sampleRate));
    const std::size_t block = statsBlock(sampleRate);
    const std::size_t columns = command.columns;
    const bool previewFix = command.previewFix;
    const auto count = static_cast<std::size_t>(sampleCount);
    const std::size_t visibleCount = end - first;
    const auto ownIndex =
        static_cast<std::size_t>(std::ranges::find(laneOrder, ownLane) - laneOrder.begin());
    const std::uint64_t now = engine.captureRing().endIndex();

    // Lane `index`'s lane channel over window samples from `from`, silent where its capture does
    // not hold them: false then (not captured yet, or any more).
    const ChannelView channel = laneChannel(frameSettings);
    const auto readLaneWindow = [&](std::size_t index, std::uint64_t from, std::span<float> into) {
        const LaneSource& lane = laneSources[index];
        readScratch.resize(std::max(readScratch.size(), into.size()));
        return readSamplesHeld(*lane.source, lane.channels, channel, lane.start + from, into,
                               readScratch);
    };

    const auto addPart = [&](const SumPart& part, std::size_t position, std::span<float> into) {
        return addPartFrom(readLaneWindow, part, position, count, into);
    };

    // The match search: one track at a time, over the newest part of the visible part, which
    // ends at its right edge.
    const auto windowSamples =
        static_cast<std::size_t>(std::lround(kMatchWindowSeconds * sampleRate));
    const std::size_t searchFirst = end - std::min(windowSamples, visibleCount);
    // Aligning, over the newest audio that this track and the other both hold, wherever the view
    // is and however short: the delay is the routing's, and an effect's latency can exceed the
    // window. As window positions, which may lie before the window or after it; none (empty) for
    // a track that holds none of this window's audio.
    const auto alignWindow = [&](std::size_t other) {
        std::int64_t from = std::numeric_limits<std::int64_t>::min();
        std::int64_t to = std::numeric_limits<std::int64_t>::max();
        for (const std::size_t index : {ownIndex, other}) {
            const LaneSource& lane = laneSources[index];
            if (lane.none) {
                return std::pair<std::int64_t, std::int64_t>{0, 0};
            }
            const std::uint64_t oldest = lane.source->beginIndex();
            const std::uint64_t newest = lane.source->endIndex();
            // Live, not the oldest eighth a capture holds: the audio thread writes on over it
            // while the search's copy is read.
            const std::uint64_t spare = isFrozen ? 0 : (newest - oldest) / 8;
            const auto start = static_cast<std::int64_t>(lane.start);
            from = std::max(from, static_cast<std::int64_t>(oldest + spare) - start);
            to = std::min(to, static_cast<std::int64_t>(newest) - start);
        }
        from = std::max(from, to - static_cast<std::int64_t>(windowSamples));
        return std::pair<std::int64_t, std::int64_t>{from, std::max(from, to)};
    };
    // The view as zoomed: a triggered window starts a different fraction of a sample in each
    // frame, which moves the samples shown by one now and then, and that is not another view.
    const Crop view = cropOf(static_cast<double>(count), 0.0, window.cropStart, window.cropEnd);
    if (const auto lane = matchToSearch(header.laneSet, view.first, view.end, count, now, start,
                                        window.viewStart, window.viewEnd)) {
        const auto [searchFrom, searchTo] =
            difference ? alignWindow(*lane)
                       : std::pair<std::int64_t, std::int64_t>{searchFirst, end};
        matchOwn.resize(static_cast<std::size_t>(searchTo - searchFrom));
        matchTrack.resize(matchOwn.size());
        // A failed read (a capture overwritten during the copy, or not holding this audio yet)
        // keeps the match as it was rather than one from partly silent audio. It counts as this
        // track's turn, so the others are searched first, but is tried again soon rather than at
        // the next refresh.
        const auto readSearch = [&](std::size_t index, std::span<float> into) {
            workDone.samplesSearched += into.size();
            // A position before the window wraps, as the lane's start plus it does back.
            return readLaneWindow(index, static_cast<std::uint64_t>(searchFrom), into);
        };
        if (searchTo > searchFrom && readSearch(ownIndex, matchOwn) &&
            readSearch(*lane, matchTrack)) {
            ++workDone.matchSearches;
            matchSearch.start(matchOwn, matchTrack, maxOffset, block, difference);
            searchAligning = difference;
            searchingSerial = laneOrder[*lane];
            searchStartedAt = now;
        } else {
            MatchState& state = matchStates.try_emplace(laneOrder[*lane]).first->second;
            state.wait = static_cast<std::uint64_t>(std::lround(kMatchRetrySeconds * liveRate));
            state.analysedAt = now;
            // Frozen, no audio comes due again by time: due again at once, a few times.
            if (isFrozen && ++state.failedReads < kFrozenReadTries) {
                state.wait = 0;
                // A frame to try it in, though nothing else is due, and no audio to wait for.
                matchesPending = true;
                searchAtOnce = true;
            }
        }
    }
    const bool searched =
        spreadStepped ? std::exchange(searchEnded, false) : continueMatchSearch(liveRate);

    buildParts();
    followLane();

    // The matches' values when due, and the summing gains over the visible part: measured over
    // the next frames (kGainReadWork samples read in each), of the window as it was when they
    // began, so that no frame reads every compared track's whole view at once.
    const auto gainInterval =
        static_cast<std::uint64_t>(protocol::kReadoutIntervalSeconds * liveRate);
    if (gainJob.active && gainAudioReplaced()) {
        gainJob.active = false; // They begin afresh below.
    }
    if (searched && gainJob.active) {
        gainJob.stale = true;
    }
    if (searched || !comparisonCache.valid || gainsDueByTime(now, gainInterval)) {
        packMatches();
        if (!gainJob.active && !panning) {
            // The fix is measured where every shifted track has samples in the window.
            const auto [earliest, latest] = fixedReach(fixedParts);
            const auto [fixedFirst, fixedEnd] = fixedPart(first, end, count, earliest, latest);
            gainJob.active = true;
            gainJob.at = now;
            gainJob.sampleRate = sampleRate;
            gainJob.count = count;
            gainJob.block = block;
            gainJob.first = first;
            gainJob.visibleCount = visibleCount;
            gainJob.fixedFirst = fixedFirst;
            gainJob.fixedEnd = fixedEnd;
            gainJob.raw = difference ? differenceParts : rawParts;
            gainJob.fixed = fixedParts;
            gainJob.held = {.first = 0, .end = visibleCount};
            gainJob.difference = difference;
            gainJob.reference = ownPart(gainJob.raw);
            gainJob.next = 0;
            gainJob.measured = 0;
            gainJob.measuring = false;
            // The oldest sample each lane's parts read, as recorded or shifted, or the oldest
            // its source holds now if later: audio it loses before the end is audio since replaced.
            const auto [oldestRead, newestRead] =
                difference ? fixedReach(differenceParts) : std::pair{earliest, latest};
            const auto reach = static_cast<std::uint64_t>(std::max(-oldestRead, newestRead));
            const std::size_t oldest = difference ? first : std::min(first, fixedFirst);
            gainJob.laneStarts.clear();
            gainJob.needFrom.clear();
            for (const LaneSource& lane : laneSources) {
                gainJob.laneStarts.push_back(lane.start);
                const std::uint64_t from = lane.start + oldest;
                gainJob.needFrom.push_back(
                    std::max(from - std::min(from, reach), lane.source->beginIndex()));
            }
            gainJob.stale = false;
            gainJob.tracks.clear();
            for (const SumPart& part : rawParts) {
                gainJob.tracks.push_back(laneOrder[part.lane]);
            }
            if (!comparisonCache.valid && comparisonCache.tracks != gainJob.tracks) {
                // Nothing measured for these tracks yet: no value until it is. For the same
                // tracks over another view (while panning or zooming), or under other lanes'
                // labels, the last stays shown.
                comparisonCache.sumGain = std::numeric_limits<float>::quiet_NaN();
                comparisonCache.fixedGain = std::numeric_limits<float>::quiet_NaN();
                comparisonCache.nullDepth = std::numeric_limits<float>::quiet_NaN();
            }
        }
    }
    if (gainJob.active && !spreadStepped) {
        (void)stepGainJob();
    }

    // The lanes' columns: rolling, each lane keeps its columns and reduces only those that
    // scrolled in, as in the multi-track view; otherwise the visible part is reduced afresh.
    frameData.clear();
    std::size_t pointCount = 0;
    const auto addColumns = [&](std::span<const float> values) {
        header.mode = protocol::FrameMode::kMinMax;
        pointCount = columns;
        frameData.insert(frameData.end(), values.begin(), values.end());
    };
    const auto addVisible = [&](std::span<const float> samples) {
        workDone.samplesReduced += samples.size();
        header.mode = reduceToColumns(samples, crop.lead, crop.trail, columns, reduced, kNoSeam,
                                      &columnPeaks);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : samples.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    };
    // The block's samples past the window's ends that lie outside `held` (block indices) are no
    // neighbours: none is held there. Those within the window are left as read.
    const auto markUnheldMargins = [&](std::span<float> block, Positions held) {
        for (std::size_t i = 0; i < block.size(); ++i) {
            const std::int64_t position = blockPosition + static_cast<std::int64_t>(i);
            const bool pastWindow = position < 0 || std::cmp_greater_equal(position, count);
            if (pastWindow && (i < held.first || i >= held.end)) {
                block[i] = std::numeric_limits<float>::quiet_NaN();
            }
        }
    };
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        Lane& shown = *lanes.at(laneOrder[lane]);
        RollColumns& roll = shown.roll;
        // A track Align lines up is drawn as the difference reads it: shifted, through its
        // filter, at its gain, and empty where its audio has not arrived (or is held no more).
        const SumPart feed = laneFeed(lane);
        const std::uint64_t laneStart = laneSources[lane].start;
        // Window positions from `position`, which may lie before the window.
        const auto readLane = [&](std::int64_t position, std::span<float> into) {
            if (!feed.aligned) {
                return readLaneWindow(lane, static_cast<std::uint64_t>(position), into);
            }
            std::ranges::fill(into, 0.0f);
            const Positions held =
                addAlignedPart(laneSources[lane], laneStart, feed, position, into);
            return held.first == 0 && held.end == into.size();
        };
        feedKey(feed, feedScratch);
        if (feedScratch != shown.rolledFeed) {
            // Columns rolled from another feed are not this one's: all reduced again.
            roll.clear();
            shown.rolledFeed = feedScratch;
        }
        if (rolling) {
            addColumns(roll.update(
                laneStart - window.grid.boundary(window.firstColumn), window.firstColumn,
                window.grid, [&](std::uint64_t from, std::span<float> destination) {
                    workDone.samplesReduced += destination.size();
                    return readLane(static_cast<std::int64_t>(from - laneStart), destination);
                }));
            header.invalidLanes |= roll.holdsInvalid() ? laneBit(lane) : 0U;
            continue;
        }
        roll.clear();
        viewSamples.resize(drawCount);
        if (feed.aligned) {
            // Read as far beyond the window as its audio is held, empty where it is not, and
            // past the window's ends no neighbours there.
            std::ranges::fill(viewSamples, 0.0f);
            const Positions held =
                addAlignedPart(laneSources[lane], laneStart, feed, blockPosition, viewSamples);
            markUnheldMargins(viewSamples, held);
        } else {
            scratch.resize(std::max(scratch.size(), viewSamples.size()));
            (void)fillBlock(
                *laneSources[lane].source, laneSources[lane].channels, channel,
                static_cast<std::int64_t>(laneStart), static_cast<std::int64_t>(count),
                blockPosition, viewSamples, scratch,
                [&](std::int64_t from, std::span<float> destination) {
                    (void)readLaneWindow(lane, static_cast<std::uint64_t>(from), destination);
                    return true;
                },
                laneSources[lane].none);
        }
        header.invalidLanes |= holdsInvalid(shownPart(viewSamples, crop)) ? laneBit(lane) : 0U;
        addVisible(viewSamples);
    }
    header.invalidLanes |= sumLaneBit(header.invalidLanes);

    // The sum lane, its parts as previewed. Rolling, it keeps its columns too, through a reader
    // that adds the parts, for as long as the parts stay the same.
    const std::vector<SumPart>& parts = shownParts(previewFix);
    sumKey.assign({static_cast<std::int64_t>(count), static_cast<std::int64_t>(columns)});
    for (const SumPart& part : parts) {
        sumKey.insert(sumKey.end(), {static_cast<std::int64_t>(laneOrder[part.lane]), part.offset,
                                     gainKey(part.gain), gainKey(part.fraction)});
    }
    // Rolled columns are kept by the audio's own positions, which nothing rewrites: only other
    // parts make them stale, not a frame that left them out.
    if (sumKey != sumKept.key) {
        sumRoll.clear();
        sumKept.key = sumKey;
    }
    // Positions position + i of the window show a part's sample at position + i + offset, and
    // silence beyond the window. False if a part's capture does not hold its samples yet.
    // An aligned part is read as far beyond the window as its audio is held; where it holds
    // none, nothing is shown: not the other parts alone, which would read as a large difference.
    const auto readSumWindow = [&](std::size_t position, std::span<float> into) {
        std::ranges::fill(into, 0.0f);
        bool complete = true;
        Positions held{.first = 0, .end = into.size()};
        for (const SumPart& part : parts) {
            if (!part.aligned) {
                complete = addPart(part, position, into) && complete;
                continue;
            }
            const Positions has =
                addAlignedPart(laneSources[part.lane], laneSources[part.lane].start, part,
                               static_cast<std::int64_t>(position), into);
            held.first = std::max(held.first, has.first);
            held.end = std::max(held.first, std::min(held.end, has.end));
        }
        if (held.first > 0 || held.end < into.size()) {
            std::ranges::fill(into.first(held.first), 0.0f);
            std::ranges::fill(into.subspan(held.end), 0.0f);
            complete = false;
        }
        return complete;
    };
    // Positions from `position`, all past the window's ends: the sum of the parts' audio there
    // as each holds it, NaN wherever one holds none.
    const auto addSumMargin = [&](std::int64_t position, std::span<float> into) {
        std::ranges::fill(into, 0.0f);
        for (const SumPart& part : parts) {
            const LaneSource& source = laneSources[part.lane];
            marginSamples.assign(into.size(), 0.0f);
            if (part.aligned) {
                const Positions held =
                    addAlignedPart(source, source.start, part, position, marginSamples);
                for (std::size_t i = 0; i < marginSamples.size(); ++i) {
                    if (i < held.first || i >= held.end) {
                        marginSamples[i] = std::numeric_limits<float>::quiet_NaN();
                    }
                }
            } else if (source.none || part.offset != 0) {
                // Holds none of this window, or (Fix preview) is shifted, so that the window's
                // last positions it shifts out of are silent: no neighbours past its ends either.
                std::ranges::fill(marginSamples, std::numeric_limits<float>::quiet_NaN());
            } else {
                marginScratch.resize(std::max(marginScratch.size(), into.size()));
                readViewHeldOrNaN(*source.source, source.channels, channel,
                                  static_cast<std::int64_t>(source.start) + position, marginSamples,
                                  marginScratch);
                for (float& sample : marginSamples) {
                    sample *= part.gain;
                }
            }
            for (std::size_t i = 0; i < into.size(); ++i) {
                into[i] += marginSamples[i];
            }
        }
    };
    if (rolling) {
        addColumns(sumRoll.update(
            start - window.grid.boundary(window.firstColumn), window.firstColumn, window.grid,
            [&](std::uint64_t from, std::span<float> destination) {
                // Within the window, the sum as drawn; past its ends (a column's neighbours), the
                // parts' audio there as held. Held only if every part holds all of it, so a
                // column that read a neighbour not held is reduced again once it is.
                workDone.samplesReduced += destination.size();
                const std::int64_t position =
                    static_cast<std::int64_t>(from) - static_cast<std::int64_t>(start);
                const auto size = static_cast<std::int64_t>(destination.size());
                const std::int64_t inFirst = std::clamp<std::int64_t>(-position, 0, size);
                const std::int64_t inEnd = std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(count) - position, inFirst, size);
                bool held = true;
                if (inFirst < inEnd) {
                    held = readSumWindow(
                        static_cast<std::size_t>(position + inFirst),
                        destination.subspan(static_cast<std::size_t>(inFirst),
                                            static_cast<std::size_t>(inEnd - inFirst)));
                }
                const std::span<float> before =
                    destination.first(static_cast<std::size_t>(inFirst));
                const std::span<float> after = destination.subspan(static_cast<std::size_t>(inEnd));
                addSumMargin(position, before);
                addSumMargin(position + inEnd, after);
                const auto finite = [](float sample) { return std::isfinite(sample); };
                return held && std::ranges::all_of(before, finite) &&
                       std::ranges::all_of(after, finite);
            }));
    } else {
        sumRoll.clear();
        sumSamples.resize(drawCount);
        // The window's part as the sum reads it, and its neighbours past the window's ends: each
        // part's audio there as held, none where any part holds none.
        const auto size = static_cast<std::int64_t>(drawCount);
        const std::int64_t inFirst = std::clamp<std::int64_t>(-blockPosition, 0, size);
        const std::int64_t inEnd = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(count) - blockPosition, inFirst, size);
        const std::span<float> drawn = sumSamples;
        if (inFirst < inEnd) {
            readSumWindow(static_cast<std::size_t>(blockPosition + inFirst),
                          drawn.subspan(static_cast<std::size_t>(inFirst),
                                        static_cast<std::size_t>(inEnd - inFirst)));
        }
        addSumMargin(blockPosition, drawn.first(static_cast<std::size_t>(inFirst)));
        addSumMargin(blockPosition + inEnd, drawn.subspan(static_cast<std::size_t>(inEnd)));
        addVisible(sumSamples);
    }
    header.laneCount = static_cast<std::uint8_t>(laneOrder.size() + 1);
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    reconstruct(header, header.laneCount, crop, factor,
                [](std::size_t) { return std::span<const std::size_t>(); });
    frameData.insert(frameData.end(), comparisonCache.matches.begin(),
                     comparisonCache.matches.end());
    header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kCompared));
    header.sumGain = comparisonCache.sumGain;
    header.fixedGain = comparisonCache.fixedGain;
    header.compareLane = comparisonCache.lane;
    header.nullDepth = comparisonCache.nullDepth;

    // Readouts measure this track's lane channel over the visible part, at most every 100 ms of
    // audio and whenever the zoom changes.
    const std::uint64_t ownEnd = engine.captureRing().endIndex();
    const ChannelView ownView = ownLaneView(frameSettings);
    const ReadoutView shown{.viewStart = window.viewStart, .viewEnd = window.viewEnd};
    if (readoutsDue(ownEnd, ownView, shown)) {
        const auto [visibleFirst, visibleEnd] =
            meterRange(crop, static_cast<std::size_t>(sampleCount));
        storeReadouts(measureWindow(*laneSources[ownIndex].source, laneSources[ownIndex].channels,
                                    ownView, start + visibleFirst, start + visibleEnd, sampleRate),
                      ownEnd, ownView, shown);
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
}

} // namespace orizon::engine
