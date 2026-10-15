#include "editor/Session.h"

#include "engine/SettingJson.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace orizon::editor {

using protocol::MessageType;

namespace {

// Widest trace area accepted, in device pixels.
constexpr double kMaxColumns = 16384.0;

std::optional<MessageType> messageType(const nlohmann::json& message)
{
    const auto& name = message["type"].get_ref<const std::string&>();
    const auto found = std::ranges::find(protocol::kMessageTypeNames, name);
    if (found == protocol::kMessageTypeNames.end()) {
        return std::nullopt;
    }
    return static_cast<MessageType>(found - protocol::kMessageTypeNames.begin());
}

std::optional<double> number(const nlohmann::json& message, const char* field)
{
    const auto found = message.find(field);
    if (found == message.end() || !found->is_number() || !std::isfinite(found->get<double>())) {
        return std::nullopt;
    }
    return found->get<double>();
}

// A number from the page, as it would read it: whole numbers without a fraction, any other as
// JSON writes it ("3.5", "5e+89"). A value too large for an integer is never cast to one.
std::string numberText(double value)
{
    constexpr double kExactIntegers = 9007199254740992.0; // 2^53.
    if (std::trunc(value) == value && std::abs(value) < kExactIntegers) {
        return std::to_string(static_cast<long long>(value));
    }
    return nlohmann::json(value).dump();
}

std::optional<protocol::SettingId> settingId(const nlohmann::json& name)
{
    if (!name.is_string()) {
        return std::nullopt;
    }
    for (const auto& setting : protocol::kSettings) {
        if (setting.name == name.get_ref<const std::string&>()) {
            return setting.id;
        }
    }
    return std::nullopt;
}

} // namespace

Session::Session(engine::Engine& engine, engine::Analyzer& analyzer, Transport& transport)
    : engine(engine), analyzer(analyzer), transport(transport)
{}

void Session::receive(const nlohmann::json& message)
{
    const auto type = messageType(message);
    if (!type) {
        return;
    }
    if (*type == MessageType::kHello) {
        hello(message);
        return;
    }
    if (!isStarted) {
        return;
    }
    switch (*type) {
    case MessageType::kSetSetting:
        setSetting(message);
        break;
    case MessageType::kViewport:
        setViewport(message);
        break;
    case MessageType::kFreeze:
        if (message.contains("frozen") && message["frozen"].is_boolean()) {
            analyzer.setFrozen(message["frozen"].get<bool>());
        }
        break;
    case MessageType::kPan:
        if (const auto seconds = number(message, "seconds")) {
            analyzer.setPan(*seconds);
        }
        break;
    case MessageType::kArm:
        analyzer.arm();
        break;
    case MessageType::kView:
        if (const auto start = number(message, "start")) {
            if (const auto end = number(message, "end")) {
                analyzer.setView(*start, *end);
            }
        }
        break;
    case MessageType::kCompare:
        if (message.contains("ids") && message["ids"].is_array()) {
            std::vector<std::uint64_t> ids;
            for (const nlohmann::json& id : message["ids"]) {
                if (id.is_number_unsigned()) {
                    ids.push_back(id.get<std::uint64_t>());
                }
            }
            analyzer.setCompared(ids);
        }
        break;
    case MessageType::kPreviewFix:
        if (message.contains("on") && message["on"].is_boolean()) {
            analyzer.setPreviewFix(message["on"].get<bool>());
        }
        break;
    default:
        break; // Engine-to-UI types.
    }
}

void Session::hello(const nlohmann::json& message)
{
    const auto version = number(message, "protocolVersion");
    if (!version || *version != protocol::kProtocolVersion) {
        isStarted = false;
        transport.sendMessage(
            {{"type", "error"},
             {"code", "protocolMismatch"},
             {"message", "The UI speaks protocol version " +
                             (version ? numberText(*version) : std::string("unknown")) +
                             "; the engine speaks version " +
                             std::to_string(protocol::kProtocolVersion) + "."}});
        return;
    }
    setViewport(message);
    analyzer.redraw(); // The page has no frame yet, whether or not anything changed.
    sendSnapshot();
    lastLaneSet = 0;
    isStarted = true;
    syncLanes();
}

// Everything the page needs to start from: the settings, and the audio format, which the UI
// converts samples to time and lays out a mono track's views with.
void Session::sendSnapshot()
{
    const engine::Settings settings = engine.settings().get();
    nlohmann::json values = nlohmann::json::object();
    for (const auto& setting : protocol::kSettings) {
        values[std::string(setting.name)] =
            engine::settingToJson(setting.id, settings.value(setting.id));
    }
    sentSampleRate = engine.sampleRate();
    sentInputChannels = engine.inputChannels();
    transport.sendMessage({{"type", "snapshot"},
                           {"protocolVersion", protocol::kProtocolVersion},
                           {"settings", values},
                           {"sampleRate", sentSampleRate},
                           {"inputChannels", sentInputChannels}});
    lastSent = settings;
}

void Session::setSetting(const nlohmann::json& message)
{
    const auto id = settingId(message.value("name", nlohmann::json()));
    if (!id) {
        return;
    }
    if (message.contains("value")) {
        if (const auto value = engine::settingFromJson(*id, message["value"])) {
            engine.settings().set(*id, *value);
        }
    }
    // Always answer, so that a rejected value snaps the UI control back.
    sendState(*id, engine.settings().get().value(*id));
}

void Session::setViewport(const nlohmann::json& message)
{
    const auto width = number(message, "viewWidth");
    const auto ratio = number(message, "pixelRatio");
    if (!width || !ratio || *width <= 0.0 || *ratio <= 0.0) {
        return;
    }
    analyzer.setColumns(
        static_cast<std::size_t>(std::clamp(std::round(*width * *ratio), 1.0, kMaxColumns)));
}

void Session::sync()
{
    if (!isStarted) {
        return;
    }
    if (engine.sampleRate() != sentSampleRate || engine.inputChannels() != sentInputChannels) {
        sendSnapshot(); // The host changed the rate or the track's channels with the editor open.
    } else {
        const engine::Settings settings = engine.settings().get();
        for (const auto& setting : protocol::kSettings) {
            if (!lastSent || lastSent->value(setting.id) != settings.value(setting.id)) {
                sendState(setting.id, settings.value(setting.id));
            }
        }
    }
    syncLanes();
}

void Session::syncLanes()
{
    const engine::LaneSet lanes = analyzer.laneSet();
    if (lanes.id == 0 || lanes.id == lastLaneSet) {
        return;
    }
    nlohmann::json labels = nlohmann::json::array();
    for (const engine::LaneLabel& lane : lanes.lanes) {
        labels.push_back({{"id", lane.id},
                          {"name", lane.name},
                          {"colour", lane.colour ? nlohmann::json(*lane.colour) : nlohmann::json()},
                          {"own", lane.own},
                          {"compared", lane.compared}});
    }
    transport.sendMessage(
        {{"type", "lanes"}, {"laneSet", lanes.id}, {"lanes", labels}, {"hidden", lanes.hidden}});
    lastLaneSet = lanes.id;
}

void Session::sendState(protocol::SettingId id, double value)
{
    transport.sendMessage(
        {{"type", "stateChanged"},
         {"name", std::string(protocol::kSettings.at(static_cast<std::size_t>(id)).name)},
         {"value", engine::settingToJson(id, value)}});
    if (!lastSent) {
        lastSent = engine::Settings();
    }
    lastSent->set(id, value);
}

} // namespace orizon::editor
