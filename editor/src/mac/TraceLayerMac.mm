#include "TraceLayerMac.h"

#include "MetalTraceRenderer.h"
#include "UniqueClassName.h"
#include "engine/MonotonicClock.h"

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <objc/message.h>
#include <objc/runtime.h>
#include <utility>

namespace orizon::editor {

namespace {

// MTLCommandBufferErrorDeviceRemoved, by its value: the SDK deprecates the name (it cannot occur on
// Apple silicon) and the build treats warnings as errors.
constexpr long kDeviceRemoved = 11;
// How long destroying the layer waits for its thread to finish; past that the thread finishes on
// its own, holding what it uses, and calls nothing of the editor's.
constexpr std::int64_t kStopWaitNanos = 2'000'000'000;

// What the main thread, the render thread and Metal's completion handlers share. The render thread
// keeps it alive while it runs.
struct Shared
{
    Shared(engine::FramePacing& pacing, TraceLayer::Hooks hooks)
        : pacing(pacing), hooks(std::move(hooks))
    {}

    engine::FramePacing& pacing;
    TraceLayer::Hooks hooks;

    // Held through every callback and every block run on the render thread; once `stopped`, none
    // touches the pacing or the hooks.
    std::mutex callbackMutex;
    bool stopped = false;

    std::mutex viewMutex;
    std::optional<TraceView> pendingView;

    std::atomic<bool> redraw{true};
    std::atomic<double> pixelRatio{1.0};
    std::atomic<bool> lost{false};
    std::atomic<std::uint64_t> failingSince{0};
    std::atomic<long> failCode{-1};
    std::atomic<bool> refuseTargets{false};
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> submitted{0};
    PresentedCounts presentedCounts;
    std::atomic<int> ticksPerFrame{1};
    std::atomic<std::size_t> drawnWidth{0};
    std::atomic<std::size_t> drawnHeight{0};
    // The frame barcode on the picture last drawn (any thread reads them, for tests).
    std::atomic<bool> barcodeDrawn{false};
    std::atomic<std::uint64_t> barcodePicture{0};
    std::atomic<std::uint64_t> barcodeDrawnFor{0};

    // Set before the render thread signals that it has started.
    CFRunLoopRef runLoop = nullptr;
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);

    // Render thread only.
    id<MTLCommandQueue> queue = nil;
    std::unique_ptr<MetalTraceRenderer> renderer;
    CAMetalDisplayLink* link = nil;
    id delegate = nil;
    bool exit = false;
    bool lostHandled = false;
    // The frame barcode: pictures drawn since the layer was made, and the link's target time, in
    // host seconds, of the refresh the next picture is drawn for.
    std::uint64_t barcodePictures = 0;
    double targetTime = 0.0;
    std::vector<std::byte> frame;
    std::vector<std::byte> incoming;
    bool haveFrame = false;
    std::optional<TraceView> view;

    ~Shared()
    {
        if (runLoop != nullptr) {
            CFRelease(runLoop);
        }
    }
};

// Native drawing stopped for good: reported once, on the main thread, unless the layer is gone.
// By value: the block captures it, and a block captures a C++ reference as a reference.
// NOLINTNEXTLINE(performance-unnecessary-value-param): captured by the block, as above.
void setLost(std::shared_ptr<Shared> state)
{
    if (state->lost.exchange(true)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      bool stopped = false;
      {
          const std::scoped_lock lock(state->callbackMutex);
          stopped = state->stopped;
      }
      if (!stopped && state->hooks.lost) {
          state->hooks.lost();
      }
    });
}

// Runs `block` on the render thread, between callbacks, unless the layer has stopped.
// NOLINTNEXTLINE(performance-unnecessary-value-param): captured by the block, as setLost's.
void perform(std::shared_ptr<Shared> state, void (^block)(Shared& shared))
{
    CFRunLoopPerformBlock(state->runLoop, kCFRunLoopDefaultMode, ^{
      const std::scoped_lock lock(state->callbackMutex);
      if (!state->stopped) {
          block(*state);
      }
    });
    CFRunLoopWakeUp(state->runLoop);
}

// A command buffer finished (Metal's thread). Persistent errors stop native drawing at once; any
// other error does once buffers have failed for kFailureSecondsBeforeLost, and meanwhile the
// picture is drawn again at the next callback.
void completed(const std::shared_ptr<Shared>& shared, id<MTLCommandBuffer> buffer)
{
    const long injected = shared->failCode.load();
    const long code = injected >= 0                                  ? injected
                      : buffer.status == MTLCommandBufferStatusError ? buffer.error.code
                                                                     : -1;
    if (code < 0) {
        shared->failingSince.store(0);
        return;
    }
    const bool persistent = code == kDeviceRemoved || code == MTLCommandBufferErrorAccessRevoked ||
                            code == MTLCommandBufferErrorNotPermitted;
    const std::uint64_t now = engine::monotonicNanos();
    std::uint64_t since = 0;
    if (shared->failingSince.compare_exchange_strong(since, now)) {
        since = now;
    }
    const auto failingFor = static_cast<double>(now - since) / 1e9;
    if (persistent || failingFor >= TraceLayer::kFailureSecondsBeforeLost) {
        setLost(shared);
        return;
    }
    shared->redraw.store(true);
}

void draw(const std::shared_ptr<Shared>& shared, id<CAMetalDrawable> drawable)
{
    Shared& s = *shared;
    if (s.refuseTargets.exchange(false)) {
        s.renderer->refuseCoverageTargetsForTesting(true);
    }
    id<MTLCommandBuffer> buffer = [s.queue commandBuffer];
    const std::optional<FrameView> frame =
        s.haveFrame ? FrameView::read(s.frame) : std::optional<FrameView>{};
    std::optional<BarcodeValues> barcode;
    if (s.hooks.frameBarcode) {
        constexpr std::uint64_t kRange = std::uint64_t{1} << protocol::kBarcodeDataBits;
        const auto ticks = static_cast<std::uint64_t>(std::llround(
            s.targetTime * 1000.0 * static_cast<double>(protocol::kBarcodeTicksPerMillisecond)));
        barcode = BarcodeValues{.picture = s.barcodePictures++, .drawnFor = ticks % kRange};
    }
    const auto result =
        s.renderer->encode(buffer, drawable.texture, frame ? &*frame : nullptr, *s.view,
                           s.pixelRatio.load(), true, barcode ? &*barcode : nullptr);
    if (result != MetalTraceRenderer::Result::kDrawn) {
        setLost(shared);
        return;
    }
    [buffer presentDrawable:drawable];
    const std::weak_ptr<Shared> weak = shared;
    // When it reaches the screen, or 0.0 if dropped: counted against the display's period as it
    // was when drawn (the pacing may be gone by the time the handler runs).
    const double period = s.pacing.displayPeriodSeconds();
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
      if (const auto alive = weak.lock()) {
          alive->presentedCounts.add(shown.presentedTime, period);
      }
    }];
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
      if (const auto alive = weak.lock()) {
          completed(alive, done);
      }
    }];
    [buffer commit];
    s.submitted.fetch_add(1);
    s.drawnWidth.store(drawable.texture.width);
    s.drawnHeight.store(drawable.texture.height);
    if (barcode) {
        s.barcodePicture.store(barcode->picture);
        s.barcodeDrawnFor.store(barcode->drawnFor);
        s.barcodeDrawn.store(true);
    }
}

// One display link callback (render thread): the clock steps, the newest frame and trace view are
// drawn when either changed, and the next shown refresh's frame is asked for.
void callback(const std::shared_ptr<Shared>& shared, CAMetalDisplayLinkUpdate* update)
{
    Shared& s = *shared;
    const std::scoped_lock lock(s.callbackMutex);
    if (s.stopped) {
        return;
    }
    s.callbacks.fetch_add(1);
    if (s.lost.load()) {
        if (!s.lostHandled) {
            s.lostHandled = true;
            s.pacing.lose();
            s.link.paused = YES;
        }
        return;
    }
    const auto decision = s.pacing.callback(update.targetPresentationTimestamp,
                                            CACurrentMediaTime(), engine::monotonicNanos());
    s.targetTime = update.targetPresentationTimestamp;
    s.ticksPerFrame.store(s.pacing.ticks());
    bool changed = s.redraw.exchange(false);
    if (s.hooks.takeFrame(s.incoming)) {
        std::swap(s.frame, s.incoming);
        s.haveFrame = true;
        changed = true;
    }
    {
        const std::scoped_lock viewLock(s.viewMutex);
        if (s.pendingView) {
            s.view = std::move(s.pendingView);
            s.pendingView.reset();
            changed = true;
        }
    }
    if (changed && s.view) {
        draw(shared, update.drawable);
    }
    if (decision.requestFrame) {
        s.hooks.requestFrame();
    }
}

std::optional<bool>& occlusionForTests()
{
    static std::optional<bool> occluded;
    return occluded;
}

// Whether no part of the window is on screen (behind others, minimised, on another space): the
// link pauses.
bool isOccluded(NSWindow* window)
{
    return occlusionForTests().value_or((window.occlusionState & NSWindowOcclusionStateVisible) ==
                                        0);
}

// ----- Objective-C classes, made at run time with names unique to this binary -----

const char kOwnerKeyTag = 0;
const void* const kOwnerKey = &kOwnerKeyTag;

class LayerMac;

template <typename Owner> Owner* ownerOf(id object)
{
    NSValue* value = objc_getAssociatedObject(object, kOwnerKey);
    return static_cast<Owner*>(value.pointerValue);
}

void setOwner(id object, void* owner)
{
    objc_setAssociatedObject(object, kOwnerKey,
                             owner != nullptr ? [NSValue valueWithPointer:owner] : nil,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void needsUpdate(id self, SEL /*cmd*/, CAMetalDisplayLink* /*link*/,
                 CAMetalDisplayLinkUpdate* update)
{
    // The render thread's own reference, held by the thread for as long as the link exists.
    if (auto* shared = ownerOf<std::shared_ptr<Shared>>(self)) {
        callback(*shared, update);
    }
}

Class delegateClass()
{
    static Class cls = [] {
        Class created = objc_allocateClassPair([NSObject class],
                                               uniqueClassName("TraceLinkDelegate").c_str(), 0);
        Protocol* protocol = @protocol(CAMetalDisplayLinkDelegate);
        const SEL selector = @selector(metalDisplayLink:needsUpdate:);
        class_addMethod(created, selector, reinterpret_cast<IMP>(&needsUpdate),
                        protocol_getMethodDescription(protocol, selector, YES, YES).types);
        class_addProtocol(created, protocol);
        objc_registerClassPair(created);
        return created;
    }();
    return cls;
}

class LayerMac final : public TraceLayer
{
public:
    LayerMac(NSView* container, NSView* above, engine::FramePacing& pacing, Hooks hooks);
    ~LayerMac() override;
    LayerMac(const LayerMac&) = delete;
    LayerMac& operator=(const LayerMac&) = delete;

    [[nodiscard]] NSView* view() const override { return layerView; }
    void setTraceView(const TraceView& view) override;
    void layout() override;
    void setPageShown(bool shown) override;
    [[nodiscard]] bool lost() const override { return shared->lost.load(); }
    [[nodiscard]] Stats statsForTesting() const override
    {
        std::optional<BarcodeValues> barcode;
        if (shared->barcodeDrawn.load()) {
            barcode = BarcodeValues{.picture = shared->barcodePicture.load(),
                                    .drawnFor = shared->barcodeDrawnFor.load()};
        }
        return {.callbacks = shared->callbacks.load(),
                .submitted = shared->submitted.load(),
                .shown = shared->presentedCounts.totals(),
                .ticksPerFrame = shared->ticksPerFrame.load(),
                .drawnWidth = shared->drawnWidth.load(),
                .drawnHeight = shared->drawnHeight.load(),
                .barcode = barcode};
    }
    void failCommandBuffersForTesting(std::optional<long> code) override
    {
        shared->failCode.store(code.value_or(-1));
    }
    void refuseCoverageTargetsForTesting() override { shared->refuseTargets.store(true); }

    void windowChanged();

private:
    void update();
    void observe(NSWindow* window);
    void stopObserving();

    NSView* container;
    NSView* layerView;
    CAMetalLayer* metalLayer;
    std::shared_ptr<Shared> shared;
    bool pageShown = false;
    bool viewKnown = false;
    bool rectVisible = true;
    bool occluded = false;
    bool running = false;
    std::vector<id> observers;
};

CALayer* makeBackingLayer(id /*self*/, SEL /*cmd*/)
{
    return [CAMetalLayer layer];
}

NSView* hitTest(id /*self*/, SEL /*cmd*/, NSPoint /*point*/)
{
    return nil; // The page above takes every event.
}

// The container resized: the screen's rectangle from its new size, not autoresizing, so below the
// page's minimum width the layer keeps the page's layout and is clipped with it.
void resizeWithOldSuperviewSize(id self, SEL /*cmd*/, NSSize /*size*/)
{
    if (auto* owner = ownerOf<LayerMac>(self)) {
        owner->layout();
    }
}

void viewDidChangeBackingProperties(id self, SEL cmd)
{
    const struct objc_super parent = {.receiver = self, .super_class = [NSView class]};
    reinterpret_cast<void (*)(const struct objc_super*, SEL)>(objc_msgSendSuper)(&parent, cmd);
    if (auto* owner = ownerOf<LayerMac>(self)) {
        owner->layout(); // A move between a Retina and a 1x display changes no size.
    }
}

void viewDidMoveToWindow(id self, SEL cmd)
{
    const struct objc_super parent = {.receiver = self, .super_class = [NSView class]};
    reinterpret_cast<void (*)(const struct objc_super*, SEL)>(objc_msgSendSuper)(&parent, cmd);
    if (auto* owner = ownerOf<LayerMac>(self)) {
        owner->windowChanged();
    }
}

Class layerViewClass()
{
    static Class cls = [] {
        Class created =
            objc_allocateClassPair([NSView class], uniqueClassName("TraceLayerView").c_str(), 0);
        const auto add = [&](SEL selector, IMP implementation) {
            class_addMethod(
                created, selector, implementation,
                method_getTypeEncoding(class_getInstanceMethod([NSView class], selector)));
        };
        add(@selector(makeBackingLayer), reinterpret_cast<IMP>(&makeBackingLayer));
        add(@selector(hitTest:), reinterpret_cast<IMP>(&hitTest));
        add(@selector(resizeWithOldSuperviewSize:),
            reinterpret_cast<IMP>(&resizeWithOldSuperviewSize));
        add(@selector(viewDidChangeBackingProperties),
            reinterpret_cast<IMP>(&viewDidChangeBackingProperties));
        add(@selector(viewDidMoveToWindow), reinterpret_cast<IMP>(&viewDidMoveToWindow));
        objc_registerClassPair(created);
        return created;
    }();
    return cls;
}

LayerMac::LayerMac(NSView* container, NSView* above, engine::FramePacing& pacing, Hooks hooks)
    : container(container), shared(std::make_shared<Shared>(pacing, std::move(hooks)))
{
    layerView = [[layerViewClass() alloc] initWithFrame:NSZeroRect];
    layerView.wantsLayer = YES;
    metalLayer = static_cast<CAMetalLayer*>(layerView.layer);
    metalLayer.device = MTLCreateSystemDefaultDevice();
    metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    metalLayer.opaque = YES;
    // Presented from the display link's thread, never inside the main thread's transaction.
    metalLayer.presentsWithTransaction = NO;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    metalLayer.colorspace = srgb;
    CGColorSpaceRelease(srgb);
    // Inside the border's curve: the page's border, drawn above, overlaps the layer's edge.
    metalLayer.cornerRadius = protocol::kScreenRadius - protocol::kScreenBorder;
    metalLayer.masksToBounds = YES;
    layerView.hidden = YES;
    // AppKit tells a subview of its superview's size changes only when it has a mask; the view
    // lays itself out from the screen's rectangle instead (resizeWithOldSuperviewSize:).
    layerView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    const std::shared_ptr<Shared> state = shared;
    CAMetalLayer* layer = metalLayer;
    NSThread* thread = [[NSThread alloc] initWithBlock:^{
      state->runLoop = CFRunLoopGetCurrent();
      CFRetain(state->runLoop);
      // A port keeps the run loop waiting while the link is paused.
      [NSRunLoop.currentRunLoop addPort:[NSMachPort port] forMode:NSDefaultRunLoopMode];
      dispatch_semaphore_signal(state->started);
      if (layer.device != nil) {
          state->queue = [layer.device newCommandQueue];
          state->renderer = MetalTraceRenderer::create(layer.device);
      }
      {
          const std::scoped_lock lock(state->callbackMutex);
          if (state->queue == nil || state->renderer == nullptr) {
              // Neither is expected on macOS 14: the one failure path, native graphics lost.
              state->lostHandled = true;
              state->pacing.lose();
              setLost(state);
          } else {
              state->delegate = [[delegateClass() alloc] init];
              // The delegate's reference to what it draws with, freed with the thread.
              auto* reference = new std::shared_ptr<Shared>(state);
              setOwner(state->delegate, reference);
              state->link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
              state->link.delegate = state->delegate;
              state->link.preferredFrameLatency = 1.0;
              state->link.paused = YES;
              [state->link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSDefaultRunLoopMode];
          }
      }
      while (!state->exit) {
          [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:NSDate.distantFuture];
      }
      [state->link invalidate];
      state->link = nil;
      if (state->delegate != nil) {
          delete ownerOf<std::shared_ptr<Shared>>(state->delegate);
          setOwner(state->delegate, nullptr);
          state->delegate = nil;
      }
      state->renderer.reset(); // Waits for the GPU to finish with its buffers.
      state->queue = nil;
      dispatch_semaphore_signal(state->finished);
    }];
    thread.name = @"Sineless traces";
    thread.qualityOfService = NSQualityOfServiceUserInteractive;
    [thread start];
    dispatch_semaphore_wait(shared->started, DISPATCH_TIME_FOREVER);
    // Added once its thread runs: moving into the window already calls back.
    setOwner(layerView, this);
    [container addSubview:layerView positioned:NSWindowBelow relativeTo:above];
    windowChanged();
}

LayerMac::~LayerMac()
{
    {
        const std::scoped_lock lock(shared->callbackMutex);
        shared->stopped = true;
    }
    stopObserving();
    setOwner(layerView, nullptr);
    const std::shared_ptr<Shared> state = shared;
    CFRunLoopPerformBlock(state->runLoop, kCFRunLoopDefaultMode, ^{
      state->exit = true;
      CFRunLoopStop(CFRunLoopGetCurrent());
    });
    CFRunLoopWakeUp(state->runLoop);
    dispatch_semaphore_wait(state->finished, dispatch_time(DISPATCH_TIME_NOW, kStopWaitNanos));
    [layerView removeFromSuperview];
}

void LayerMac::setTraceView(const TraceView& view)
{
    {
        const std::scoped_lock lock(shared->viewMutex);
        shared->pendingView = view;
    }
    viewKnown = true;
    update();
}

void LayerMac::setPageShown(bool shown)
{
    pageShown = shown;
    if (!shown) {
        viewKnown = false; // The next page sends its own.
    }
    update();
}

void LayerMac::layout()
{
    const NSSize size = container.bounds.size;
    const ScreenRect rect = ScreenRect::of(size.width, size.height);
    NSWindow* window = layerView.window;
    const double scale =
        window != nil ? window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    // From the top on the page; AppKit's origin is at the bottom unless the container is flipped.
    const double y = container.isFlipped ? rect.y : size.height - rect.y - rect.height;
    layerView.frame = NSMakeRect(rect.x, y, rect.width, rect.height);
    metalLayer.contentsScale = scale;
    // As the page sizes its canvas: the screen's size in device pixels, rounded.
    metalLayer.drawableSize = CGSizeMake(std::max(1.0, std::round(rect.width * scale)),
                                         std::max(1.0, std::round(rect.height * scale)));
    [CATransaction commit];
    shared->pixelRatio.store(scale);
    shared->redraw.store(true);
    rectVisible = rect.visible;
    update();
}

void LayerMac::windowChanged()
{
    NSWindow* window = layerView.window;
    observe(window);
    occluded = window != nil && isOccluded(window);
    if (window.screen != nil) {
        const double rate = static_cast<double>(window.screen.maximumFramesPerSecond);
        perform(shared, ^(Shared& state) {
          state.pacing.setScreenRate(rate);
        });
    }
    layout();
}

void LayerMac::stopObserving()
{
    for (id observer : observers) {
        [NSNotificationCenter.defaultCenter removeObserver:observer];
    }
    observers.clear();
}

void LayerMac::observe(NSWindow* window)
{
    stopObserving();
    if (window == nil) {
        return;
    }
    // Owned by the layer and removed before it goes: the blocks never outlive it.
    LayerMac* self = this;
    observers.push_back([NSNotificationCenter.defaultCenter
        addObserverForName:NSWindowDidChangeOcclusionStateNotification
                    object:window
                     queue:nil
                usingBlock:^(NSNotification* /*note*/) {
                  self->occluded = isOccluded(window);
                  self->update();
                }]);
    observers.push_back([NSNotificationCenter.defaultCenter
        addObserverForName:NSWindowDidChangeScreenNotification
                    object:window
                     queue:nil
                usingBlock:^(NSNotification* /*note*/) {
                  self->windowChanged();
                }]);
}

// Shown once the page is up and has said how to draw, at a screen taller than the chrome; the
// link runs while shown on screen, and is paused (the clock on the monotonic time) otherwise.
void LayerMac::update()
{
    const bool visible = pageShown && viewKnown && rectVisible;
    layerView.hidden = !visible;
    const bool run = visible && layerView.window != nil && !occluded;
    if (run == running) {
        return;
    }
    running = run;
    perform(shared, ^(Shared& state) {
      if (state.link == nil || state.lostHandled) {
          return;
      }
      state.link.paused = !run;
      if (run) {
          state.redraw.store(true); // A current frame at once.
      } else {
          state.pacing.pause();
          state.presentedCounts.restart();
      }
    });
}

} // namespace

void TraceLayer::setOcclusionForTesting(std::optional<bool> occluded)
{
    occlusionForTests() = occluded;
}

std::unique_ptr<TraceLayer> TraceLayer::create(NSView* container, NSView* above,
                                               engine::FramePacing& pacing, Hooks hooks)
{
    return std::make_unique<LayerMac>(container, above, pacing, std::move(hooks));
}

} // namespace orizon::editor
