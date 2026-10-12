// Help texts, shown as tooltips while help is on. An element names its topic with a data-help
// attribute; the nearest such element under the pointer gives the tooltip.

import {
  analysisGateDbfs,
  frequencyWindowSeconds,
  levelFloorDbfs,
  matchReachSeconds,
  matchWindowSeconds,
  maxLanes,
  rollThresholdSeconds,
  summingMarginDb,
} from "../../../protocol/src/limits.ts";
import { settings } from "../../../protocol/src/settings.ts";
import { formatSeconds } from "./format.ts";

// Modifier keys as the platform names them: the page also runs in WebView2 on Windows.
export const isMac =
  typeof navigator !== "undefined" && /Mac/u.test(navigator.platform);
const command = isMac ? "⌘" : "Ctrl";
const lanesMulti = String(maxLanes);
const lanesCompare = String(maxLanes - 1);
const reach = formatSeconds(matchReachSeconds);
const matchWindow = formatSeconds(matchWindowSeconds);
const frequencyWindow = formatSeconds(frequencyWindowSeconds);
const margin = String(summingMarginDb);
const levelFloor = String(levelFloorDbfs).replace("-", "−");
const gate = String(analysisGateDbfs).replace("-", "−");
const shortestSpan = formatSeconds(settings.timeSpan.minimum);
const longestSpan = formatSeconds(settings.timeSpan.maximum);
const rollSpan = formatSeconds(rollThresholdSeconds);
const option = isMac ? "⌥" : "Alt";

export interface HelpText {
  readonly title: string;
  readonly text: string;
}

export const helpTexts = {
  help: {
    title: "Help",
    text: "Turns help on or off. While it is on, resting the pointer on any control explains what it is, what it does and how to use it.",
  },
  displayMode: {
    title: "Single, Multi or Compare",
    text: `Single shows this track's channels. Multi shows every track of this instance's group that has Sineless inserted, one lane each as the track's Mid, at most ${lanesMulti}. Compare (in beta) shows the same tracks, at most ${lanesCompare}, and the sum of those you check, to find where they cancel. Keep all instances in the same plugin hosting mode, and put Sineless last on each track so lanes line up.`,
  },
  compareTrack: {
    title: "Compare",
    text: "Check a track to add it to the sum and measure it against this track, which is always part of the comparison. Uncheck it to leave it out.",
  },
  previewFix: {
    title: "Preview fix",
    text: "Redraws the sum as if every checked track were moved by its offset and, if needed, flipped in polarity, and shows the summing gain that would give. Apply it in your host, with track delay and a phase or polarity switch.",
  },
  sumGain: {
    title: "Summing gain",
    text: `How loud the checked tracks are together, against unrelated signals of the same levels. Within ${margin} dB of 0 is harmless; above that they reinforce, and below it they cancel. Where they cancel strongly, the sum's lane is shaded red, provided a checked track is related to yours: unrelated tracks only beat against each other. A dash means they never sound together, or nothing is measured yet. Measured over the visible part, but only where at least two of them sound together above ${gate} dBFS, so a long view does not water down a short clash.`,
  },
  match: {
    title: "Against this track",
    text: `How late this track is against yours, within ${reach} (negative: early), with Ø when its polarity is opposite, and how alike they move, from +1 (in step) through 0 (unrelated) to -1 (opposite), measured over the ${matchWindow} before the playhead (zoomed away from it, the newest ${matchWindow} shown). Unrelated means no shift makes them match clearly, as with a kick and a hi-hat: there is nothing to fix, and Preview fix leaves the track as it is. Only audio above ${gate} dBFS is compared; a dash means they never sound together there, or the track is not measured yet.`,
  },
  group: {
    title: "Group",
    text: "Instances in the same group appear together in Multi and Compare modes. Use different groups to keep separate comparisons apart, such as drums in A and bass in B. Every instance belongs to a group, whatever its own mode.",
  },
  channelViews: {
    title: "Views",
    text: "Which channels this track shows. In Multi mode every track shows its Mid, so these apply in Single mode.",
  },
  status: {
    title: "Status",
    text: "What the display is doing: Trig'd when it is locked to a trigger, Waiting while Normal or Single mode waits for one, Roll, Sync, Frozen or Zoom for the current mode, and Filling while a sweep longer than the capture draws its first pass (Partial when frozen before it did).",
  },
  peak: {
    title: "Peak",
    text: `The highest sample level over the whole span, in dBFS: of the trigger source in Sec mode, and of the Mid in Beats mode. Below ${levelFloor} dBFS it reads −∞. Indicative, not a standards-compliant meter.`,
  },
  rms: {
    title: "RMS",
    text: `The average power over the whole span, in dBFS, measured like Peak: a full-scale sine reads −3 dB RMS. Below ${levelFloor} dBFS it reads −∞. Indicative, not a standards-compliant meter.`,
  },
  frequency: {
    title: "Frequency",
    text: `An estimate of the pitch over the last ${frequencyWindow}, from how often the signal rises through its average: of the trigger source in Sec mode, and of the Mid in Beats mode. A dash means no steady repetition was found, or the signal is below ${gate} dBFS.`,
  },
  screen: {
    title: "Screen",
    text: `The signal over time. ${command} + scroll or pinch to zoom around the pointer, drag across to zoom to a region, swipe sideways to pan, double-click to reset. In Sec mode, while frozen and not zoomed, swipe to look further back.`,
  },
  triggerMarker: {
    title: "Trigger level",
    text: "The level the signal must cross to trigger. Drag the arrow up or down to set it.",
  },
  timeCursor: {
    title: "Time cursor",
    text: `Drag sideways to move it. It snaps to zero crossings and to the highest and lowest points; hold ${option} to place it freely.`,
  },
  levelCursor: {
    title: "Level cursor",
    text: `Drag up or down to move it. It snaps to the highest and lowest levels and to zero; hold ${option} to place it freely.`,
  },
  timeSpan: {
    title: "Time span",
    text: `How much time the screen shows, from ${shortestSpan} to ${longestSpan}. Drag up or down or scroll to step through 1-2-5 values (Shift for fine steps); double-click to type one, such as 20 ms. Spans above ${rollSpan} scroll continuously.`,
  },
  beatSpan: {
    title: "Beat span",
    text: "How many beats or bars the screen shows at the host tempo. Drag up or down or scroll to change it; double-click to type.",
  },
  timeBase: {
    title: "Time base",
    text: "Beats sweeps across the screen in time with the host, from bar line to bar line, with a playhead. Sec shows a fixed amount of time, triggered by the signal.",
  },
  verticalScale: {
    title: "Vertical scale",
    text: "The level at the top of the screen, from −60 to +6 dBFS. Drag up or down or scroll to step by 3 dB (Shift for 1 dB); double-click to type.",
  },
  viewLeft: {
    title: "Left",
    text: "Shows or hides the left channel. At least one view stays on.",
  },
  viewRight: {
    title: "Right",
    text: "Shows or hides the right channel. At least one view stays on.",
  },
  viewMid: {
    title: "Mid",
    text: "Shows or hides the mid signal, (L + R) / 2: what both channels share. At least one view stays on.",
  },
  viewSide: {
    title: "Side",
    text: "Shows or hides the side signal, (L − R) / 2: the difference between the channels, which carries the stereo width. At least one view stays on.",
  },
  layout: {
    title: "Layout",
    text: "Overlay draws all views on one screen; Stack gives each view its own lane.",
  },
  trigger: {
    title: "Trigger",
    text: "Decides when the display draws, so a repeating signal stands still. Click the title for source, slope, level, hysteresis and hold-off. Not used in Beats mode, where the display follows the host tempo instead.",
  },
  modeAuto: {
    title: "Auto",
    text: "Draws on every trigger, and keeps drawing without one when none arrives for a moment. The usual choice.",
  },
  modeNormal: {
    title: "Normal",
    text: "Draws only when the signal triggers; otherwise the last trace stays.",
  },
  modeFree: {
    title: "Free",
    text: "Draws continuously without waiting for a trigger.",
  },
  modeSingle: {
    title: "Single",
    text: "Captures one triggered trace and freezes it. Re-arm waits for the next.",
  },
  rearm: {
    title: "Re-arm",
    text: "In Single mode, waits for the next trigger again.",
  },
  triggerSource: {
    title: "Trigger source",
    text: "Which signal the trigger watches. Readouts and cursor snapping use it too. Not used in Multi mode, which shows each track's Mid.",
  },
  triggerSlope: {
    title: "Slope",
    text: "Rising triggers when the signal crosses the level going up; Falling, going down.",
  },
  triggerLevel: {
    title: "Trigger level",
    text: "The sample value the signal must cross, from −1 to +1. Drag, scroll or double-click to type; or drag the arrow at the screen's left edge.",
  },
  triggerHysteresis: {
    title: "Hysteresis",
    text: "How far the signal must move away from the level before it can trigger again. Raise it when noise causes false triggers.",
  },
  triggerHoldOff: {
    title: "Hold-off",
    text: "The least time between triggers. Use it to lock onto the start of a repeating pattern instead of a hit inside it. Drag up or down or scroll to step through 1-2-5 values (Shift for fine steps); double-click to type.",
  },
  hoverReadout: {
    title: "Hover readout",
    text: "When on, resting the pointer on a trace shows, beside it, the position there, the level, and the length, frequency and note of the cycle around it. A dotted line marks the column or point read, and a faint band the cycle measured.",
  },
  displayOptions: {
    title: "Display options",
    text: "Click the title for further display settings, such as the concert pitch that note names refer to.",
  },
  concertPitch: {
    title: "Concert pitch",
    text: "The pitch of A4 that note names in the readouts refer to, from 400 to 500 Hz. Drag up or down or scroll to change it (Shift for 0.1 Hz); double-click to type.",
  },
  freeze: {
    title: "Freeze",
    text: "Holds the display so you can study it. Scale, views, zoom and cursors keep working, and in Sec mode you can swipe to look back. Click again to go live.",
  },
  autoFreeze: {
    title: "Auto-freeze",
    text: "When on, the display freezes when the host stops and goes live again when it plays. A freeze you make yourself is kept.",
  },
  timeCursors: {
    title: "Time cursors",
    text: "Two vertical lines, A and B, for measuring time. Drag them; the readout shows the time between them, its frequency and nearest note, and the beats in Beats mode.",
  },
  levelCursors: {
    title: "Level cursors",
    text: "Two horizontal lines, 1 and 2, for measuring levels. Drag them; the readout shows each level in dBFS and the difference in dB.",
  },
} as const satisfies Record<string, HelpText>;

export type HelpTopic = keyof typeof helpTexts;

export function isHelpTopic(name: string): name is HelpTopic {
  return Object.hasOwn(helpTexts, name);
}
