#pragma once

// Marks a function as real-time safe (QG-1). The compiler checks it with -Wfunction-effects,
// and RealtimeSanitizer builds abort if it allocates, locks or makes a blocking call at run time.
#if defined(__has_cpp_attribute) && __has_cpp_attribute(clang::nonblocking)
#define ORIZON_NONBLOCKING [[clang::nonblocking]]
#else
#define ORIZON_NONBLOCKING
#endif
