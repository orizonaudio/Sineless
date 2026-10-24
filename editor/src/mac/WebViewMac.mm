#include "EmbeddedAssets.h"
#include "editor/WebView.h"
#include "engine/Version.h"
#include "mac/WebViewMacTesting.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string>
#include <utility>

// Objective-C classes are created at run time under names that include the product, the build
// version and an id unique to this loaded image, so that several copies or versions of the
// plugin in one host process never share a class.

namespace orizon::editor {
namespace {

NSString* const kSchemeName = @"orizon";
NSString* const kStartUrl = @"orizon://ui/index.html";
NSString* const kMessageHandlerName = @"orizon";
NSString* const kFocusHandlerName = @"orizonFocus";
NSString* const kCursorHandlerName = @"orizonCursor";
// The page fetches each display frame from here; the request is held until a frame is ready.
NSString* const kFramePath = @"/frame";
// The page's own origin: the only one it may navigate to or send messages from.
NSString* const kUiHost = @"ui";
// A page process that keeps dying is not reloaded again within the minute.
constexpr std::size_t kMaxReloadsPerMinute = 3;

bool isUiUrl(NSURL* url)
{
    return [url.scheme isEqualToString:kSchemeName] && [url.host isEqualToString:kUiHost];
}

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

// Reports the page's mouse cursor (its CSS cursor under the pointer, or of the element a drag
// holds) to native code whenever it changes. An Audio Unit's view hosted out of process (in Logic,
// AUHostingService mirrors it into Logic's window) shows no cursor WebKit sets, nor one set on a
// mouse move: only the cursor the view gives AppKit through its cursor rectangle and
// cursorUpdate:, which the view takes from the page.
constexpr const char* kCursorTrackingScript = R"JS(
(() => {
  let reported = "";
  const report = (event) => {
    const target = event.target instanceof Element ? event.target : document.documentElement;
    const cursor = getComputedStyle(target).cursor;
    if (cursor !== reported) {
      reported = cursor;
      window.webkit.messageHandlers.orizonCursor.postMessage(cursor);
    }
  };
  for (const type of ["pointerover", "pointermove", "pointerdown", "pointerup"]) {
    document.addEventListener(type, report, true);
  }
})();
)JS";

// A diagonal resize cursor drawn from the matching system symbol, for macOS before 15, which has
// no public one: black on a white outline, as the system's cursors are, so that it shows on dark
// and light alike.
NSCursor* drawnDiagonalResizeCursor()
{
    static NSCursor* drawn = [] {
        NSImage* symbol = [NSImage imageWithSystemSymbolName:@"arrow.up.left.and.arrow.down.right"
                                    accessibilityDescription:nil];
        const auto tinted = [&](NSColor* colour) {
            NSImageSymbolConfiguration* configuration =
                [[NSImageSymbolConfiguration configurationWithPointSize:14
                                                                 weight:NSFontWeightSemibold]
                    configurationByApplyingConfiguration:[NSImageSymbolConfiguration
                                                             configurationWithPaletteColors:@[
                                                                 colour
                                                             ]]];
            return [symbol imageWithSymbolConfiguration:configuration];
        };
        NSImage* black = tinted([NSColor blackColor]);
        NSImage* white = tinted([NSColor whiteColor]);
        constexpr CGFloat kOutline = 1.5;
        const NSSize glyph = black.size;
        const NSSize size = NSMakeSize(glyph.width + 2 * kOutline, glyph.height + 2 * kOutline);
        NSImage* image =
            [NSImage imageWithSize:size
                           flipped:NO
                    drawingHandler:^BOOL(NSRect /*rect*/) {
                      for (const CGFloat dx : {-kOutline, 0.0, kOutline}) {
                          for (const CGFloat dy : {-kOutline, 0.0, kOutline}) {
                              [white drawAtPoint:NSMakePoint(kOutline + dx, kOutline + dy)
                                        fromRect:NSZeroRect
                                       operation:NSCompositingOperationSourceOver
                                        fraction:1.0];
                          }
                      }
                      [black drawAtPoint:NSMakePoint(kOutline, kOutline)
                                fromRect:NSZeroRect
                               operation:NSCompositingOperationSourceOver
                                fraction:1.0];
                      return YES;
                    }];
        return [[NSCursor alloc] initWithImage:image
                                       hotSpot:NSMakePoint(size.width / 2, size.height / 2)];
    }();
    return drawn;
}

// The diagonal resize cursor of the page's resize grip: macOS's own from 15 on, drawn before.
NSCursor* diagonalResizeCursor()
{
    if (@available(macOS 15.0, *)) {
        return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionBottomRight
                                          inDirections:NSCursorFrameResizeDirectionsAll];
    }
    return drawnDiagonalResizeCursor();
}

// The macOS cursor for a CSS cursor the page uses, or nil for one without a match, which is left to
// WebKit.
NSCursor* cursorFor(NSString* css)
{
    if ([css isEqualToString:@"auto"] || [css isEqualToString:@"default"]) {
        return [NSCursor arrowCursor];
    }
    if ([css isEqualToString:@"pointer"]) {
        return [NSCursor pointingHandCursor];
    }
    if ([css isEqualToString:@"text"]) {
        return [NSCursor IBeamCursor];
    }
    if ([css isEqualToString:@"crosshair"]) {
        return [NSCursor crosshairCursor];
    }
    if ([css isEqualToString:@"ns-resize"] || [css isEqualToString:@"row-resize"]) {
        return [NSCursor resizeUpDownCursor];
    }
    if ([css isEqualToString:@"ew-resize"] || [css isEqualToString:@"col-resize"]) {
        return [NSCursor resizeLeftRightCursor];
    }
    if ([css isEqualToString:@"nwse-resize"]) {
        return diagonalResizeCursor();
    }
    return nil;
}

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
    WebViewMac(NSView* parent, int width, int height, MessageHandler onMessage,
               FrameRequestHandler onFrameRequest);
    ~WebViewMac() override;
    WebViewMac(const WebViewMac&) = delete;
    WebViewMac& operator=(const WebViewMac&) = delete;

    void setSize(int width, int height) override;
    void postMessage(std::string_view message) override;
    void sendFrame(std::span<const std::byte> frame) override;
    [[nodiscard]] void* nativeView() const override { return (__bridge void*)webView; }
    void evaluate(std::string_view script, std::function<void(std::string)> done) override;
    void setScale(double /*scale*/) override {} // macOS sizes in points: always 1.

    void receive(WKScriptMessage* message);
    void requestFrame(id<WKURLSchemeTask> task);
    void cancelFrameRequest(id<WKURLSchemeTask> task);
    [[nodiscard]] bool editableFocused() const { return isEditableFocused; }
    [[nodiscard]] NSCursor* cursor() const { return pageCursor; }

    void pageLoaded();
    void pageFailed(NSError* error);
    void processTerminated();
    bool terminateForTests();

private:
    void showMessage(NSString* text);

    void evaluate(NSString* script);
    static void answer(id<WKURLSchemeTask> task, NSData* frame);

    MessageHandler onMessage;
    FrameRequestHandler onFrameRequest;
    NSView* container = nil; // The page's colour, behind it until it has loaded.
    NSTextField* notice = nil;
    WKWebView* webView = nil;
    id messageHandler = nil;
    id schemeHandler = nil;
    id navigationDelegate = nil;
    bool loaded = false;
    std::deque<std::chrono::steady_clock::time_point> reloads; // In the last minute.
    bool isEditableFocused = false;
    // As the page last reported it; nil when it has no macOS match, left to WebKit.
    NSCursor* pageCursor = [NSCursor arrowCursor];
    // The page's open frame request, and a frame produced while none was open.
    id<WKURLSchemeTask> frameTask = nil;
    NSData* heldFrame = nil;
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

// ----- URL scheme handler serving the embedded UI -----

void startUrlSchemeTask(id self, SEL /*cmd*/, WKWebView* /*webView*/, id<WKURLSchemeTask> task)
{
    NSURL* url = task.request.URL;
    if ([url.path isEqualToString:kFramePath]) {
        if (WebViewMac* owner = ownerOf(self)) {
            owner->requestFrame(task);
        }
        return;
    }
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
        @"Content-Security-Policy" : @(std::string(kContentSecurityPolicy).c_str()),
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

void stopUrlSchemeTask(id self, SEL /*cmd*/, WKWebView* /*webView*/, id<WKURLSchemeTask> task)
{
    // Asset tasks complete synchronously in startUrlSchemeTask; only a frame request stays open.
    if (WebViewMac* owner = ownerOf(self)) {
        owner->cancelFrameRequest(task);
    }
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

// ----- Navigation delegate: the page stays on its own origin, and comes back after a crash -----

void decidePolicyForNavigationAction(id /*self*/, SEL /*cmd*/, WKWebView* /*webView*/,
                                     WKNavigationAction* action,
                                     void (^decisionHandler)(WKNavigationActionPolicy))
{
    // A link or a file dropped on the editor would otherwise replace the UI.
    decisionHandler(isUiUrl(action.request.URL) ? WKNavigationActionPolicyAllow
                                                : WKNavigationActionPolicyCancel);
}

void didFinishNavigation(id self, SEL /*cmd*/, WKWebView* /*webView*/, WKNavigation* /*navigation*/)
{
    if (WebViewMac* owner = ownerOf(self)) {
        owner->pageLoaded();
    }
}

void didFailNavigation(id self, SEL /*cmd*/, WKWebView* /*webView*/, WKNavigation* /*navigation*/,
                       NSError* error)
{
    if (WebViewMac* owner = ownerOf(self)) {
        owner->pageFailed(error);
    }
}

void webContentProcessDidTerminate(id self, SEL /*cmd*/, WKWebView* /*webView*/)
{
    if (WebViewMac* owner = ownerOf(self)) {
        owner->processTerminated();
    }
}

Class navigationDelegateClass()
{
    static Class cls = [] {
        Class created = objc_allocateClassPair([NSObject class],
                                               uniqueClassName("NavigationDelegate").c_str(), 0);
        Protocol* protocol = @protocol(WKNavigationDelegate);
        const auto add = [&](SEL selector, IMP implementation) {
            class_addMethod(created, selector, implementation,
                            protocol_getMethodDescription(protocol, selector, NO, YES).types);
        };
        add(@selector(webView:decidePolicyForNavigationAction:decisionHandler:),
            reinterpret_cast<IMP>(&decidePolicyForNavigationAction));
        add(@selector(webView:didFinishNavigation:), reinterpret_cast<IMP>(&didFinishNavigation));
        add(@selector(webView:didFailNavigation:withError:),
            reinterpret_cast<IMP>(&didFailNavigation));
        add(@selector(webView:didFailProvisionalNavigation:withError:),
            reinterpret_cast<IMP>(&didFailNavigation));
        add(@selector(webViewWebContentProcessDidTerminate:),
            reinterpret_cast<IMP>(&webContentProcessDidTerminate));
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

// Gives AppKit the page's cursor over the whole view, which it shows as the pointer enters or
// moves over it. A cursor without a macOS match is left to WebKit.
void addPageCursorRect(id self, SEL /*cmd*/)
{
    WebViewMac* owner = ownerOf(self);
    if (owner != nullptr && owner->cursor() != nil) {
        [self addCursorRect:[self bounds] cursor:owner->cursor()];
    }
}

// Shows the page's cursor when AppKit asks for one; one without a macOS match is left to WebKit.
void showPageCursor(id self, SEL cmd, NSEvent* event)
{
    WebViewMac* owner = ownerOf(self);
    if (owner != nullptr && owner->cursor() != nil) {
        [owner->cursor() set];
        return;
    }
    objc_super superInfo{.receiver = self, .super_class = [WKWebView class]};
    reinterpret_cast<void (*)(objc_super*, SEL, NSEvent*)>(objc_msgSendSuper)(&superInfo, cmd,
                                                                              event);
}

BOOL acceptsFirstMouse(id /*self*/, SEL /*cmd*/, NSEvent* /*event*/)
{
    return YES; // A click in an inactive plugin window acts immediately.
}

void willOpenMenu(id /*self*/, SEL /*cmd*/, NSMenu* menu, NSEvent* /*event*/)
{
    [menu removeAllItems]; // No browser menu (Reload, Back): an empty one does not open.
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
        class_addMethod(created, @selector(resetCursorRects),
                        reinterpret_cast<IMP>(&addPageCursorRect),
                        superclassMethodTypes(superclass, @selector(resetCursorRects)));
        class_addMethod(created, @selector(cursorUpdate:), reinterpret_cast<IMP>(&showPageCursor),
                        superclassMethodTypes(superclass, @selector(cursorUpdate:)));
        class_addMethod(created, @selector(acceptsFirstMouse:),
                        reinterpret_cast<IMP>(&acceptsFirstMouse),
                        superclassMethodTypes(superclass, @selector(acceptsFirstMouse:)));
        class_addMethod(created, @selector(willOpenMenu:withEvent:),
                        reinterpret_cast<IMP>(&willOpenMenu),
                        superclassMethodTypes(superclass, @selector(willOpenMenu:withEvent:)));
        objc_registerClassPair(created);
        return created;
    }();
    return cls;
}

// ----- WebViewMac -----

WebViewMac::WebViewMac(NSView* parent, int width, int height, MessageHandler onMessage,
                       FrameRequestHandler onFrameRequest)
    : onMessage(std::move(onMessage)), onFrameRequest(std::move(onFrameRequest))
{
    WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
    // The page keeps nothing in WebKit's storage: settings live in the engine. Without this,
    // WebKit would keep its caches and storage on disk in the host's data folder.
    configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    schemeHandler = [[schemeHandlerClass() alloc] init];
    setOwner(schemeHandler, this);
    [configuration setURLSchemeHandler:schemeHandler forURLScheme:kSchemeName];

    messageHandler = [[messageHandlerClass() alloc] init];
    setOwner(messageHandler, this);
    WKUserContentController* contentController = configuration.userContentController;
    [contentController addScriptMessageHandler:messageHandler name:kMessageHandlerName];
    [contentController addScriptMessageHandler:messageHandler name:kFocusHandlerName];
    [contentController addScriptMessageHandler:messageHandler name:kCursorHandlerName];
    for (const char* script : {kFocusTrackingScript, kCursorTrackingScript}) {
        [contentController
            addUserScript:[[WKUserScript alloc]
                                initWithSource:[NSString stringWithUTF8String:script]
                                 injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                              forMainFrameOnly:YES]];
    }

    // The page's background colour behind it, so the editor never shows white while it loads;
    // the page is shown once loaded, and a message in its place if it cannot be.
    const NSRect bounds = NSMakeRect(0, 0, width, height);
    container = [[NSView alloc] initWithFrame:bounds];
    container.wantsLayer = YES;
    container.layer.backgroundColor = [NSColor colorWithSRGBRed:0x16 / 255.0
                                                          green:0x1b / 255.0
                                                           blue:0x21 / 255.0
                                                          alpha:1.0]
                                          .CGColor;
    notice = [NSTextField wrappingLabelWithString:@""];
    notice.alignment = NSTextAlignmentCenter;
    notice.textColor = [NSColor colorWithSRGBRed:0.85 green:0.87 blue:0.9 alpha:1.0];
    notice.frame = NSInsetRect(bounds, 24, 24);
    notice.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    notice.hidden = YES;
    [container addSubview:notice];

    webView = [[webViewClass() alloc] initWithFrame:bounds configuration:configuration];
    setOwner(webView, this);
    webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    webView.hidden = YES;
    navigationDelegate = [[navigationDelegateClass() alloc] init];
    setOwner(navigationDelegate, this);
    webView.navigationDelegate = navigationDelegate;
#ifndef NDEBUG
    if (@available(macOS 13.3, *)) {
        webView.inspectable = YES;
    }
#endif
    [container addSubview:webView];
    [parent addSubview:container];
    [webView loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:kStartUrl]]];
}

WebViewMac::~WebViewMac()
{
    // Detach every path back into this object before the webview can outlive it.
    setOwner(schemeHandler, nullptr);
    setOwner(messageHandler, nullptr);
    setOwner(navigationDelegate, nullptr);
    setOwner(webView, nullptr);
    webView.navigationDelegate = nil;
    [webView.configuration.userContentController removeAllScriptMessageHandlers];
    [webView stopLoading];
    [webView removeFromSuperview];
    [container removeFromSuperview];
    webView = nil;
    container = nil;
    messageHandler = nil;
    navigationDelegate = nil;
}

void WebViewMac::setSize(int width, int height)
{
    [container setFrameSize:NSMakeSize(width, height)];
}

void WebViewMac::showMessage(NSString* text)
{
    notice.stringValue = text;
    notice.hidden = NO;
    webView.hidden = YES;
}

void WebViewMac::pageLoaded()
{
    loaded = true;
    notice.hidden = YES;
    webView.hidden = NO;
}

void WebViewMac::pageFailed(NSError* error)
{
    // A navigation the policy refused, or one cancelled by the next, is no failure; nor is any
    // once the page is up.
    const bool refused = [error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102;
    const bool cancelled =
        [error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled;
    if (loaded || refused || cancelled) {
        return;
    }
    showMessage(@"Sineless's editor could not load its page.\nClose and reopen it; if that does "
                @"not help, restart the host.");
}

// macOS ends a page's process under memory pressure, and it can crash: the page is loaded again,
// says hello and gets the session's state anew, unless it keeps dying.
void WebViewMac::processTerminated()
{
    const auto now = std::chrono::steady_clock::now();
    while (!reloads.empty() && now - reloads.front() > std::chrono::minutes(1)) {
        reloads.pop_front();
    }
    loaded = false;
    frameTask = nil; // The page that asked is gone.
    heldFrame = nil;
    isEditableFocused = false;
    if (reloads.size() >= kMaxReloadsPerMinute) {
        showMessage(@"Sineless's editor keeps failing.\nClose and reopen it; if that does not "
                    @"help, restart the host.");
        return;
    }
    reloads.push_back(now);
    webView.hidden = YES; // Until the page is back.
    // Loaded afresh, not reloaded: a page that never finished loading has nothing to reload.
    [webView loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:kStartUrl]]];
}

bool WebViewMac::terminateForTests()
{
    // The page process's id, from WebKit's own test hook (not public API, so only tests use it);
    // killed as a crash would end it.
    const SEL identifier = NSSelectorFromString(@"_webProcessIdentifier");
    if (![webView respondsToSelector:identifier]) {
        return false;
    }
    const pid_t process = reinterpret_cast<pid_t (*)(id, SEL)>(objc_msgSend)(webView, identifier);
    return process > 0 && kill(process, SIGKILL) == 0;
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

// Frames travel as the binary body of the page's fetch: no text encoding on either side.
void WebViewMac::sendFrame(std::span<const std::byte> frame)
{
    NSData* data = [NSData dataWithBytes:frame.data() length:frame.size()];
    if (frameTask == nil) {
        heldFrame = data;
        return;
    }
    answer(frameTask, data);
    frameTask = nil;
}

void WebViewMac::requestFrame(id<WKURLSchemeTask> task)
{
    if (frameTask != nil) {
        answer(frameTask, [NSData data]); // Superseded; the page asks for one frame at a time.
    }
    frameTask = task;
    if (heldFrame != nil) {
        answer(frameTask, heldFrame);
        frameTask = nil;
        heldFrame = nil;
    }
    if (onFrameRequest) {
        onFrameRequest(*this);
    }
}

void WebViewMac::cancelFrameRequest(id<WKURLSchemeTask> task)
{
    if (task == frameTask) {
        frameTask = nil;
    }
}

void WebViewMac::answer(id<WKURLSchemeTask> task, NSData* frame)
{
    NSDictionary* headers = @{
        @"Content-Type" : @"application/octet-stream",
        @"Content-Length" : [NSString stringWithFormat:@"%lu", (unsigned long)frame.length],
        @"Cache-Control" : @"no-store",
        @"Content-Security-Policy" : @(std::string(kContentSecurityPolicy).c_str()),
    };
    [task didReceiveResponse:[[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                                         statusCode:200
                                                        HTTPVersion:@"HTTP/1.1"
                                                       headerFields:headers]];
    [task didReceiveData:frame];
    [task didFinish];
}

void WebViewMac::evaluate(NSString* script)
{
    [webView evaluateJavaScript:script completionHandler:nil];
}

void WebViewMac::evaluate(std::string_view script, std::function<void(std::string)> done)
{
    NSString* source = [[NSString alloc] initWithBytes:script.data()
                                                length:script.size()
                                              encoding:NSUTF8StringEncoding];
    auto callback = std::make_shared<std::function<void(std::string)>>(std::move(done));
    [webView evaluateJavaScript:source
              completionHandler:^(id value, NSError* /*error*/) {
                (*callback)([value isKindOfClass:[NSString class]] ? [value UTF8String] : "");
              }];
}

void WebViewMac::receive(WKScriptMessage* message)
{
    // Only the UI's own page talks to the plugin.
    WKSecurityOrigin* origin = message.frameInfo.securityOrigin;
    if (!message.frameInfo.mainFrame || ![origin.protocol isEqualToString:kSchemeName] ||
        ![origin.host isEqualToString:kUiHost] || ![message.body isKindOfClass:[NSString class]]) {
        return;
    }
    NSString* body = message.body;
    if ([message.name isEqualToString:kFocusHandlerName]) {
        isEditableFocused = [body isEqualToString:@"1"];
        return;
    }
    if ([message.name isEqualToString:kCursorHandlerName]) {
        pageCursor = cursorFor(body);
        [webView.window invalidateCursorRectsForView:webView];
        return;
    }
    if (onMessage) {
        onMessage(*this, std::string_view(body.UTF8String));
    }
}

} // namespace

std::unique_ptr<WebView> WebView::create(void* parent, int width, int height,
                                         MessageHandler onMessage,
                                         FrameRequestHandler onFrameRequest)
{
    return std::make_unique<WebViewMac>((__bridge NSView*)parent, width, height,
                                        std::move(onMessage), std::move(onFrameRequest));
}

bool terminateContentProcessForTests(WebView& view)
{
    return static_cast<WebViewMac&>(view).terminateForTests();
}

const void* cursorForTests(WebView& view)
{
    return (__bridge const void*)static_cast<WebViewMac&>(view).cursor();
}

const void* cursorForCssForTests(const char* css)
{
    return (__bridge const void*)cursorFor([NSString stringWithUTF8String:css]);
}

const void* drawnDiagonalResizeCursorForTests()
{
    return (__bridge const void*)drawnDiagonalResizeCursor();
}

} // namespace orizon::editor
