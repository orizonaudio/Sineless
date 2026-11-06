// Host events against the analysis: what a host does to a group of instances while the editor is
// open, in any order. The host prepares at a sample rate or changes it while processing, gives a
// track one or two channels, plays audio with a transport, a tempo and a metre, adds and removes
// tracks; the page freezes, sends control messages and asks for frames. Two tracks play the same
// generated signal, the other one negated, so that what one lane shows the other mirrors.
//
// Input: a sequence of steps, read byte by byte (InputReader), each step a byte choosing what
// happens and the bytes after it choosing how; bytes past the end read as zero. Audio is generated
// from a few bytes (a sine, square, DC, silence, impulse or non-finite signal with its frequency,
// level and rare broken samples), so short inputs play seconds of it. The steps play twice, into
// two worlds built the same way, and each frame must be the same in both: a frame that depends on
// anything but its inputs (uninitialised data, a global, the real clock) differs.

#include "FrameDecoding.h"
#include "FrameOracles.h"
#include "FuzzCheck.h"
#include "HostSteps.h"
#include "editor/Session.h"
#include "editor/Transport.h"
#include "engine/Analyzer.h"
#include "engine/CaptureRing.h"
#include "engine/Engine.h"
#include "engine/InstanceRegistry.h"
#include "engine/SettingJson.h"
#include "engine/Timeline.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <numbers>
#include <span>
#include <string>
#include <vector>

using orizon::fuzz::checkFrame;
using orizon::fuzz::require;
using namespace orizon;
using namespace orizon::fuzz::host;

namespace {

// Enough steps for a few freezes, rate changes and tracks coming and going between frames, while
// an input stays well under libFuzzer's timeout at the highest rate, in both worlds.
constexpr std::size_t kMaxSteps = 96;
constexpr std::size_t kMaxFrames = 8;
// Advances observe block stamps without producing a frame, so the paced edge's block clock can
// lock (16 updates, BlockClock.h) within one input.
constexpr std::size_t kMaxAdvances = 32;
constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;

// Reads an input front to back; past its end, every byte is zero.
class InputReader
{
public:
    explicit InputReader(std::span<const std::uint8_t> bytes) : bytes(bytes) {}

    [[nodiscard]] bool done() const { return next >= bytes.size(); }
    std::uint8_t byte() { return next < bytes.size() ? bytes[next++] : 0; }
    std::uint16_t word()
    {
        const unsigned low = byte();
        const unsigned high = byte();
        return static_cast<std::uint16_t>(low | (high << 8U));
    }
    // A value below `count` (at least 1).
    std::size_t below(std::size_t count) { return count <= 256 ? byte() % count : word() % count; }
    bool flag() { return (byte() & 1U) != 0; }
    template <typename T, std::size_t N> T pick(const std::array<T, N>& values)
    {
        return values[below(N)];
    }

private:
    std::span<const std::uint8_t> bytes;
    std::size_t next = 0;
};

struct Signal
{
    SignalKind kind = SignalKind::kSine;
    std::uint32_t hertz = 440;
    float level = 0.5f;
    // Every this many samples, a broken one (NaN, an infinity or a denormal); 0 for none.
    std::uint32_t brokenEvery = 0;
};

struct Step
{
    StepKind kind = StepKind::kFrame;
    Tracks tracks = Tracks::kBoth;
    double rate = 48000.0;
    int channels = 2;
    // Play.
    Signal signal;
    int blocks = 1;
    int blockSize = 512;
    std::uint64_t silent = 0; // Channels flagged silent, the same on both tracks.
    bool paced = true;
    // Transport.
    engine::HostTimeline host;
    // Freeze.
    bool frozen = false;
    // Message.
    std::string message;
    // Wait.
    std::uint64_t nanos = 0;
};

float sampleOf(const Signal& signal, std::uint64_t n, std::uint32_t rate)
{
    if (signal.brokenEvery != 0 && n % signal.brokenEvery == 0) {
        constexpr auto kBroken = std::to_array<float>(
            {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::denorm_min()});
        return kBroken[(n / signal.brokenEvery) % kBroken.size()];
    }
    // The phase is from integers, as in Signals.h: exact however long the signal plays.
    const std::uint64_t phase = (n * signal.hertz) % rate;
    switch (signal.kind) {
    case SignalKind::kSine:
        return static_cast<float>(
            signal.level * std::sin(2.0 * std::numbers::pi * static_cast<double>(phase) / rate));
    case SignalKind::kSquare:
        return phase * 2 < rate ? signal.level : -signal.level;
    case SignalKind::kDc:
        return signal.level;
    case SignalKind::kSilence:
        return 0.0f;
    case SignalKind::kImpulse:
        return phase < signal.hertz ? signal.level : 0.0f;
    case SignalKind::kNonFinite:
        return n % 2 == 0 ? std::numeric_limits<float>::quiet_NaN()
                          : std::numeric_limits<float>::infinity();
    case SignalKind::kCount:
        break;
    }
    return 0.0f;
}

std::string controlMessage(InputReader& input)
{
    nlohmann::json message;
    switch (input.below(7)) {
    case 0: {
        const auto& setting = protocol::kSettings[input.below(protocol::kSettings.size())];
        double value = setting.defaultValue;
        switch (setting.kind) {
        case protocol::SettingKind::kNumber: {
            const auto at = input.byte();
            value = at == 0 ? setting.minimum
                    : at == 255
                        ? setting.maximum
                        : setting.minimum + (setting.maximum - setting.minimum) * at / 255.0;
            break;
        }
        case protocol::SettingKind::kEnum:
            value = static_cast<double>(input.below(setting.valueNames.size()));
            break;
        case protocol::SettingKind::kFlags:
            value = static_cast<double>(
                1 + input.below((std::size_t{1} << setting.valueNames.size()) - 1));
            break;
        }
        message = {{"type", "setSetting"},
                   {"name", setting.name},
                   {"value", engine::settingToJson(setting.id, value)}};
        break;
    }
    case 1:
        message = {{"type", "pan"}, {"seconds", (input.byte() - 32) / 16.0}};
        break;
    case 2: {
        const double start = input.byte() / 255.0;
        message = {{"type", "view"}, {"start", start}, {"end", input.byte() / 255.0}};
        break;
    }
    case 3:
        message = {{"type", "arm"}};
        break;
    case 4: {
        // Lane ids are the tracks' creation order: 2 the first other track, 3 one that joined.
        nlohmann::json ids = nlohmann::json::array();
        const auto mask = input.byte();
        for (unsigned id = 1; id <= 4; ++id) {
            if ((mask & (1U << id)) != 0) {
                ids.push_back(id);
            }
        }
        message = {{"type", "compare"}, {"ids", ids}};
        break;
    }
    case 5:
        message = {{"type", "previewFix"}, {"on", input.flag()}};
        break;
    default:
        message = {{"type", "viewport"},
                   {"viewWidth", 1 + input.word() % 4000},
                   {"pixelRatio", 1 + input.below(3)}};
        break;
    }
    return message.dump();
}

std::vector<Step> readSteps(InputReader& input)
{
    std::vector<Step> steps;
    engine::HostTimeline host{.playing = true,
                              .positionValid = true,
                              .tempoValid = true,
                              .timeSignatureValid = true,
                              .secondsValid = true};
    std::size_t frames = 0;
    std::size_t advances = 0;
    while (!input.done() && steps.size() < kMaxSteps) {
        Step step;
        step.kind = static_cast<StepKind>(input.below(kStepKinds));
        switch (step.kind) {
        case StepKind::kPrepare:
        case StepKind::kRate:
            step.tracks = static_cast<Tracks>(input.below(3));
            step.rate = input.pick(kRates);
            break;
        case StepKind::kChannels:
            step.tracks = static_cast<Tracks>(input.below(3));
            step.channels = input.flag() ? 1 : 2;
            break;
        case StepKind::kPlay:
            step.signal.kind = static_cast<SignalKind>(input.below(kSignalKinds));
            step.signal.hertz = 1 + input.word() % 20000;
            step.signal.level = static_cast<float>(input.byte()) / 128.0f;
            if (input.below(8) == 0) {
                step.signal.brokenEvery = 1 + input.word();
            }
            step.blocks = 1 + static_cast<int>(input.below(8));
            step.blockSize = static_cast<int>(32U << input.below(8)); // 32 to 4,096 samples.
            step.silent = input.below(4) == 0 ? input.below(4) : 0;
            step.paced = input.below(8) != 0;
            break;
        case StepKind::kTransport: {
            const auto flags = input.byte();
            host.playing = (flags & 1U) != 0;
            host.positionValid = (flags & 2U) == 0;
            host.tempoValid = (flags & 4U) == 0;
            host.timeSignatureValid = (flags & 8U) == 0;
            host.secondsValid = (flags & 16U) == 0;
            if ((flags & 32U) != 0) { // A jump: a loop, a relocation.
                host.quarterNotes = input.word() / 16.0 - 64.0;
                host.seconds = host.quarterNotes / 2.0;
            }
            host.tempo = (flags & 64U) != 0 ? input.pick(kTempi) : 20.0 + input.word() % 980;
            host.numerator = 1 + static_cast<int>(input.below(16));
            host.denominator = input.pick(kDenominators);
            step.host = host;
            break;
        }
        case StepKind::kLeave:
        case StepKind::kJoin:
        case StepKind::kAdvance:
            break;
        case StepKind::kFreeze:
            step.frozen = input.flag();
            break;
        case StepKind::kMessage:
            step.message = controlMessage(input);
            break;
        case StepKind::kWait:
            step.nanos = (1 + input.below(200)) * kNanosPerSecond / 1000;
            break;
        case StepKind::kFrame:
        case StepKind::kCount:
            break;
        }
        if ((step.kind == StepKind::kFrame && frames++ >= kMaxFrames) ||
            (step.kind == StepKind::kAdvance && advances++ >= kMaxAdvances)) {
            continue;
        }
        steps.push_back(step);
    }
    steps.emplace_back(); // The input ends with a frame: a Step is one by default.
    return steps;
}

class CheckingTransport final : public editor::Transport
{
public:
    void sendMessage(const nlohmann::json& message) override
    {
        require(message.is_object() && message.contains("type") && message["type"].is_string(),
                "every message to the page is an object with a string type");
    }
    void sendFrame(std::span<const std::byte> /*frame*/) override {}
};

// One host with this track's editor open, and one other track of its group.
class World
{
public:
    World() : own(std::make_shared<engine::Engine>()), other(std::make_shared<engine::Engine>())
    {
        for (const auto& engine : {own, other}) {
            engine->prepare(48000.0);
            registry.add(engine);
        }
        analyzer.setClock([this] { return clock; });
        session.receive(nlohmann::json{{"type", "hello"},
                                       {"protocolVersion", protocol::kProtocolVersion},
                                       {"viewWidth", 800},
                                       {"pixelRatio", 1}});
    }
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;
    ~World() = default;

    // Applies one step; for a Frame, returns whether a frame was produced into `frame`.
    bool apply(const Step& step, std::vector<std::byte>& frame)
    {
        switch (step.kind) {
        case StepKind::kPrepare:
            forTracks(step.tracks, [&](engine::Engine& engine) { engine.prepare(step.rate); });
            break;
        case StepKind::kRate:
            // As Processor::setupProcessing while processing: the capture keeps its size.
            forTracks(step.tracks,
                      [&](engine::Engine& engine) { engine.setSampleRate(step.rate); });
            break;
        case StepKind::kChannels:
            forTracks(step.tracks,
                      [&](engine::Engine& engine) { engine.setInputChannels(step.channels); });
            break;
        case StepKind::kPlay:
            play(step);
            break;
        case StepKind::kTransport:
            host = step.host;
            break;
        case StepKind::kLeave:
            if (joined) {
                registry.remove(*other);
                joined = false;
                diverged = true;
            }
            break;
        case StepKind::kJoin:
            if (!joined) {
                other = std::make_shared<engine::Engine>();
                other->prepare(own->sampleRate());
                other->setInputChannels(own->inputChannels());
                registry.add(other);
                joined = true;
                diverged = true; // It holds none of the audio played before.
            }
            break;
        case StepKind::kFreeze:
            analyzer.setFrozen(step.frozen);
            break;
        case StepKind::kMessage:
            if (const auto message = editor::WebViewTransport::parse(step.message)) {
                session.receive(*message);
            }
            break;
        case StepKind::kWait:
            clock += step.nanos;
            break;
        case StepKind::kAdvance:
            followGroup();
            analyzer.advance();
            break;
        case StepKind::kFrame:
            followGroup();
            return analyzer.produce(frame);
        case StepKind::kCount:
            break;
        }
        session.sync();
        return false;
    }

    // Draws the next frame whatever has changed, as for a page that has just loaded.
    void redraw() { analyzer.redraw(); }

    [[nodiscard]] bool comparing() const { return engine::compareShown(own->settings().get()); }

    [[nodiscard]] protocol::TimeBase timeBase() const
    {
        return static_cast<protocol::TimeBase>(
            own->settings().get().value(protocol::SettingId::kTimeBase));
    }

    // Whether a step has treated the two tracks differently, so that their audio may differ.
    [[nodiscard]] bool tracksDiverged() const { return diverged; }

private:
    template <typename Apply> void forTracks(Tracks tracks, const Apply& apply)
    {
        diverged = diverged || tracks != Tracks::kBoth;
        if (tracks != Tracks::kOther) {
            apply(*own);
        }
        if (tracks != Tracks::kOwn && joined) {
            apply(*other);
        }
    }

    // The other track stays in this track's group, so that Multi and Compare have its lane.
    void followGroup()
    {
        other->settings().set(protocol::SettingId::kGroup,
                              own->settings().get().value(protocol::SettingId::kGroup));
    }

    void play(const Step& step)
    {
        const auto block = static_cast<std::size_t>(step.blockSize);
        std::vector<float> samples(block);
        std::vector<float> negated(block);
        for (int b = 0; b < step.blocks; ++b) {
            const double rate = own->sampleRate();
            const auto integerRate = static_cast<std::uint32_t>(rate);
            for (std::size_t i = 0; i < block; ++i) {
                samples[i] = sampleOf(step.signal, played + i, integerRate);
                negated[i] = -samples[i];
            }
            played += block;
            clock +=
                static_cast<std::uint64_t>(static_cast<double>(block) * kNanosPerSecond / rate);
            playInto(*own, samples, step);
            if (joined) {
                playInto(*other, negated, step);
            }
            if (host.playing) {
                host.quarterNotes += static_cast<double>(block) * host.tempo / (60.0 * rate);
                host.seconds += static_cast<double>(block) / rate;
            }
        }
    }

    // As a plugin's process(): capture the block, stamp it (on the simulated clock, so that what
    // a frame shows depends on the input alone), and advance the musical clock.
    void playInto(engine::Engine& engine, const std::vector<float>& samples, const Step& step)
    {
        const std::array<const float*, 2> channels{samples.data(), samples.data()};
        engine::captureBlock(engine.captureRing(), channels.data(), engine.inputChannels(),
                             step.silent, step.blockSize);
        engine.blockStamp().publish(
            {.index = engine.captureRing().writeIndex(), .nanos = clock, .paced = step.paced});
        engine.timeline().advance(host, step.blockSize, engine.sampleRate());
    }

    engine::InstanceRegistry registry;
    std::shared_ptr<engine::Engine> own;
    std::shared_ptr<engine::Engine> other;
    bool joined = true;
    bool diverged = false;
    // The simulated clock, from 1 s as the paced tests start it.
    std::uint64_t clock = kNanosPerSecond;
    std::uint64_t played = 0;
    engine::HostTimeline host{.playing = true,
                              .positionValid = true,
                              .tempoValid = true,
                              .timeSignatureValid = true,
                              .secondsValid = true};
    engine::Analyzer analyzer{*own, registry};
    CheckingTransport transport;
    editor::Session session{*own, analyzer, transport};
};

} // namespace

// The entry point libFuzzer calls, by its name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    InputReader input({data, size});
    const std::vector<Step> steps = readSteps(input);

    World first;
    World second;
    std::vector<std::byte> firstFrame;
    std::vector<std::byte> secondFrame;
    // The newest frame drawn, and whether only format changes have happened since: a frozen frame
    // drawn again must then be the same (#123).
    std::vector<std::byte> previous;
    bool onlyFormatChanges = false;
    for (const Step& step : steps) {
        const bool formatChange = step.kind == StepKind::kPrepare || step.kind == StepKind::kRate;
        bool redrawn = false;
        if (step.kind == StepKind::kFrame && onlyFormatChanges &&
            engine::decode(previous).has(protocol::FrameFlag::kFrozen) && !first.comparing()) {
            // In Compare, freezing restarts the match search, which ends over frames: a frozen
            // Compare frame changes when it does.
            first.redraw();
            second.redraw();
            redrawn = true;
        }
        const bool drew = first.apply(step, firstFrame);
        const bool drewAgain = second.apply(step, secondFrame);
        if (step.kind != StepKind::kFrame) {
            onlyFormatChanges = onlyFormatChanges && formatChange;
            continue;
        }
        require(drew == drewAgain && (!drew || firstFrame == secondFrame),
                "the same host events draw the same frames");
        orizon::fuzz::counted("determinism");
        if (redrawn) {
            require(drew && firstFrame == previous,
                    "a frozen frame drawn again after a format change is the same");
            orizon::fuzz::counted("frozen");
        }
        if (!drew) {
            continue;
        }
        orizon::fuzz::counted("drawn");
        checkFrame(firstFrame);
        const engine::DecodedFrame frame = engine::decode(firstFrame);
        orizon::fuzz::checkReadouts(frame);
        // In Trigger and Free, live: there every lane shows the time window this track places
        // (FR-14.4). In Beats, lanes may legitimately stand apart (before the tracks share a host
        // position, AD-15; while blocks have stopped, AD-29; on another sweep length), which
        // PacedSweepTests covers state by state. Frozen, a track's lane is held when a Multi frame
        // first needs it, which may be after the rate changed, and a lane held at another rate is
        // drawn silent (FR-14.4).
        if (frame.laneCount == 2 && !frame.has(protocol::FrameFlag::kCompared) &&
            first.timeBase() != protocol::TimeBase::kBeats &&
            !frame.has(protocol::FrameFlag::kFrozen) && frame.invalidLanes == 0 &&
            frame.invalidSamples == 0 && !first.tracksDiverged()) {
            orizon::fuzz::checkMirroredLanes(frame);
        }
        previous = firstFrame;
        onlyFormatChanges = true;
    }
    return 0;
}
