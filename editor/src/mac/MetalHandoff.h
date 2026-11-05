#pragma once

// Objective-C++ only: the objects are Metal's (MetalTraceRenderer.h holds its Metal parts so too).
#ifdef __OBJC__
#import <Foundation/Foundation.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#define ORIZON_METAL_HANDOFF_TSAN 1
#endif
#endif

namespace orizon::editor {

// Tells ThreadSanitizer that Metal runs a handler only after it was added. Adding a handler copies
// its block, and what it captured, on the adding thread; Metal runs it later on a thread of its own
// ("an undefined thread", Metal Programming Guide), once the command buffer has completed or the
// drawable is shown. Metal orders the two (a handler is added before commit), but through its
// driver, which ThreadSanitizer does not see, so it reported the handler's first read of what it
// captured as a race (#327). The adding thread calls handlerAdded on the command buffer or drawable
// right after adding the handler, and the handler calls handlerRuns on the one Metal passes it,
// first: never on a capture, as reading one before is the access reported. The address only keys
// ThreadSanitizer's record; nothing is read there. Nothing outside ThreadSanitizer builds, as in
// Dawn's Metal queue (QueueMTL.mm).
inline void handlerAdded([[maybe_unused]] id object)
{
#if defined(ORIZON_METAL_HANDOFF_TSAN)
    __tsan_release((__bridge void*)object);
#endif
}

inline void handlerRuns([[maybe_unused]] id object)
{
#if defined(ORIZON_METAL_HANDOFF_TSAN)
    __tsan_acquire((__bridge void*)object);
#endif
}

} // namespace orizon::editor
#endif
