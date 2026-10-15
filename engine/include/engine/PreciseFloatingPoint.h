#pragma once

// The engine measures around samples that are not finite and sends NaN for every value it has no
// measurement of, so it must not be compiled with a fast floating-point model, which lets an
// optimised build assume there is no NaN or infinity. The VST3 SDK's CMake turns on /fp:fast for
// every target on Windows; cmake/ProjectOptions.cmake turns it off again for ours. A source that
// includes this fails to build if that ever stops working.
#if defined(_M_FP_FAST) || defined(__FAST_MATH__)
#error "Built with a fast floating-point model: the engine needs /fp:precise (no -ffast-math)."
#endif
