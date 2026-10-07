#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace orizon::editor {

// Native webview that hosts the UI inside the host's editor window.
class WebView
{
public:
    // Called on the UI thread for every message the page sends over the native channel.
    using MessageHandler = std::function<void(WebView& view, std::string_view message)>;

    // `parent` is the host's NSView. The page is loaded from the embedded UI.
    static std::unique_ptr<WebView> create(void* parent, int width, int height,
                                           MessageHandler onMessage);

    WebView() = default;
    virtual ~WebView() = default;
    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;

    virtual void setSize(int width, int height) = 0;

    // Native-to-page delivery over the native channel.
    virtual void postMessage(std::string_view message) = 0;
    virtual void postBinary(std::span<const std::byte> payload) = 0;

    // The platform view (NSView*), for tests.
    [[nodiscard]] virtual void* nativeView() const = 0;
};

} // namespace orizon::editor
