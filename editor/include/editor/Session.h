#pragma once

#include "editor/Transport.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"
#include "engine/Settings.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

namespace orizon::editor {

// The engine side of a UI session. C++ is the store of record: the UI changes a
// setting only by asking, and applies only the values this session reports back.
// Used on the UI thread.
class Session
{
public:
    // Display frames do not pass through the session: the page fetches them (see WebView).
    Session(engine::Engine& engine, engine::Analyzer& analyzer, Transport& transport);

    // Handles one message from the page. Before a successful hello only hello is accepted.
    void receive(const nlohmann::json& message);

    // Reports what changed outside the session: settings (the host's setState) as stateChanged,
    // and a new multi-track lane set as lanes.
    void sync();

    [[nodiscard]] bool started() const { return isStarted; }

private:
    void hello(const nlohmann::json& message);
    void setSetting(const nlohmann::json& message);
    void setViewport(const nlohmann::json& message);
    void sendState(protocol::SettingId id, double value);
    void syncLanes();

    engine::Engine& engine;
    engine::Analyzer& analyzer;
    Transport& transport;
    bool isStarted = false;
    std::optional<engine::Settings> lastSent;
    std::uint32_t lastLaneSet = 0;
};

} // namespace orizon::editor
