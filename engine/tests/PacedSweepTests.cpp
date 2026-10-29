#include "FrameDecoding.h"
#include "SampleRates.h"
#include "engine/Analyzer.h"
#include "engine/InstanceRegistry.h"
#include "engine/Motion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::FrameFlag;
using protocol::SettingId;

constexpr double kStart = 1.0;

// Tracks playing a ramp of host time (each sample holds the host sample it was played at, so a
// sweep says which audio it holds) in Beats, in blocks on a simulated clock. Each track's blocks
// arrive `lateSeconds` after they fall due, and its capture can start `offset` samples ahead of
// the host's (an instance that was processing before the others), so its capture indices differ.
// The transport can jump (a loop, a relocation).
class BeatsHost
{
public:
    struct Track
    {
        std::shared_ptr<Engine> engine;
        double lateSeconds = 0.0;
        std::uint64_t offset = 0;
        std::uint64_t played = 0; // Host samples delivered.
    };

    BeatsHost(double rate, std::uint64_t block, std::size_t tracks = 1,
              protocol::DisplayMode mode = protocol::DisplayMode::kSingle, std::uint64_t offset = 0,
              double tempo = 120.0)
        : rate(rate), block(block), tempo(tempo)
    {
        for (std::size_t index = 0; index < tracks; ++index) {
            auto engine = std::make_shared<Engine>();
            engine->prepare(rate);
            engine->settings().set(SettingId::kTimeBase,
                                   static_cast<double>(protocol::TimeBase::kBeats));
            engine->settings().set(SettingId::kBeatSpan,
                                   static_cast<double>(protocol::BeatSpan::kOneBeat));
            engine->settings().set(SettingId::kChannelViews, 0b0001);
            engine->settings().set(SettingId::kDisplayMode, static_cast<double>(mode));
            registry.add(engine);
            Track track{.engine = engine, .offset = index == 0 ? 0 : offset};
            if (track.offset > 0) {
                // Audio from before the host's start, at the positions before it.
                std::vector<float> silence(track.offset);
                std::array<const float*, 2> channels{silence.data(), silence.data()};
                engine->captureRing().write(channels.data(), 2, static_cast<int>(track.offset));
                engine->timeline().advance(
                    {.playing = true,
                     .positionValid = true,
                     .quarterNotes = -static_cast<double>(track.offset) / rate * tempo / 60.0,
                     .tempoValid = true,
                     .tempo = tempo},
                    static_cast<int>(track.offset), rate);
            }
            this->tracks.push_back(track);
        }
        analyzer = std::make_unique<Analyzer>(*this->tracks.front().engine, registry);
        analyzer->setClock([this] { return nanos(); });
        analyzer->setColumns(4000);
    }

    // Plays `seconds`: blocks as they arrive, the worker's 2 ms steps, and a frame at 60 Hz unless
    // the page asks for none.
    void play(double seconds, bool draw = true)
    {
        const double end = time + seconds;
        for (; time < end; time += 0.0005) {
            for (Track& track : tracks) {
                while (static_cast<double>(track.played + block) / rate + track.lateSeconds <=
                       time - kStart) {
                    writeBlock(track);
                }
            }
            if (std::fmod(time * 1000.0, 2.0) < 0.5) {
                analyzer->advance();
            }
            if (time >= nextDraw) {
                nextDraw += 1.0 / 60.0;
                if (!draw) {
                    continue;
                }
                std::vector<std::byte> bytes;
                if (analyzer->produce(bytes)) {
                    frames.push_back({.seconds = time, .frame = decode(bytes)});
                }
            }
        }
    }

    // Track `index`'s processing restarts: its capture takes `samples` of silence the host did
    // not play (no position known), so from then on its capture indices are further ahead.
    void restart(std::size_t index, std::uint64_t samples)
    {
        Track& track = tracks.at(index);
        std::vector<float> silence(samples);
        std::array<const float*, 2> channels{silence.data(), silence.data()};
        track.engine->captureRing().write(channels.data(), 2, static_cast<int>(samples));
        track.engine->timeline().advance({.playing = true, .tempoValid = true, .tempo = tempo},
                                         static_cast<int>(samples), rate);
        track.offset += samples;
    }

    // From host sample `sample` on, the transport plays from `quarterNotes`.
    void jumpAt(std::uint64_t sample, double quarterNotes)
    {
        jumpSample = sample;
        jumpTo = quarterNotes;
    }

    // From host sample `sample` on, the transport plays at `newTempo` in `newNumerator`/4, its
    // position going on.
    void changeAt(std::uint64_t sample, double newTempo, int newNumerator)
    {
        changeSample = sample;
        laterTempo = newTempo;
        laterNumerator = newNumerator;
    }

    [[nodiscard]] bool changed(std::uint64_t sample) const
    {
        return changeSample && sample >= *changeSample;
    }

    [[nodiscard]] double quarterNotesAt(std::uint64_t sample) const
    {
        if (jumpSample && sample >= *jumpSample) {
            return jumpTo + static_cast<double>(sample - *jumpSample) / rate * tempo / 60.0;
        }
        if (changed(sample)) {
            return static_cast<double>(*changeSample) / rate * tempo / 60.0 +
                   static_cast<double>(sample - *changeSample) / rate * laterTempo / 60.0;
        }
        return static_cast<double>(sample) / rate * tempo / 60.0;
    }

    [[nodiscard]] std::uint64_t nanos() const
    {
        return static_cast<std::uint64_t>(std::llround(time * 1e9));
    }

    struct Drawn
    {
        double seconds = 0.0;
        DecodedFrame frame;
    };

    double rate;
    std::uint64_t block;
    double tempo;
    InstanceRegistry registry;
    std::vector<Track> tracks;
    std::unique_ptr<Analyzer> analyzer;
    double time = kStart;
    double nextDraw = kStart;
    std::vector<Drawn> frames;

private:
    void writeBlock(Track& track)
    {
        std::vector<float> ramp(block);
        for (std::size_t i = 0; i < ramp.size(); ++i) {
            ramp[i] = static_cast<float>(track.played + i);
        }
        std::array<const float*, 2> channels{ramp.data(), ramp.data()};
        Engine& engine = *track.engine;
        engine.captureRing().write(channels.data(), 2, static_cast<int>(block));
        engine.timeline().advance({.playing = true,
                                   .positionValid = true,
                                   .quarterNotes = quarterNotesAt(track.played),
                                   .tempoValid = true,
                                   .tempo = changed(track.played) ? laterTempo : tempo,
                                   .timeSignatureValid = true,
                                   .numerator = changed(track.played) ? laterNumerator : 4},
                                  static_cast<int>(block), rate);
        track.played += block;
        engine.blockStamp().publish(
            {.index = track.offset + track.played, .nanos = nanos(), .paced = true});
    }

    std::optional<std::uint64_t> jumpSample;
    double jumpTo = 0.0;
    std::optional<std::uint64_t> changeSample;
    double laterTempo = 0.0;
    int laterNumerator = 4;
};

std::vector<EdgeDraw> playheads(Engine& engine)
{
    std::vector<EdgeDraw> draws;
    for (const Engine::EdgeRecord& record : engine.takeEdges()) {
        draws.push_back({.seconds = static_cast<double>(record.nanos) / 1e9,
                         .edge = static_cast<double>(record.edge),
                         .newest = static_cast<double>(record.newest)});
    }
    return draws;
}

// Lane `lane`'s points in a frame: (min, max) columns, or raw samples for a sweep shorter than
// the screen's columns.
std::span<const float> laneColumns(const DecodedFrame& frame, std::size_t lane)
{
    const std::size_t perPoint = frame.mode == protocol::FrameMode::kRaw ? 1 : 2;
    const std::size_t perLane = std::size_t{frame.pointCount} * perPoint;
    return std::span(frame.data).subspan(lane * perLane, perLane);
}

// The newest host sample a lane draws: its playhead, as the ramp holds host time.
float newestDrawn(const DecodedFrame& frame, std::size_t lane = 0)
{
    return std::ranges::max(laneColumns(frame, lane));
}

// Whether host sample `sample`, played at quarter note `quarterNotes`, sits where it was played
// in a one-beat sweep: in the column at fraction `quarterNotes mod 1` of the lane.
bool sitsWherePlayed(const DecodedFrame& frame, std::size_t lane, std::uint64_t sample,
                     double quarterNotes)
{
    const auto columns = laneColumns(frame, lane);
    const std::size_t count = columns.size() / 2;
    const double fraction = quarterNotes - std::floor(quarterNotes);
    const auto column =
        std::min(count - 1, static_cast<std::size_t>(fraction * static_cast<double>(count)));
    const auto value = static_cast<float>(sample);
    return columns[column * 2] <= value && columns[column * 2 + 1] >= value - 12.0f;
}

// The playhead follows the paced edge: per frame it moves as far as the time drawn, within a
// millisecond, never past the audio captured or backwards, and about a block behind it. The sweep
// holds no audio newer than the playhead, so nothing new shows in the dimmed previous cycle. With
// the newest sample as the playhead, 10.7 ms blocks at 60 Hz frames moved it by one block or two.
TEST(PacedSweep, MovesThePlayheadEvenlyWithNothingNewBeyondIt)
{
    for (const double rate : kSampleRates) {
        SCOPED_TRACE(rate);
        // Blocks of about 10.7 ms at every rate.
        const auto block = static_cast<std::uint64_t>(std::llround(512.0 * rate / 48000.0));
        BeatsHost host(rate, block);
        host.play(3.0); // The clock locks.
        (void)host.tracks.front().engine->takeEdges();
        host.frames.clear();
        host.play(4.0);
        const auto draws = playheads(*host.tracks.front().engine);
        ASSERT_GT(draws.size(), 200U);
        const Motion motion = measureMotion(draws, host.rate);
        EXPECT_LE(motion.advanceErrorP99, 1.0);
        EXPECT_FALSE(motion.ahead);
        EXPECT_FALSE(motion.backwards);
        EXPECT_GT(motion.lagMax, 512.0 / 48.0 * 0.5) << "behind the newest audio";
        ASSERT_EQ(host.frames.size(), draws.size());
        for (std::size_t index = 0; index < draws.size(); ++index) {
            // The ramp holds float values: a sample's own index to within its precision.
            ASSERT_LT(static_cast<double>(newestDrawn(host.frames[index].frame)),
                      draws[index].edge + 1.0 + draws[index].edge * 1e-7)
                << index;
        }
    }
}

// The paced playhead moves on every analysis step, but the sweeps are updated only as audio
// arrives and for each frame: with 42.7 ms blocks, about 23 blocks and 60 frames a second against
// 500 steps, in Single and in Multi. A block whose musical position only continues the one before
// is placed once. Updated on every step, seven lanes used three times the analysis time per frame.
TEST(PacedSweep, UpdatesTheSweepsOnlyAsAudioArrivesAndForEachFrame)
{
    for (const auto mode : {protocol::DisplayMode::kSingle, protocol::DisplayMode::kMulti}) {
        SCOPED_TRACE(static_cast<int>(mode));
        const std::size_t tracks = mode == protocol::DisplayMode::kSingle ? 1 : 3;
        BeatsHost host(48000.0, 2048, tracks, mode);
        host.play(3.0); // The clock locks.
        ASSERT_GT(host.analyzer->work().sweepUpdates, 0U);
        const std::uint64_t before = host.analyzer->work().sweepUpdates;
        const std::size_t framesBefore = host.frames.size();
        const std::uint64_t playedBefore = host.tracks.front().played;
        host.play(2.0);
        const std::uint64_t updates = host.analyzer->work().sweepUpdates - before;
        const std::uint64_t blocks = (host.tracks.front().played - playedBefore) / host.block;
        const std::uint64_t frames = host.frames.size() - framesBefore;
        ASSERT_GT(frames, 100U);
        EXPECT_LE(updates, (blocks + frames + 2) * tracks) << "updated between blocks";
        // Each frame still draws every lane's playhead further on.
        for (std::size_t index = framesBefore + 1; index < host.frames.size(); ++index) {
            for (std::size_t lane = 0; lane < tracks; ++lane) {
                EXPECT_GT(newestDrawn(host.frames[index].frame, lane),
                          newestDrawn(host.frames[index - 1].frame, lane))
                    << "frame " << index << ", lane " << lane;
            }
        }
    }
}

// While the page asks for no frames (hidden or busy), the sweeps still take audio as it arrives,
// so a sweep longer than the capture holds shows all of it when frames come again: no column
// holds audio older than one sweep. A four-bar sweep at 120 BPM is 8 s; the capture keeps 4.25 s.
TEST(PacedSweep, PlacesAudioWhileTheFramesStop)
{
    for (const auto mode : {protocol::DisplayMode::kSingle, protocol::DisplayMode::kMulti}) {
        SCOPED_TRACE(static_cast<int>(mode));
        BeatsHost host(48000.0, 512, mode == protocol::DisplayMode::kSingle ? 1 : 2, mode);
        for (const BeatsHost::Track& track : host.tracks) {
            track.engine->settings().set(SettingId::kBeatSpan,
                                         static_cast<double>(protocol::BeatSpan::kFourBars));
        }
        host.play(3.0); // The clock locks.
        host.play(9.0, false);
        host.frames.clear();
        host.play(0.05);
        ASSERT_FALSE(host.frames.empty());
        const DecodedFrame& frame = host.frames.back().frame;
        const float sweep = 8.0F * 48000.0F;
        for (std::size_t lane = 0; lane < host.tracks.size(); ++lane) {
            const auto columns = laneColumns(frame, lane);
            EXPECT_GT(std::ranges::min(columns), newestDrawn(frame, lane) - sweep - 1000.0F)
                << "lane " << lane << " shows audio from an earlier sweep";
        }
    }
}

// A sweep shorter than pacedSpanMinimumSeconds is new across its width every frame: it is drawn
// at the newest audio, as a short Seconds span is, in Single and in Multi, where the other lanes
// follow. Just above the minimum it is paced, behind the newest audio. A sixteenth of a beat is
// 18.75 ms at 200 BPM and 22.06 ms at 170 BPM.
TEST(PacedSweep, ShowsASweepShorterThanTheMinimumAtTheNewestAudio)
{
    for (const double tempo : {200.0, 170.0}) {
        const bool shorter = 60.0 / tempo / 16.0 < protocol::kPacedSpanMinimumSeconds;
        for (const auto mode : {protocol::DisplayMode::kSingle, protocol::DisplayMode::kMulti}) {
            SCOPED_TRACE(std::to_string(tempo) + " BPM, mode " +
                         std::to_string(static_cast<int>(mode)));
            const std::size_t tracks = mode == protocol::DisplayMode::kSingle ? 1 : 2;
            BeatsHost host(48000.0, 512, tracks, mode, 0, tempo);
            for (const BeatsHost::Track& track : host.tracks) {
                track.engine->settings().set(
                    SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kSixteenthBeat));
            }
            host.play(3.0);
            std::size_t checked = 0;
            for (const EdgeDraw& draw : playheads(*host.tracks.front().engine)) {
                if (draw.seconds > 2.0) {
                    if (shorter) {
                        ASSERT_EQ(draw.edge, draw.newest) << draw.seconds;
                    } else {
                        ASSERT_LT(draw.edge, draw.newest) << "paced at " << draw.seconds;
                    }
                    ++checked;
                }
            }
            EXPECT_GT(checked, 30U);
            const DecodedFrame& frame = host.frames.back().frame;
            ASSERT_EQ(frame.laneCount, mode == protocol::DisplayMode::kSingle ? 0 : tracks);
            for (std::size_t lane = 1; lane < tracks; ++lane) {
                EXPECT_NEAR(newestDrawn(frame, lane), newestDrawn(frame, 0), 0.001 * host.rate)
                    << "lane " << lane;
            }
        }
    }
}

// Audio from before the transport jumped stays where it was played: until the playhead reaches
// the jump, samples are placed by the musical position of their own block, not the newer one. A
// one-beat sweep at 120 BPM (24000 samples); the transport jumps back a quarter of a beat, so the
// newer position would put them elsewhere. In Single and in each lane of Multi, with a lane whose
// blocks come 3 ms later and whose capture started 777 samples earlier. With 64-sample blocks the
// playhead is several blocks behind the newest audio: each block's position is seen all the same.
// At 384 kHz too, with 4096-sample blocks.
TEST(PacedSweep, PlacesAudioFromBeforeAJumpWhereItWasPlayed)
{
    struct JumpCase
    {
        protocol::DisplayMode mode;
        std::uint64_t block;
        double rate;
    };
    for (const JumpCase c : {JumpCase{protocol::DisplayMode::kSingle, 512, 48000.0},
                             JumpCase{protocol::DisplayMode::kMulti, 512, 48000.0},
                             JumpCase{protocol::DisplayMode::kSingle, 64, 48000.0},
                             JumpCase{protocol::DisplayMode::kMulti, 64, 48000.0},
                             JumpCase{protocol::DisplayMode::kSingle, 4096, 384000.0},
                             JumpCase{protocol::DisplayMode::kMulti, 4096, 384000.0}}) {
        SCOPED_TRACE(std::string(protocol::kDisplayModeNames.at(static_cast<std::size_t>(c.mode))) +
                     " " + std::to_string(c.block) + " at " + std::to_string(c.rate));
        const std::size_t tracks = c.mode == protocol::DisplayMode::kSingle ? 1 : 2;
        BeatsHost host(c.rate, c.block, tracks, c.mode, 777);
        if (tracks == 2) {
            host.tracks[1].lateSeconds = 0.003;
        }
        host.play(3.0);
        const std::uint64_t jump = (host.tracks.front().played / c.block + 20) * c.block;
        host.jumpAt(jump, host.quarterNotesAt(jump) - 0.25);
        // Until each lane's playhead has just passed the jump: later audio comes back over the
        // same place a quarter beat (0.125 s) on.
        const auto quarterBeat = static_cast<std::uint64_t>(0.125 * c.rate);
        const auto passed = [&] {
            if (host.frames.empty()) {
                return false;
            }
            for (std::size_t lane = 0; lane < tracks; ++lane) {
                if (newestDrawn(host.frames.back().frame, lane) <
                    static_cast<float>(jump + quarterBeat / 6)) {
                    return false;
                }
            }
            return true;
        };
        while (!passed()) {
            host.frames.clear();
            host.play(1.0 / 60.0);
        }
        const DecodedFrame& frame = host.frames.back().frame;
        const std::uint64_t after = jump + quarterBeat / 12;
        for (std::size_t lane = 0; lane < tracks; ++lane) {
            ASSERT_LT(newestDrawn(frame, lane), static_cast<float>(jump + quarterBeat));
            EXPECT_TRUE(sitsWherePlayed(frame, lane, jump - 1, host.quarterNotesAt(jump - 1)))
                << "lane " << lane << ": the last sample before the jump";
            EXPECT_TRUE(sitsWherePlayed(frame, lane, after, host.quarterNotesAt(after)))
                << "lane " << lane << ": audio after the jump";
        }
    }
}

// A grouped track whose host stops sending blocks (suspended while silent, or bypassed) does not
// hold the others back: its lane stops at its newest audio, and this track's playhead stays about
// a block behind its own audio, not the largest delay. A lane's audio only late is covered (see
// KeepsTheLanesPlayheadsTogether).
TEST(PacedSweep, LeavesAStoppedTrackBehind)
{
    BeatsHost host(48000.0, 512, 2, protocol::DisplayMode::kMulti);
    host.play(3.0);
    host.tracks[1].lateSeconds = 1e6; // No more blocks.
    host.play(2.0);
    (void)host.tracks.front().engine->takeEdges();
    host.play(1.0);
    const auto draws = playheads(*host.tracks.front().engine);
    ASSERT_GT(draws.size(), 50U);
    const Motion motion = measureMotion(draws, host.rate);
    EXPECT_LT(motion.lagMax, 4.0 * 512.0 / 48.0) << "ms behind its own audio";
    EXPECT_LE(motion.advanceErrorP99, 1.0);
}

// Frozen while the transport jumps, then live again: the audio played meanwhile is placed by
// where it was played, before the jump and after it, not by the position the sweep was placed by
// before the freeze nor by the newest. The audio half a beat before the jump (one-beat sweep) is
// where no audio after it reaches by the time of the check.
TEST(PacedSweep, PlacesAudioPlayedDuringAFreezeWhereItWasPlayed)
{
    for (const auto mode : {protocol::DisplayMode::kSingle, protocol::DisplayMode::kMulti}) {
        SCOPED_TRACE(static_cast<int>(mode));
        const std::size_t tracks = mode == protocol::DisplayMode::kSingle ? 1 : 2;
        BeatsHost host(48000.0, 512, tracks, mode);
        host.play(3.0);
        host.analyzer->setFrozen(true);
        host.play(0.4);
        const std::uint64_t jump = (host.tracks.front().played / 512 + 4) * 512;
        host.jumpAt(jump, host.quarterNotesAt(jump) - 0.375);
        host.play(0.3);
        host.analyzer->setFrozen(false);
        host.frames.clear();
        host.play(0.05);
        ASSERT_FALSE(host.frames.empty());
        const DecodedFrame& frame = host.frames.back().frame;
        ASSERT_FALSE(frame.has(FrameFlag::kFrozen));
        const std::uint64_t after = jump + 4096;
        const std::uint64_t before = jump - 12000;
        for (std::size_t lane = 0; lane < tracks; ++lane) {
            ASSERT_GT(newestDrawn(frame, lane), static_cast<float>(after));
            EXPECT_TRUE(sitsWherePlayed(frame, lane, after, host.quarterNotesAt(after)))
                << "lane " << lane << ": audio played after the jump, while frozen";
            EXPECT_TRUE(sitsWherePlayed(frame, lane, before, host.quarterNotesAt(before)))
                << "lane " << lane << ": audio played before the jump, while frozen";
        }
    }
}

// Frozen just after a transport jump the playhead has not reached yet, the frozen sweep holds
// the audio up to the newest, each block where it was played: the audio between the playhead and
// the jump, and the jump's own. 2048-sample blocks, later from 3 s by 60 ms (absorbed, not a
// jump), so the delay rises past a block and the playhead trails the jump when it arrives.
TEST(PacedSweep, FreezesAJumpThePlayheadHadNotReachedWhereItWasPlayed)
{
    for (const auto mode : {protocol::DisplayMode::kSingle, protocol::DisplayMode::kMulti}) {
        SCOPED_TRACE(static_cast<int>(mode));
        const std::size_t tracks = mode == protocol::DisplayMode::kSingle ? 1 : 2;
        BeatsHost host(48000.0, 2048, tracks, mode);
        host.play(3.0);
        for (BeatsHost::Track& track : host.tracks) {
            track.lateSeconds = 0.06;
        }
        host.play(2.0);
        const std::uint64_t jump = (host.tracks.front().played / 2048 + 4) * 2048;
        host.jumpAt(jump, host.quarterNotesAt(jump) - 0.375);
        while (host.tracks.back().played < jump + 2048) {
            host.play(0.0005);
        }
        // A frame now, then the freeze at once: the playhead is where this frame drew it.
        (void)host.tracks.front().engine->takeEdges();
        std::vector<std::byte> bytes;
        ASSERT_TRUE(host.analyzer->produce(bytes));
        const auto playhead =
            static_cast<std::uint64_t>(playheads(*host.tracks.front().engine).back().edge);
        ASSERT_LT(playhead + 64, jump) << "the playhead had not reached the jump";
        host.analyzer->setFrozen(true);
        ASSERT_TRUE(host.analyzer->produce(bytes));
        const DecodedFrame frame = decode(bytes);
        ASSERT_TRUE(frame.has(FrameFlag::kFrozen));
        for (std::size_t lane = 0; lane < tracks; ++lane) {
            for (const std::uint64_t sample : {(playhead + jump) / 2, jump + 256}) {
                EXPECT_TRUE(sitsWherePlayed(frame, lane, sample, host.quarterNotesAt(sample)))
                    << "lane " << lane << ", sample " << sample;
            }
        }
    }
}

// Lengthened from a sweep drawn at the newest audio to one that is paced, the playhead goes on
// from the newest audio it showed: it may wait for the paced edge to catch up, but never steps
// back. With 93 ms blocks the delay is longer than a frame.
TEST(PacedSweep, NeverStepsBackWhenPacingStarts)
{
    BeatsHost host(44100.0, 4096, 1, protocol::DisplayMode::kSingle, 0, 200.0);
    Engine& engine = *host.tracks.front().engine;
    engine.settings().set(SettingId::kBeatSpan,
                          static_cast<double>(protocol::BeatSpan::kSixteenthBeat));
    host.play(3.0);
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBeat));
    host.play(1.0);
    EXPECT_FALSE(measureMotion(playheads(engine), host.rate).backwards);
}

// A tempo or metre change reaches the sweep when the playhead does: until then the cycle keeps
// its length, and from there it takes the new one. 2048-sample blocks, later from 3 s by 60 ms
// (absorbed), so the playhead trails the newest audio by more than a block. A one-beat sweep from
// 120 to 100 BPM (0.5 s to 0.6 s), and a one-bar sweep from 4/4 to 3/4 (2 s to 1.5 s).
TEST(PacedSweep, ChangesTheCycleWhenThePlayheadReachesTheChange)
{
    struct Change
    {
        std::string name;
        protocol::BeatSpan span;
        double tempo;
        int numerator;
        double before;
        double after;
    };
    for (const Change& change : {Change{.name = "tempo",
                                        .span = protocol::BeatSpan::kOneBeat,
                                        .tempo = 100.0,
                                        .numerator = 4,
                                        .before = 0.5,
                                        .after = 0.6},
                                 Change{.name = "metre",
                                        .span = protocol::BeatSpan::kOneBar,
                                        .tempo = 120.0,
                                        .numerator = 3,
                                        .before = 2.0,
                                        .after = 1.5}}) {
        SCOPED_TRACE(change.name);
        BeatsHost host(48000.0, 2048);
        Engine& engine = *host.tracks.front().engine;
        engine.settings().set(SettingId::kBeatSpan, static_cast<double>(change.span));
        host.play(3.0);
        host.tracks.front().lateSeconds = 0.06;
        host.play(2.0);
        const std::uint64_t at = (host.tracks.front().played / 2048 + 4) * 2048;
        host.changeAt(at, change.tempo, change.numerator);
        while (host.tracks.front().played < at + 2048) {
            host.play(0.0005);
        }
        (void)engine.takeEdges();
        std::vector<std::byte> bytes;
        ASSERT_TRUE(host.analyzer->produce(bytes));
        ASSERT_LT(playheads(engine).back().edge, static_cast<double>(at))
            << "the playhead had not reached the change";
        // The cycle's length: the samples a sweep shows, not zoomed.
        const auto cycle = [&](const DecodedFrame& frame) {
            return frame.visibleSamples / host.rate;
        };
        EXPECT_NEAR(cycle(decode(bytes)), change.before, 1e-3) << "before the playhead";
        host.frames.clear();
        host.play(0.2);
        ASSERT_FALSE(host.frames.empty());
        EXPECT_NEAR(cycle(host.frames.back().frame), change.after, 1e-3) << "reached";
    }
}

// A track whose host stops calling it (suspended or bypassed) stops its own playhead, at its
// newest audio; the other tracks' lanes, still playing, go on with theirs.
TEST(PacedSweep, KeepsTheLanesGoingWhenThisTrackStops)
{
    BeatsHost host(48000.0, 512, 2, protocol::DisplayMode::kMulti);
    host.play(3.0);
    host.tracks.front().lateSeconds = 1e6; // No more blocks for this track.
    host.play(1.0);
    ASSERT_FALSE(host.frames.empty());
    const DecodedFrame& frame = host.frames.back().frame;
    EXPECT_GT(newestDrawn(frame, 1), static_cast<float>(host.tracks[1].played) - 0.15F * 48000.0F)
        << "the lane stopped with this track";
}

// Audio played while the view was not placing the sweeps live (in Seconds, or in the other
// display mode) is placed by where it was played once it is again, also across a transport jump
// made meanwhile: the positions seen before are forgotten, not taken for the newer audio.
TEST(PacedSweep, PlacesAudioPlayedWhileAwayWhereItWasPlayed)
{
    using protocol::DisplayMode;
    using protocol::TimeBase;
    struct Away
    {
        std::string name;
        DisplayMode mode;
        std::optional<TimeBase> timeBase;
        std::optional<DisplayMode> otherMode;
    };
    for (const Away& away : {Away{.name = "Single in Seconds",
                                  .mode = DisplayMode::kSingle,
                                  .timeBase = TimeBase::kSeconds,
                                  .otherMode = std::nullopt},
                             Away{.name = "Multi in Seconds",
                                  .mode = DisplayMode::kMulti,
                                  .timeBase = TimeBase::kSeconds,
                                  .otherMode = std::nullopt},
                             Away{.name = "Multi in Single",
                                  .mode = DisplayMode::kMulti,
                                  .timeBase = std::nullopt,
                                  .otherMode = DisplayMode::kSingle},
                             Away{.name = "Single in Multi",
                                  .mode = DisplayMode::kSingle,
                                  .timeBase = std::nullopt,
                                  .otherMode = DisplayMode::kMulti}}) {
        SCOPED_TRACE(away.name);
        BeatsHost host(48000.0, 512, 2, away.mode);
        const auto set = [&](SettingId id, double value) {
            for (const BeatsHost::Track& track : host.tracks) {
                track.engine->settings().set(id, value);
            }
        };
        host.play(3.0);
        if (away.timeBase) {
            set(SettingId::kTimeBase, static_cast<double>(*away.timeBase));
        } else {
            set(SettingId::kDisplayMode, static_cast<double>(*away.otherMode));
        }
        host.play(0.2);
        const std::uint64_t jump = (host.tracks.front().played / 512 + 4) * 512;
        host.jumpAt(jump, host.quarterNotesAt(jump) - 0.375);
        host.play(0.3);
        set(SettingId::kTimeBase, static_cast<double>(TimeBase::kBeats));
        set(SettingId::kDisplayMode, static_cast<double>(away.mode));
        host.frames.clear();
        host.play(0.1);
        ASSERT_FALSE(host.frames.empty());
        const DecodedFrame& frame = host.frames.back().frame;
        const std::uint64_t after = jump + 4096;
        const std::size_t lanes = away.mode == DisplayMode::kSingle ? 1 : 2;
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            ASSERT_GT(newestDrawn(frame, lane), static_cast<float>(after));
            EXPECT_TRUE(sitsWherePlayed(frame, lane, after, host.quarterNotesAt(after)))
                << "lane " << lane << ": audio played after the jump, while away";
        }
    }
}

// A track whose processing restarted captures more than the host played, so its capture indices
// move further ahead of this track's: the lanes' offset is measured again, and once the new one
// has lasted, the lane's playhead is with this track's again, within a millisecond. At every
// rate, with blocks of about 10.7 ms.
TEST(PacedSweep, FollowsATrackWhoseProcessingRestarted)
{
    for (const double rate : kSampleRates) {
        SCOPED_TRACE(rate);
        const auto block = static_cast<std::uint64_t>(std::llround(512.0 * rate / 48000.0));
        BeatsHost host(rate, block, 2, protocol::DisplayMode::kMulti);
        host.play(4.0);
        host.restart(1, static_cast<std::uint64_t>(0.05 * rate));
        host.frames.clear();
        host.play(3.0);
        std::size_t compared = 0;
        for (const BeatsHost::Drawn& drawn : host.frames) {
            if (drawn.seconds < host.time - 1.0) {
                continue; // The new offset is taken.
            }
            EXPECT_NEAR(newestDrawn(drawn.frame, 1), newestDrawn(drawn.frame, 0), 0.001 * host.rate)
                << "at " << drawn.seconds;
            ++compared;
        }
        EXPECT_GT(compared, 30U);
    }
}

// Other tracks' lanes keep their playheads with this track's on every frame, though their blocks
// arrive 3 ms later and their captures started 16 ms earlier: each lane is placed up to this
// track's playhead's musical position in its own capture, and the delay covers a lane whose audio
// has not arrived yet. At every rate, with blocks of about 10.7 ms; mono and stereo at 48 kHz.
TEST(PacedSweep, KeepsTheLanesPlayheadsTogether)
{
    for (const double rate : kSampleRates) {
        // Mono and stereo at 48 kHz; mono at the other rates.
        for (const int channels : {1, 2}) {
            if (channels == 2 && rate != 48000.0) {
                continue;
            }
            SCOPED_TRACE(std::to_string(rate) + ", " + std::to_string(channels) + " channels");
            const auto block = static_cast<std::uint64_t>(std::llround(512.0 * rate / 48000.0));
            const auto offset = static_cast<std::uint64_t>(std::llround(777.0 * rate / 48000.0));
            BeatsHost host(rate, block, 2, protocol::DisplayMode::kMulti, offset);
            host.tracks[1].lateSeconds = 0.003;
            for (auto& track : host.tracks) {
                track.engine->setInputChannels(channels);
            }
            host.play(6.0);
            std::size_t compared = 0;
            for (const BeatsHost::Drawn& drawn : host.frames) {
                if (drawn.seconds < 4.0) {
                    continue; // The clock locks and the delay settles.
                }
                ASSERT_EQ(drawn.frame.laneCount, 2);
                EXPECT_NEAR(newestDrawn(drawn.frame, 1), newestDrawn(drawn.frame, 0),
                            0.001 * host.rate)
                    << "within a millisecond at " << drawn.seconds;
                ++compared;
            }
            EXPECT_GT(compared, 100U);
        }
    }
}

} // namespace
} // namespace orizon::engine
