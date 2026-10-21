#pragma once

// For tests of the macOS UI dispatcher (UiDispatcherMac.mm).

namespace orizon::editor {

// How many of the dispatchers' repeating timers have been created and not yet freed.
int liveTimersForTests();

} // namespace orizon::editor
