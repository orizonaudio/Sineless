#pragma once

#include "protocol/Protocol.h"

#include <string_view>
#include <utility>
#include <vector>

// The editor's states whose controls and labels set the widths of the toolbar and the status bar.
// The editor tests of each platform check that everything fits in each, at the minimum width and
// at the default width with the wordmark, with the embedded fonts as that platform's webview
// renders them. A state's script presses buttons that are not pressed yet, so that the states
// apply in any order.
namespace orizon::editor::fit {

struct State
{
    std::string_view name;
    // Applied to the engine over the defaults, as a host's saved state would be.
    std::vector<std::pair<protocol::SettingId, double>> settings;
    // Then run in the page, for what only the page keeps.
    std::string_view script;
};

inline std::vector<State> states()
{
    using protocol::SettingId;
    return {
        {.name = "defaults", .settings = {}, .script = ""},
        {.name = "Multi, group D",
         .settings = {{SettingId::kDisplayMode, 1.0}, {SettingId::kGroup, 3.0}},
         .script = ""},
        {.name = "Compare", .settings = {{SettingId::kDisplayMode, 2.0}}, .script = ""},
        {.name = "Sec, 250 ms",
         .settings = {{SettingId::kTimeBase, 0.0}, {SettingId::kTimeSpan, 0.25}},
         .script = ""},
        {.name = "Sec, 12.5 ms",
         .settings = {{SettingId::kTimeBase, 0.0}, {SettingId::kTimeSpan, 0.0125}},
         .script = ""},
        {.name = "16 bars", .settings = {{SettingId::kBeatSpan, 10.0}}, .script = ""},
        {.name = "1/16 beat", .settings = {{SettingId::kBeatSpan, 0.0}}, .script = ""},
        {.name = "-60 dB", .settings = {{SettingId::kVerticalScale, -60.0}}, .script = ""},
        // The widest toolbar: Seconds shows the Trigger field, its Single mode adds Re-arm, and
        // Freeze reads Frozen.
        {.name = "Sec, trigger Single, frozen",
         .settings = {{SettingId::kTimeBase, 0.0}, {SettingId::kTriggerMode, 3.0}},
         .script = "{ const freeze = document.getElementById('freeze');"
                   "  if (freeze.getAttribute('aria-pressed') !== 'true') freeze.click(); } ''"},
        {.name = "trigger Side, falling, -1, Single",
         .settings = {{SettingId::kTriggerSource, 3.0},
                      {SettingId::kTriggerSlope, 1.0},
                      {SettingId::kTriggerLevel, -1.0},
                      {SettingId::kTriggerMode, 3.0}},
         .script = ""},
        {.name = "trigger Normal, -0.555",
         .settings = {{SettingId::kTriggerMode, 1.0}, {SettingId::kTriggerLevel, -0.555}},
         .script = ""},
        {.name = "A4 432.5", .settings = {{SettingId::kConcertPitch, 432.5}}, .script = ""},
        {.name = "frozen, auto-freeze, cursors, hover",
         .settings = {{SettingId::kAutoFreeze, 1.0}, {SettingId::kHoverReadout, 1.0}},
         .script = "for (const id of ['freeze', 'time-cursors', 'level-cursors']) {"
                   "  const button = document.getElementById(id);"
                   "  if (button.getAttribute('aria-pressed') !== 'true') button.click();"
                   "} ''"},
    };
}

// "shown" when the toolbar shows the wordmark beside the mark.
inline constexpr const char* kWordmarkScript =
    "getComputedStyle(document.getElementById('wordmark')).display === 'none' ? 'hidden' : 'shown'";

// The height of everything but the screen (CSS px): the editor's minimum height counts on it
// being at most protocol::kChromeHeight.
inline constexpr const char* kChromeScript =
    "String(Math.ceil(innerHeight - "
    "document.getElementById('screen').getBoundingClientRect().height))";

// Lists the page's elements that reach past its right edge or clip their content, "" if none.
inline constexpr const char* kOverflowScript = R"JS(
(() => {
  const found = [];
  for (const element of document.querySelectorAll("body *")) {
    const box = element.getBoundingClientRect();
    if (box.width === 0) continue;
    const name = element.id || element.className || element.tagName;
    if (box.right > innerWidth + 0.5) {
      found.push(name + " ends at " + Math.round(box.right));
    } else if (element.scrollWidth > element.clientWidth + 1 &&
               getComputedStyle(element).overflowX !== "visible") {
      found.push(name + " clips its content");
    }
  }
  return found.slice(0, 8).join("; ");
})()
)JS";

} // namespace orizon::editor::fit
