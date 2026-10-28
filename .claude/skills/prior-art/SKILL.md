---
name: prior-art
description: Research how a problem is officially and commonly solved before writing a change. Use when a change (a fix, a feature or any other) involves a platform, SDK, host or standard - WebView2, WKWebView, Win32 (DPI, input, windows), VST3, Audio Units, CoreAudio, DAW host behaviour, DOM events, CSS, CMake/MSVC, or audio measurement conventions (levels, metering, pitch) - and before a second attempt at any fix whose first attempt failed.
---

# Prior art before code

Agents on this project have reached complex, trial-and-error fixes where the vendor or a
widely used project already documented a simpler one (e.g. the WebView2 handle leak, fixed
in the end by hiding the view before closing it). Research first; it is cheaper.

## 1. Find the sources

Read, in parallel where you can (background subagents work well):

1. **The vendor's guidance:** API reference, best-practice and known-issue pages, official
   samples, and its issue tracker (e.g. MicrosoftEdge/WebView2Feedback).
2. **2–3 widely used projects** that solved the same problem: JUCE, iPlug2, VSTGUI, CHOC,
   DPF, wry/Tauri, Chromium, the VST3 SDK. Read their source, not forum posts, and note
   the file and line.
3. **The relevant standard**, where one exists: AES17, ITU-R BS.1770, EBU R128, the CSS
   specifications.

If a documentation site is blocked, read its source repository:
MicrosoftDocs/edge-developer, MicrosoftDocs/win32, MicrosoftDocs/sdk-api,
steinbergmedia/vst3_pluginterfaces, vst3_public_sdk, vst3_dev_portal, apple/AudioUnitSDK,
or the XML docs inside the WebView2 NuGet package. Clone reference projects at a pinned tag
into the session scratchpad, never into the repository.

## 2. Decide

- Prefer the documented or widely adopted approach.
- Do something different only with a written reason: what the documented approach does not
  cover, and the evidence.
- Then find every place in Sineless that shares the cause (see CLAUDE.md, "Fixing a bug"), or,
  for a feature, every place that must change with it.

## 3. Record it

If the change has a plan (`plan-before-code`), put the findings in its Prior art section too.
End the change's commit message with a short note:

```
Prior art: WebView2 docs (put_IsVisible: "set to false when minimized"); JUCE
juce_WebBrowserComponent_windows.cpp:1194 does the same; WebView2Feedback #5704 open.
```

If nothing relevant exists, say so and where you searched: `Prior art: none found
(WebView2 docs, WebView2Feedback, JUCE, wry).`
