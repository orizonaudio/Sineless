#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace orizon::editor {

// Native webview that hosts the UI inside the host's editor window.
class WebView
{
public:
    // Called on the UI thread for every message the page sends over the native channel.
    using MessageHandler = std::function<void(WebView& view, std::string_view message)>;
    // Called on the UI thread when the page asks for the next display frame.
    using FrameRequestHandler = std::function<void(WebView& view)>;

    // `parent` is the host's NSView or HWND. The page is loaded from the embedded UI.
    static std::unique_ptr<WebView> create(void* parent, int width, int height,
                                           MessageHandler onMessage,
                                           FrameRequestHandler onFrameRequest = {});

    WebView() = default;
    virtual ~WebView() = default;
    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;

    // In the host's window units, like the editor's rect.
    virtual void setSize(int width, int height) = 0;
    // Host window units per CSS pixel (see displayScale); the page lays out in CSS pixels.
    virtual void setScale(double scale) = 0;

    // Native-to-page delivery: messages over the native channel, and display frames as the
    // binary answer to the page's frame request (kept for the next request if none is open).
    virtual void postMessage(std::string_view message) = 0;
    virtual void sendFrame(std::span<const std::byte> frame) = 0;

    // The platform view (NSView*, or the host's HWND on Windows), for tests.
    [[nodiscard]] virtual void* nativeView() const = 0;

    // Runs `script` in the page and calls `done` on the UI thread with its result if that is a
    // string, otherwise with "". For tests.
    virtual void evaluate(std::string_view script, std::function<void(std::string)> done) = 0;
};

} // namespace orizon::editor
