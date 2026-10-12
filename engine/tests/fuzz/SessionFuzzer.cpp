// The page's control messages against malformed input: whatever text arrives over the webview's
// native channel goes through WebViewTransport::parse and Session::receive, as in the editor, and
// the analyzer then produces frames with the commands they set. Built from the editor's
// SDK-free sources (Session.cpp, Transport.cpp), not the editor library, which needs the VST3 SDK.
//
// Input: one message per line, and an empty line where the analyzer produces a frame, as it does
// after the input's last line. A first line "<sample rate> <channels>" (such as "96000 1") sets
// the audio format; otherwise it is 48 kHz stereo and the first line is a message too. A second
// track in the same group plays the same tone inverted, so the Multi and Compare views have
// lanes to show.

#include "FuzzCheck.h"
#include "editor/Session.h"
#include "editor/Transport.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"
#include "engine/InstanceRegistry.h"

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

using orizon::fuzz::require;
using namespace orizon;

namespace {

// Enough to reach every message type with a few repeats and frames between them, while staying
// fast: a frame of a long sweep takes tens of milliseconds.
constexpr std::size_t kMaxMessages = 16;
constexpr std::size_t kMaxFrames = 4;
constexpr int kToneSamples = 16384;

class CheckingTransport final : public editor::Transport
{
public:
    void sendMessage(const nlohmann::json& message) override
    {
        require(message.is_object() && message.contains("type") && message["type"].is_string(),
                "every message to the page is an object with a string type");
        // The page reads them as text; any value must be writable.
        static_cast<void>(message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }
    void sendFrame(std::span<const std::byte> /*frame*/) override {}
};

struct Format
{
    double sampleRate = 48000.0;
    int channels = 2;
};

// Reads "<rate> <channels>" from the first line; returns nothing if it is not one.
std::optional<Format> parseFormat(std::string_view line)
{
    const char* const begin = std::to_address(line.begin());
    const char* const end = std::to_address(line.end());
    unsigned rate = 0;
    auto parsed = std::from_chars(begin, end, rate);
    if (parsed.ec != std::errc{} || parsed.ptr == end || *parsed.ptr != ' ') {
        return std::nullopt;
    }
    int channels = 0;
    parsed = std::from_chars(parsed.ptr + 1, end, channels);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    // The rates hosts run at, with margin; the capture holds one or two channels.
    if (rate < 8000 || rate > 768000 || channels < 1 || channels > 2) {
        return std::nullopt;
    }
    return Format{.sampleRate = static_cast<double>(rate), .channels = channels};
}

void play(engine::Engine& engine, const Format& format, float sign)
{
    engine.prepare(format.sampleRate);
    engine.setInputChannels(format.channels);
    std::vector<float> tone(kToneSamples);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = sign * ((i / 50) % 2 == 0 ? 0.5f : -0.5f);
    }
    std::array<const float*, 2> channels{tone.data(), tone.data()};
    engine.captureRing().write(channels.data(), format.channels, kToneSamples);
}

// The frame holds exactly the data its header announces (frame.ts).
void checkFrame(const std::vector<std::byte>& frame)
{
    using protocol::FrameHeader;
    require(frame.size() >= FrameHeader::kSize, "a frame holds its header");
    const auto byte = [&](std::size_t offset) { return std::to_integer<unsigned>(frame[offset]); };
    std::uint32_t points = 0;
    std::memcpy(&points, frame.data() + FrameHeader::kPointCountOffset, sizeof(points));
    const auto mode = static_cast<protocol::FrameMode>(byte(FrameHeader::kModeOffset));
    const std::size_t perBlock =
        std::size_t{points} * (mode == protocol::FrameMode::kMinMax ? 2 : 1);
    const auto views = static_cast<std::size_t>(std::popcount(byte(FrameHeader::kViewsOffset)));
    const std::size_t lanes = byte(FrameHeader::kLaneCountOffset);
    const bool compared = (byte(FrameHeader::kFlagsOffset) &
                           static_cast<unsigned>(protocol::FrameFlag::kCompared)) != 0;
    const std::size_t matches = compared && lanes > 0 ? (lanes - 1) * protocol::kMatchValues : 0;
    require(frame.size() == FrameHeader::kSize + ((views + lanes) * perBlock + matches) * 4,
            "a frame's size matches its header");
}

} // namespace

// The entry point libFuzzer calls, by its name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    std::string_view input(reinterpret_cast<const char*>(data), size);
    const auto nextLine = [&input]() {
        const std::size_t end = input.find('\n');
        const std::string_view line = input.substr(0, end);
        input.remove_prefix(end == std::string_view::npos ? input.size() : end + 1);
        return line;
    };

    Format format;
    if (const auto first = parseFormat(input.substr(0, input.find('\n')))) {
        format = *first;
        nextLine();
    }

    engine::InstanceRegistry registry;
    const auto own = std::make_shared<engine::Engine>();
    const auto other = std::make_shared<engine::Engine>();
    registry.add(own);
    registry.add(other);
    play(*own, format, 1.0f);
    play(*other, format, -1.0f);

    engine::Analyzer analyzer(*own, registry);
    CheckingTransport transport;
    editor::Session session(*own, analyzer, transport);
    std::vector<std::byte> frame;
    const auto produce = [&]() {
        other->settings().set(protocol::SettingId::kGroup,
                              own->settings().get().value(protocol::SettingId::kGroup));
        if (analyzer.produce(frame)) {
            checkFrame(frame);
        }
    };

    std::size_t messages = 0;
    std::size_t frames = 0;
    while (!input.empty() && messages < kMaxMessages) {
        const std::string_view line = nextLine();
        if (line.empty()) {
            if (frames++ < kMaxFrames) {
                produce();
            }
            continue;
        }
        ++messages;
        if (const auto message = editor::WebViewTransport::parse(line)) {
            session.receive(*message);
        }
        session.sync();
        const engine::Settings settings = own->settings().get();
        for (const auto& setting : protocol::kSettings) {
            require(engine::Settings::isValid(setting.id, settings.value(setting.id)),
                    "the page cannot set an invalid value");
        }
    }
    produce();
    return 0;
}
