#pragma once

// For tests that load the editor's page in WKWebView (the VST3 editor's and the Audio Unit view's):
// run the main run loop until a condition holds, and ask the page for its state.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orizon::testing {

// Long enough for the page to load on a busy machine.
inline constexpr std::chrono::seconds kPageTimeout{15};

// Runs the main run loop until `condition` holds or `timeout` passes; whether it holds.
inline bool pumpUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    return condition();
}

// Evaluates `script` in the page and returns its result as a string ("" if none yet).
inline std::string evaluate(WKWebView* webView, NSString* script)
{
    struct Result
    {
        bool done = false;
        std::string value;
    };
    auto result = std::make_shared<Result>();
    [webView evaluateJavaScript:script
              completionHandler:^(id value, NSError* /*error*/) {
                if ([value isKindOfClass:[NSString class]]) {
                    result->value = [value UTF8String];
                }
                result->done = true;
              }];
    pumpUntil([&] { return result->done; }, std::chrono::seconds(5));
    return result->value;
}

// Whether the page has drawn a frame from the engine.
inline bool hasSnapshot(WKWebView* webView)
{
    return evaluate(webView, @"String(window.orizonDebug?.snapshot?.type ?? '')") == "snapshot";
}

// Whether any raw page message has the given type.
inline bool saw(const std::vector<std::string>& messages, const std::string& type)
{
    return std::ranges::any_of(messages, [&](const std::string& message) {
        return message.find(R"("type":")" + type + "\"") != std::string::npos;
    });
}

} // namespace orizon::testing
