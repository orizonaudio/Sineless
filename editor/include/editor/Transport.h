#pragma once

#include "editor/WebView.h"
#include "protocol/Protocol.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string_view>

namespace orizon::editor {

// A message type's name on the wire, from the generated protocol table, so a renamed
// type cannot leave a hand-written copy behind.
[[nodiscard]] constexpr std::string_view messageName(protocol::MessageType type)
{
    return protocol::kMessageTypeNames[static_cast<std::size_t>(type)];
}

// Engine-to-UI delivery: JSON control messages and binary display frames. Every message
// is a JSON object with a string `type` (protocol MessageType). Called on the UI thread.
class Transport
{
public:
    Transport() = default;
    virtual ~Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    virtual void sendMessage(const nlohmann::json& message) = 0;
    virtual void sendFrame(std::span<const std::byte> frame) = 0;
};

// Transport over the webview's native message channel.
class WebViewTransport final : public Transport
{
public:
    explicit WebViewTransport(WebView& view) : view(view) {}

    void sendMessage(const nlohmann::json& message) override;
    void sendFrame(std::span<const std::byte> frame) override;

    // Parses a message from the page; anything but an object with a string `type` is dropped.
    [[nodiscard]] static std::optional<nlohmann::json> parse(std::string_view text);

private:
    WebView& view;
};

} // namespace orizon::editor
