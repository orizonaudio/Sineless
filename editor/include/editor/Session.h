#pragma once

#include "editor/Transport.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"
#include "engine/Settings.h"

#include <nlohmann/json.hpp>
#include <optional>

namespace orizon::editor {

// The engine side of a UI session (DESIGN 6.1). C++ is the store of record: the UI changes a
// setting only by asking, and applies only the values this session reports back.
// Used on the UI thread.
class Session
{
public:
    Session(engine::Engine& engine, engine::Analyzer& analyzer, Transport& transport);

    // Handles one message from the page. Before a successful hello only hello is accepted.
    void receive(const nlohmann::json& message);

    // Reports settings changed outside the session (the host's setState) as stateChanged.
    void syncSettings();

    [[nodiscard]] bool started() const { return isStarted; }

private:
    void hello(const nlohmann::json& message);
    void setSetting(const nlohmann::json& message);
    void setViewport(const nlohmann::json& message);
    void sendState(protocol::SettingId id, double value);

    engine::Engine& engine;
    engine::Analyzer& analyzer;
    Transport& transport;
    bool isStarted = false;
    std::optional<engine::Settings> lastSent;
};

} // namespace orizon::editor
