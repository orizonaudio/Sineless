// The layout probe (#424): loads the built page in WKWebView, on its mock transport, walks it
// (walk.js) and prints every element's box after each step. It links only Cocoa and WebKit, so it
// can be linked as any macOS SDK: WebKit keeps some layout for apps built against older SDKs,
// keyed to the main executable's, and LayoutAcrossSdksTest compares a probe linked as an old SDK
// with one linked as the current SDK.
//
//   layout_probe <ui folder> <walk.js>
//
// Prints "canary <button margin> <input margin>" (an unstyled button's and input's computed
// margins in a blank document: the legacy margin is non-zero), then for each editor width
// "width <n>" and the walk's output, then "done". Exits non-zero on a failure it can name.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <chrono>
#include <cstdio>
#include <editor/ChosenSize.h>
#include <exception>
#include <functional>
#include <memory>
#include <objc/message.h>
#include <set>
#include <span>
#include <string>

@interface LayoutProbeSchemeHandler : NSObject <WKURLSchemeHandler> {
    NSString* root;
}
- (instancetype)initWithRoot:(NSString*)folder;
@end

@implementation LayoutProbeSchemeHandler

- (instancetype)initWithRoot:(NSString*)folder
{
    self = [super init];
    if (self != nil) {
        root = [folder copy];
    }
    return self;
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task
{
    (void)webView;
    NSURL* url = task.request.URL;
    NSString* path = url.path.length > 1 ? [url.path substringFromIndex:1] : @"index.html";
    NSData* data = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:path]];
    NSString* type = @"application/octet-stream";
    NSDictionary<NSString*, NSString*>* types = @{
        @"html" : @"text/html",
        @"js" : @"text/javascript",
        @"css" : @"text/css",
        @"woff2" : @"font/woff2",
        @"svg" : @"image/svg+xml",
    };
    if (NSString* known = types[path.pathExtension]) {
        type = known;
    }
    NSDictionary* headers = @{@"Content-Type" : type, @"Cache-Control" : @"no-store"};
    [task didReceiveResponse:[[NSHTTPURLResponse alloc] initWithURL:url
                                                         statusCode:data != nil ? 200 : 404
                                                        HTTPVersion:@"HTTP/1.1"
                                                       headerFields:headers]];
    if (data != nil) {
        [task didReceiveData:data];
    }
    [task didFinish];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task
{
    (void)webView;
    (void)task;
}

@end

namespace {

constexpr std::chrono::seconds kLoadTimeout{15};
// The walk's own waits end it well before this; it only bounds a page that never answers.
constexpr std::chrono::minutes kWalkTimeout{5};

bool pumpUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    return condition();
}

// Runs `body` as an async function in the page; its resolved value as text ("" on an error).
std::string run(WKWebView* webView, NSString* body, std::chrono::milliseconds timeout)
{
    struct Result
    {
        bool done = false;
        std::string value;
    };
    auto result = std::make_shared<Result>();
    [webView callAsyncJavaScript:body
        arguments:@{}
        inFrame:nil
        inContentWorld:WKContentWorld.pageWorld
        completionHandler:^(id value, NSError* error) {
          if (error != nil) {
              result->value = std::string("failed ") + error.description.UTF8String;
          } else if ([value isKindOfClass:[NSString class]]) {
              result->value = [value UTF8String];
          }
          result->done = true;
        }];
    if (!pumpUntil([&] { return result->done; }, timeout)) {
        return "failed: the page did not answer";
    }
    return result->value;
}

bool load(WKWebView* webView, const std::function<void()>& start)
{
    start();
    pumpUntil([&] { return webView.loading; }, std::chrono::seconds(1));
    return pumpUntil([&] { return !webView.loading; }, kLoadTimeout);
}

int probe(std::span<const char* const> arguments)
{
    @autoreleasepool {
        if (arguments.size() != 3) {
            std::fprintf(stderr, "usage: layout_probe <ui folder> <walk.js>\n");
            return 2;
        }
        NSString* root = @(arguments[1]);
        NSString* walk = [NSString stringWithContentsOfFile:@(arguments[2])
                                                   encoding:NSUTF8StringEncoding
                                                      error:nil];
        if (walk == nil) {
            std::fprintf(stderr, "cannot read %s\n", arguments[2]);
            return 2;
        }
        [NSApplication sharedApplication];

        WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
        configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        LayoutProbeSchemeHandler* handler = [[LayoutProbeSchemeHandler alloc] initWithRoot:root];
        [configuration setURLSchemeHandler:handler forURLScheme:@"orizon"];

        const NSRect frame = NSMakeRect(100, 100, orizon::editor::ChosenSize::kDefaultWidth,
                                        orizon::editor::ChosenSize::kDefaultHeight);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        WKWebView* webView = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
        window.contentView = webView;
        [window orderFrontRegardless];
        // The page's animation frames run whatever the display reports, as WebKit's own tests
        // make them (as NativeTracesTests does): an unbundled process's windows are otherwise
        // occluded, and WebKit then runs none, and the frames from the mock come only on them.
        const SEL occlusion = NSSelectorFromString(@"_setWindowOcclusionDetectionEnabled:");
        if ([webView respondsToSelector:occlusion]) {
            reinterpret_cast<void (*)(id, SEL, BOOL)>(objc_msgSend)(webView, occlusion, NO);
        }

        // The canary: an unstyled button's and input's margins, in this window.
        if (!load(webView,
                  [&] { [webView loadHTMLString:@"<button>b</button><input>" baseURL:nil]; })) {
            std::printf("failed: the blank document did not load\n");
            return 1;
        }
        std::printf("canary %s\n",
                    run(webView,
                        @"const style = (s) => getComputedStyle(document.querySelector(s)).margin;"
                         "return `${style('button')} ${style('input')}`;",
                        std::chrono::seconds(5))
                        .c_str());

        const std::set<int> widths{orizon::editor::ChosenSize::kDefaultWidth,
                                   orizon::editor::ChosenSize::kMinimumWidth};
        for (const int width : widths) {
            [window setContentSize:NSMakeSize(width, orizon::editor::ChosenSize::kDefaultHeight)];
            NSURL* url = [NSURL URLWithString:@"orizon://ui/index.html"];
            if (!load(webView, [&] { [webView loadRequest:[NSURLRequest requestWithURL:url]]; })) {
                std::printf("failed: the page did not load\n");
                return 1;
            }
            const std::string output =
                run(webView, [walk stringByAppendingString:@"\nreturn await report();"],
                    std::chrono::duration_cast<std::chrono::milliseconds>(kWalkTimeout));
            std::printf("width %d\n%s\n", width, output.c_str());
            if (!output.starts_with("step ")) {
                return 1;
            }
        }
        std::printf("done\n");
        [window orderOut:nil];
        return 0;
    }
}

} // namespace

int main(int argc, const char* argv[])
{
    try {
        return probe(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "layout_probe: %s\n", error.what());
        return 1;
    }
}
