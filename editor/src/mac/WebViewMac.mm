#include "EmbeddedAssets.h"
#include "editor/WebView.h"
#include "engine/Version.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string>
#include <utility>

// Objective-C classes are created at run time under names that include the product, the build
// version and an id unique to this loaded image, so that several copies or versions of the
// plugin in one host process never share a class (DESIGN AD-4).

namespace orizon::editor {
namespace {

NSString* const kSchemeName = @"orizon";
NSString* const kStartUrl = @"orizon://ui/index.html";
NSString* const kMessageHandlerName = @"orizon";
NSString* const kFocusHandlerName = @"orizonFocus";

// Reports to native code whether an editable element has focus, so that keystrokes go to the
// host unless the user is typing into the page.
constexpr const char* kFocusTrackingScript = R"JS(
(() => {
  const nonText = new Set(["button", "checkbox", "color", "file", "hidden", "image", "radio",
                           "range", "reset", "submit"]);
  const isEditable = (el) => !!el && (el.isContentEditable || el.tagName === "TEXTAREA" ||
      el.tagName === "SELECT" || (el.tagName === "INPUT" && !nonText.has(el.type)));
  const report = () => window.webkit.messageHandlers.orizonFocus.postMessage(
      isEditable(document.activeElement) ? "1" : "0");
  document.addEventListener("focusin", report, true);
  document.addEventListener("focusout", () => setTimeout(report, 0), true);
})();
)JS";

const char kOwnerKeyTag = 0;
const void* const kOwnerKey = static_cast<const void*>(&kOwnerKeyTag);

class WebViewMac;

WebViewMac* ownerOf(id object)
{
    NSValue* value = objc_getAssociatedObject(object, kOwnerKey);
    return static_cast<WebViewMac*>(value.pointerValue);
}

void setOwner(id object, WebViewMac* owner)
{
    objc_setAssociatedObject(object, kOwnerKey,
                             owner != nullptr ? [NSValue valueWithPointer:owner] : nil,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

std::string uniqueClassName(const char* baseName)
{
    static const int kImageMarker = 0;
    std::string version = engine::kVersionString;
    std::ranges::replace(version, '.', '_');
    std::array<char, 32> imageId{};
    std::snprintf(imageId.data(), imageId.size(), "%llx",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&kImageMarker)));
    return std::string("OrizonOscilloscope_") + version + "_" + baseName + "_" + imageId.data();
}

const char* protocolMethodTypes(Protocol* protocol, SEL selector)
{
    return protocol_getMethodDescription(protocol, selector, YES, YES).types;
}

const char* superclassMethodTypes(Class superclass, SEL selector)
{
    return method_getTypeEncoding(class_getInstanceMethod(superclass, selector));
}

class WebViewMac final : public WebView
{
public:
    WebViewMac(NSView* parent, int width, int height, MessageHandler onMessage);
    ~WebViewMac() override;
    WebViewMac(const WebViewMac&) = delete;
    WebViewMac& operator=(const WebViewMac&) = delete;

    void setSize(int width, int height) override;
    void postMessage(std::string_view message) override;
    void postBinary(std::span<const std::byte> payload) override;
    [[nodiscard]] void* nativeView() const override { return (__bridge void*)webView; }

    void receive(WKScriptMessage* message);
    [[nodiscard]] bool editableFocused() const { return isEditableFocused; }

private:
    void evaluate(NSString* script);

    MessageHandler onMessage;
    WKWebView* webView = nil;
    id messageHandler = nil;
    bool isEditableFocused = false;
};

// ----- Script message handler (page to native) -----

void didReceiveScriptMessage(id self, SEL /*cmd*/, WKUserContentController* /*controller*/,
                             WKScriptMessage* message)
{
    if (WebViewMac* owner = ownerOf(self)) {
        owner->receive(message);
    }
}

Class messageHandlerClass()
{
    static Class cls = [] {
        Class created =
            objc_allocateClassPair([NSObject class], uniqueClassName("MessageHandler").c_str(), 0);
        Protocol* protocol = @protocol(WKScriptMessageHandler);
        SEL selector = @selector(userContentController:didReceiveScriptMessage:);
        class_addMethod(created, selector, reinterpret_cast<IMP>(&didReceiveScriptMessage),
                        protocolMethodTypes(protocol, selector));
        class_addProtocol(created, protocol);
        objc_registerClassPair(created);
        return created;
    }();
    return cls;
}

// ----- URL scheme handler serving the embedded UI (AD-11) -----

void startUrlSchemeTask(id /*self*/, SEL /*cmd*/, WKWebView* /*webView*/, id<WKURLSchemeTask> task)
{
    NSURL* url = task.request.URL;
    std::string path = url.path.length > 1 ? std::string(url.path.UTF8String).substr(1)
                                           : std::string("index.html");
    const EmbeddedAsset* asset = findEmbeddedAsset(path);
    if (asset == nullptr) {
        [task didReceiveResponse:[[NSHTTPURLResponse alloc] initWithURL:url
                                                             statusCode:404
                                                            HTTPVersion:@"HTTP/1.1"
                                                           headerFields:@{}]];
        [task didFinish];
        return;
    }
    NSString* mimeType = [[NSString alloc] initWithBytes:asset->mimeType.data()
                                                  length:asset->mimeType.size()
                                                encoding:NSUTF8StringEncoding];
    NSDictionary* headers = @{
        @"Content-Type" : mimeType,
        @"Content-Length" : [NSString stringWithFormat:@"%zu", asset->size],
        @"Cache-Control" : @"no-store",
    };
    [task didReceiveResponse:[[NSHTTPURLResponse alloc] initWithURL:url
                                                         statusCode:200
                                                        HTTPVersion:@"HTTP/1.1"
                                                       headerFields:headers]];
    [task didReceiveData:[NSData dataWithBytesNoCopy:const_cast<unsigned char*>(asset->data)
                                              length:asset->size
                                        freeWhenDone:NO]];
    [task didFinish];
}

void stopUrlSchemeTask(id /*self*/, SEL /*cmd*/, WKWebView* /*webView*/,
                       id<WKURLSchemeTask> /*task*/)
{
    // Tasks complete synchronously in startUrlSchemeTask, so there is nothing to cancel.
}

Class schemeHandlerClass()
{
    static Class cls = [] {
        Class created =
            objc_allocateClassPair([NSObject class], uniqueClassName("SchemeHandler").c_str(), 0);
        Protocol* protocol = @protocol(WKURLSchemeHandler);
        SEL start = @selector(webView:startURLSchemeTask:);
        SEL stop = @selector(webView:stopURLSchemeTask:);
        class_addMethod(created, start, reinterpret_cast<IMP>(&startUrlSchemeTask),
                        protocolMethodTypes(protocol, start));
        class_addMethod(created, stop, reinterpret_cast<IMP>(&stopUrlSchemeTask),
                        protocolMethodTypes(protocol, stop));
        class_addProtocol(created, protocol);
        objc_registerClassPair(created);
        return created;
    }();
    return cls;
}

// ----- WKWebView subclass: keystrokes go to the host unless an editable element has focus -----

void forwardKeyEvent(id self, SEL cmd, NSEvent* event)
{
    WebViewMac* owner = ownerOf(self);
    if (owner != nullptr && owner->editableFocused()) {
        objc_super superInfo{.receiver = self, .super_class = [WKWebView class]};
        reinterpret_cast<void (*)(objc_super*, SEL, NSEvent*)>(objc_msgSendSuper)(&superInfo, cmd,
                                                                                  event);
        return;
    }
    NSResponder* next = [self nextResponder];
    if ([next respondsToSelector:cmd]) {
        reinterpret_cast<void (*)(id, SEL, NSEvent*)>(objc_msgSend)(next, cmd, event);
    }
}

BOOL performKeyEquivalent(id self, SEL cmd, NSEvent* event)
{
    WebViewMac* owner = ownerOf(self);
    if (owner != nullptr && owner->editableFocused()) {
        objc_super superInfo{.receiver = self, .super_class = [WKWebView class]};
        return reinterpret_cast<BOOL (*)(objc_super*, SEL, NSEvent*)>(objc_msgSendSuper)(
            &superInfo, cmd, event);
    }
    return NO; // Let the host handle shortcuts.
}

BOOL acceptsFirstMouse(id /*self*/, SEL /*cmd*/, NSEvent* /*event*/)
{
    return YES; // A click in an inactive plugin window acts immediately.
}

Class webViewClass()
{
    static Class cls = [] {
        Class superclass = [WKWebView class];
        Class created = objc_allocateClassPair(superclass, uniqueClassName("WebView").c_str(), 0);
        for (SEL selector : { @selector(keyDown:), @selector(keyUp:), @selector(flagsChanged:) }) {
            class_addMethod(created, selector, reinterpret_cast<IMP>(&forwardKeyEvent),
                            superclassMethodTypes(superclass, selector));
        }
        class_addMethod(created, @selector(performKeyEquivalent:),
                        reinterpret_cast<IMP>(&performKeyEquivalent),
                        superclassMethodTypes(superclass, @selector(performKeyEquivalent:)));
        class_addMethod(created, @selector(acceptsFirstMouse:),
                        reinterpret_cast<IMP>(&acceptsFirstMouse),
                        superclassMethodTypes(superclass, @selector(acceptsFirstMouse:)));
        objc_registerClassPair(created);
        return created;
    }();
    return cls;
}

// ----- WebViewMac -----

WebViewMac::WebViewMac(NSView* parent, int width, int height, MessageHandler onMessage)
    : onMessage(std::move(onMessage))
{
    WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
    [configuration setURLSchemeHandler:[[schemeHandlerClass() alloc] init]
                          forURLScheme:kSchemeName];

    messageHandler = [[messageHandlerClass() alloc] init];
    setOwner(messageHandler, this);
    WKUserContentController* contentController = configuration.userContentController;
    [contentController addScriptMessageHandler:messageHandler name:kMessageHandlerName];
    [contentController addScriptMessageHandler:messageHandler name:kFocusHandlerName];
    [contentController
        addUserScript:[[WKUserScript alloc]
                            initWithSource:[NSString stringWithUTF8String:kFocusTrackingScript]
                             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                          forMainFrameOnly:YES]];

    webView = [[webViewClass() alloc] initWithFrame:NSMakeRect(0, 0, width, height)
                                      configuration:configuration];
    setOwner(webView, this);
    webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
#ifndef NDEBUG
    if (@available(macOS 13.3, *)) {
        webView.inspectable = YES;
    }
#endif
    [parent addSubview:webView];
    [webView loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:kStartUrl]]];
}

WebViewMac::~WebViewMac()
{
    // Detach every path back into this object before the webview can outlive it.
    setOwner(messageHandler, nullptr);
    setOwner(webView, nullptr);
    [webView.configuration.userContentController removeAllScriptMessageHandlers];
    [webView stopLoading];
    [webView removeFromSuperview];
    webView = nil;
    messageHandler = nil;
}

void WebViewMac::setSize(int width, int height)
{
    [webView setFrameSize:NSMakeSize(width, height)];
}

void WebViewMac::postMessage(std::string_view message)
{
    NSString* text = [[NSString alloc] initWithBytes:message.data()
                                              length:message.size()
                                            encoding:NSUTF8StringEncoding];
    NSData* json = [NSJSONSerialization dataWithJSONObject:text
                                                   options:NSJSONWritingFragmentsAllowed
                                                     error:nil];
    NSString* literal = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
    evaluate([NSString stringWithFormat:@"window.orizonBridge?.receive(%@)", literal]);
}

void WebViewMac::postBinary(std::span<const std::byte> payload)
{
    NSData* data = [NSData dataWithBytes:payload.data() length:payload.size()];
    NSString* base64 = [data base64EncodedStringWithOptions:0];
    evaluate([NSString stringWithFormat:@"window.orizonBridge?.receiveBinary(\"%@\")", base64]);
}

void WebViewMac::evaluate(NSString* script)
{
    [webView evaluateJavaScript:script completionHandler:nil];
}

void WebViewMac::receive(WKScriptMessage* message)
{
    if (![message.body isKindOfClass:[NSString class]]) {
        return;
    }
    NSString* body = message.body;
    if ([message.name isEqualToString:kFocusHandlerName]) {
        isEditableFocused = [body isEqualToString:@"1"];
        return;
    }
    if (onMessage) {
        onMessage(*this, std::string_view(body.UTF8String));
    }
}

} // namespace

std::unique_ptr<WebView> WebView::create(void* parent, int width, int height,
                                         MessageHandler onMessage)
{
    return std::make_unique<WebViewMac>((__bridge NSView*)parent, width, height,
                                        std::move(onMessage));
}

} // namespace orizon::editor
