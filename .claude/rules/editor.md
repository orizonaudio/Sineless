---
paths:
  - "editor/**"
---

# Editor and web views

- A change to the Windows web view (`editor/src/win/WebViewWin.cpp`) needs its macOS
  counterpart (`editor/src/mac/WebViewMac.mm`), and the other way round, unless the commit
  says why not. Crash recovery, navigation guards, origin checks, background colour and
  failure messages must behave the same on both.
- Follow the VST3 `IPlugView` sizing sequence (`resizeView`, then the host's `onSize`), and
  handle hosts that grant a size without calling `onSize`.
- Editor tests that wait for the page should wait on a condition, not a fixed time, and
  must not depend on the network.
