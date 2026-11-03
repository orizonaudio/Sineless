#pragma once

#include "editor/FrameView.h"
#include "editor/TraceView.h"

#include <memory>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

namespace orizon::editor {

// The page's trace renderer (ui/src/render/trace-renderer.ts, AD-22) ported to Metal for the macOS
// editor's native drawing (AD-30), drawing the same picture: the screen's colour and grid,
// Compare's cancellation shading, the traces (each point's piece as quads whose coverage comes from
// the pixel's distance, one channel per trace in four-channel targets, the larger kept, then
// layered over the screen once), the previous cycle's dimming and the playhead, the moving edges at
// their exact positions with coverage. Shaders compiled from source when created.
//
// One thread at a time; it owns its coverage targets and sizes them to each target it draws into.
class MetalTraceRenderer
{
public:
    virtual ~MetalTraceRenderer() = default;
    MetalTraceRenderer(const MetalTraceRenderer&) = delete;
    MetalTraceRenderer& operator=(const MetalTraceRenderer&) = delete;

    enum class Result
    {
        kDrawn,
        // Metal refused the memory for the coverage targets at this size.
        kOutOfMemory,
    };

#ifdef __OBJC__
    // Nullptr when the shaders do not build.
    static std::unique_ptr<MetalTraceRenderer> create(id<MTLDevice> device);

    // Encodes the whole picture into `target` (BGRA8, its size in device pixels) on `buffer`.
    // `frame` may be null (nothing drawn yet: the screen and grid only). `grid` false leaves the
    // grid out (the picture-parity tests compare with the page's trace canvas alone). After
    // kDrawn, `buffer` must be committed: the renderer reuses its buffers once the GPU has run it.
    virtual Result encode(id<MTLCommandBuffer> buffer, id<MTLTexture> target,
                          const FrameView* frame, const TraceView& view, double pixelRatio,
                          bool grid = true) = 0;
#endif

    // Tests: makes every later allocation of the coverage targets fail, as Metal refusing the
    // memory would.
    virtual void refuseCoverageTargetsForTesting(bool refuse) = 0;

protected:
    MetalTraceRenderer() = default;
};

} // namespace orizon::editor
