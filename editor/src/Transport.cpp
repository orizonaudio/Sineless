#include "editor/Transport.h"

namespace orizon::editor {

void WebViewTransport::sendMessage(const nlohmann::json& message)
{
    view.postMessage(message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

void WebViewTransport::sendFrame(std::span<const std::byte> frame)
{
    view.sendFrame(frame);
}

std::optional<nlohmann::json> WebViewTransport::parse(std::string_view text)
{
    nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
    if (!message.is_object() || !message.contains("type") || !message["type"].is_string()) {
        return std::nullopt;
    }
    return message;
}

} // namespace orizon::editor
