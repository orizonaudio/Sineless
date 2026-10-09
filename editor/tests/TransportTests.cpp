#include "editor/Transport.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace orizon::editor {
namespace {

class RecordingWebView final : public WebView
{
public:
    void setSize(int /*width*/, int /*height*/) override {}
    void setScale(double /*scale*/) override {}
    void postMessage(std::string_view message) override { messages.emplace_back(message); }
    void sendFrame(std::span<const std::byte> frame) override
    {
        frames.emplace_back(frame.begin(), frame.end());
    }
    [[nodiscard]] void* nativeView() const override { return nullptr; }
    void evaluate(std::string_view /*script*/, std::function<void(std::string)> done) override
    {
        done("");
    }

    std::vector<std::string> messages;
    std::vector<std::vector<std::byte>> frames;
};

TEST(WebViewTransport, SendsMessagesAsJsonText)
{
    RecordingWebView view;
    WebViewTransport transport(view);
    transport.sendMessage({{"type", "stateChanged"}, {"name", "triggerMode"}, {"value", "auto"}});
    ASSERT_EQ(view.messages.size(), 1u);
    EXPECT_EQ(nlohmann::json::parse(view.messages[0])["value"], "auto");
}

TEST(WebViewTransport, SendsFramesAsBinary)
{
    RecordingWebView view;
    WebViewTransport transport(view);
    const std::vector<std::byte> frame{std::byte{1}, std::byte{2}, std::byte{255}};
    transport.sendFrame(frame);
    ASSERT_EQ(view.frames.size(), 1u);
    EXPECT_EQ(view.frames[0], frame);
}

TEST(WebViewTransport, ParsesObjectsWithAType)
{
    const auto message = WebViewTransport::parse(R"({"type": "hello", "viewWidth": 800})");
    ASSERT_TRUE(message);
    EXPECT_EQ((*message)["viewWidth"], 800);
}

TEST(WebViewTransport, DropsMalformedMessages)
{
    for (const char* text :
         {"", "ping", "[1]", R"({"viewWidth": 800})", R"({"type": 3})", R"({"type": "hello")"}) {
        EXPECT_FALSE(WebViewTransport::parse(text)) << text;
    }
}

} // namespace
} // namespace orizon::editor
