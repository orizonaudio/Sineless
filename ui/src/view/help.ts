// Help texts, shown as tooltips while help is on. An element names its topic with a data-help
// attribute; the nearest such element under the pointer gives the tooltip.

import type { TriggerMode } from "../../../protocol/src/enums.ts";
import {
  analysisGateDbfs,
  gridColumnsCoarse,
  gridColumnsFine,
  gridColumnsMedium,
  gridCoarseLevelStride,
  gridFineBeatLineGap,
  gridFineLevelSubdivisions,
  gridLevelStepDb,
  levelFloorDbfs,
  matchReachSeconds,
  matchWindowSeconds,
  maxLanes,
  summingMarginDb,
} from "../../../protocol/src/limits.ts";
import { settings } from "../../../protocol/src/settings.ts";
import { choiceLabels } from "./choice-labels.ts";
import { positionFineStep, positionStep } from "./fields.ts";
import { formatPercent, formatSeconds } from "./format.ts";
import { beatSpanLabels } from "./musical-grid.ts";

// Whether the page runs on macOS, which names the modifier keys ⌘ and ⌥: the page also runs in
// WebView2 on Windows, which names them Ctrl and Alt.
export const isMac =
  typeof navigator !== "undefined" && /Mac/u.test(navigator.platform);
const lanesMulti = String(maxLanes);
const lanesCompare = String(maxLanes - 1);
const reach = formatSeconds(matchReachSeconds);
const matchWindow = formatSeconds(matchWindowSeconds);
const margin = String(summingMarginDb);
const levelFloor = String(levelFloorDbfs).replace("-", "−");
const gate = String(analysisGateDbfs).replace("-", "−");
const positionDefault = formatPercent(settings.triggerPosition.defaultValue);
const positionLowest = formatPercent(settings.triggerPosition.minimum);
const positionHighest = formatPercent(settings.triggerPosition.maximum);
const positionCoarse = formatPercent(positionStep);
const positionFine = formatPercent(positionFineStep);
const shortestTriggerSpan = formatSeconds(settings.triggerSpan.minimum);
const longestTriggerSpan = formatSeconds(settings.triggerSpan.maximum);
const shortestFreeSpan = formatSeconds(settings.freeSpan.minimum);
const longestFreeSpan = formatSeconds(settings.freeSpan.maximum);
// The level lines' steps at Coarse, Medium and Fine, in dB.
const coarseLevelDb = String(gridLevelStepDb * gridCoarseLevelStride);
const mediumLevelDb = String(gridLevelStepDb);
const fineLevelDb = String(gridLevelStepDb / gridFineLevelSubdivisions);

// A topic is read in one order: what it is, its choices, what the hand does, then the limits.
export interface HelpChoice {
  readonly name: string; // as the control names it
  readonly text: string; // what it does, a line of a list
}
export interface HelpAction {
  readonly gesture: string; // such as "Double-click" or "⌘ + scroll"
  readonly effect: string; // what it does, a line of a list
}
export interface HelpText {
  readonly title: string;
  readonly lead: string; // one sentence: what this is
  readonly choices?: readonly HelpChoice[];
  readonly actions?: readonly HelpAction[];
  readonly notes?: readonly string[]; // limits and caveats, as sentences
}

// The lead is read first, to decide whether to read on: a short sentence. Past the topic's budget
// (title aside, choice names and gestures counted) a tip stops being quick to read; the longest
// texts are rewritten to fit, not cut.
export const leadWordBudget = 16;
export const topicWordBudget = 70;

// A topic's words in reading order, a part to a line.
export function helpWords(text: HelpText): string {
  return [
    text.lead,
    ...(text.choices ?? []).map(({ name, text }) => `${name} ${text}`),
    ...(text.actions ?? []).map(
      ({ gesture, effect }) => `${gesture} ${effect}`,
    ),
    ...(text.notes ?? []),
  ].join("\n");
}

// A setting's choices, named by its control's labels and in their order, so help and control
// never differ; a value added to the setting needs its text here before it compiles.
function choicesOf<Value extends string>(
  labels: Readonly<Record<Value, string>>,
  texts: Readonly<Record<Value, string>>,
): HelpChoice[] {
  return (Object.entries(labels) as [Value, string][]).map(([value, name]) => ({
    name,
    text: texts[value],
  }));
}

// The beat spans' range, from their labels, and a label's amount alone as it may be typed.
const [shortestBeatSpan] = beatSpanLabels;
const longestBeatSpan: string = beatSpanLabels.at(-1) ?? shortestBeatSpan;
const bareAmount = (label: string) => label.split(" ")[0] ?? label;

// The table for a platform: `mac` names the keys as macOS does, otherwise as Windows does.
export function helpTextsFor(mac: boolean) {
  const command = mac ? "⌘" : "Ctrl";
  const option = mac ? "⌥" : "Alt";
  return {
    help: {
      title: "Help",
      lead: "Turns help on or off: while on, resting on any control explains it.",
    },
    // Shown at once beside the switch when it turns help on.
    helpOn: {
      title: "Help is on",
      lead: "Rest the pointer on any control to see what it does.",
      actions: [{ gesture: "Click ?", effect: "turn help off" }],
    },
    about: {
      title: "About",
      lead: "Shows which version of the plugin this is, as your host lists it.",
      notes: ["Give it when you report a problem."],
    },
    contact: {
      title: "About us",
      lead: "Opens our About page, with how to contact us, in your default browser.",
    },
    displayMode: {
      title: "Single or Multi",
      lead: "What the screen shows: this track, or every track of its group.",
      choices: choicesOf(choiceLabels.displayMode, {
        single: "this track's channels",
        multi: `every track of this instance's group with this plugin, one lane each, showing the channel chosen in the Group popover, at most ${lanesMulti} (${lanesCompare} with Compare on, which adds their sum)`,
      }),
      notes: [
        "Keep all instances in the same plugin hosting mode, and put this plugin last on each track so lanes line up.",
      ],
    },
    compare: {
      title: "Compare",
      lead: "In Multi, sums the tracks you check and measures how each lines up with this one.",
      notes: [
        `In beta. The sum takes a lane, so Multi then shows at most ${lanesCompare} tracks.`,
        "It stays on or off as you leave Multi and come back, and is saved with the project.",
      ],
    },
    compareTrack: {
      title: "Compare",
      lead: "Check a track to add it to the sum and measure it against this track.",
      notes: [
        "This track is always compared; uncheck another to leave it out.",
      ],
    },
    previewFix: {
      title: "Preview fix",
      lead: "Redraws the sum as if every checked track were shifted and flipped as suggested.",
      notes: [
        "The summing gain shown is the one that would give.",
        "Apply it in your host, with track delay and a phase or polarity switch.",
      ],
    },
    sumGain: {
      title: "Summing gain",
      lead: "How loud the checked tracks are together, against unrelated signals of the same levels.",
      choices: [
        { name: `Within ±${margin} dB`, text: "harmless" },
        { name: "Above", text: "they reinforce" },
        {
          name: "Below",
          text: "they cancel; strongly, the sum's lane is shaded yellow if a checked track is related to yours",
        },
        {
          name: "—",
          text: "they never sound together, or nothing is measured yet",
        },
      ],
      notes: [
        `Measured over the visible part, only where two or more sound together above ${gate} dBFS.`,
      ],
    },
    match: {
      title: "Against this track",
      lead: "How this track lines up with yours.",
      choices: [
        {
          name: "Offset",
          text: `how late it is, within ±${reach}; negative is early`,
        },
        { name: "Ø", text: "its polarity is opposite" },
        { name: "Likeness", text: "+1 in step, 0 unrelated, −1 opposite" },
        {
          name: "Unrelated",
          text: "no shift makes them match clearly; Preview fix leaves it",
        },
        {
          name: "—",
          text: "they never sound together there, or not measured yet",
        },
      ],
      notes: [
        `Over the ${matchWindow} before the playhead (zoomed away, the newest shown), above ${gate} dBFS only.`,
      ],
    },
    group: {
      title: "Views",
      lead: "Which group's tracks Multi shows, and with Compare their sum.",
      notes: [
        "The field names the group, the lane channel and Σ, which is dimmed while Compare is off.",
        "Instances in the same group appear together; use groups to keep comparisons apart, such as drums in A and bass in B.",
        "Every instance belongs to a group, whatever its own mode.",
      ],
    },
    laneChannel: {
      title: "Channel",
      lead: "Which channel every lane of Multi shows: Left, Right, Mid or Side.",
      notes: [
        "The trigger, the Meter, snapping and Compare read it too.",
        "A mono track shows its one channel whatever the choice.",
        "Chosen while frozen, it redraws this track and leaves the others empty until the display resumes.",
      ],
    },
    channelViews: {
      title: "Views",
      lead: "Which channels this track shows, and which one the Meter reads.",
      notes: [
        "The letter after the dot is the channel the Meter reads.",
        "In Multi every track shows the channel chosen in the Group popover, so these apply in Single.",
      ],
    },
    status: {
      title: "Status",
      lead: "What the display is doing.",
      choices: [
        { name: "Trig’d", text: "locked to a trigger" },
        { name: "Waiting", text: "Normal or Single awaits a trigger" },
        { name: "Sync", text: "following the host's beats" },
        {
          name: "Filling",
          text: "a long sweep draws its first pass",
        },
        { name: "Partial", text: "frozen before it did" },
        { name: "Span limited", text: "shortened to what can be kept" },
        { name: "No tempo", text: "none from the host yet" },
        {
          name: "Invalid samples",
          text: "not numbers, or infinite",
        },
      ],
      notes: [
        "The Meter and Compare measure around them; their lanes are named in yellow.",
      ],
    },
    meter: {
      title: "Meter",
      lead: "When on, shows levels over the whole span of the channel chosen under Measure.",
      choices: [
        { name: "Peak", text: "the highest sample level, in dBFS" },
        {
          name: "RMS",
          text: "the average power, in dBFS; a full-scale sine reads −3 dB",
        },
        {
          name: "Crest",
          text: "how far peak stands above RMS: 3 dB for a sine, more if spikier",
        },
      ],
      notes: [
        `Below ${levelFloor} dBFS a level reads −∞, and Crest a dash.`,
        "In Multi it reads this track. Indicative, not a standards-compliant meter.",
      ],
    },
    screen: {
      title: "Screen",
      lead: "The signal over time.",
      actions: [
        {
          gesture: `${command} + scroll or pinch`,
          effect: "zoom around the pointer",
        },
        { gesture: "Drag across", effect: "zoom to a region" },
        { gesture: "Swipe sideways", effect: "pan" },
        { gesture: "Double-click", effect: "reset" },
      ],
      notes: [
        "In Trig and Free, frozen and not zoomed, swipe to look further back.",
      ],
    },
    triggerMarker: {
      title: "Trigger level",
      lead: "The level the signal must cross to trigger.",
      actions: [{ gesture: "Drag up or down", effect: "set it" }],
    },
    timeCursor: {
      title: "Time cursor",
      lead: "A time cursor, for measuring time.",
      actions: [
        {
          gesture: "Drag sideways",
          effect:
            "move it; it snaps to zero crossings and the highest and lowest points",
        },
        { gesture: `${option} + drag`, effect: "place it freely" },
      ],
    },
    levelCursor: {
      title: "Level cursor",
      lead: "A level cursor, for measuring levels.",
      actions: [
        {
          gesture: "Drag up or down",
          effect:
            "move it; it snaps to the highest and lowest levels and to zero",
        },
        { gesture: `${option} + drag`, effect: "place it freely" },
      ],
    },
    resizeGrip: {
      title: "Resize",
      lead: "Drag to resize the window.",
      notes: [
        "It stays wide enough for every control and, with stacked lanes, tall enough for every lane.",
      ],
    },
    triggerSpan: {
      title: "Time span",
      lead: `How much time the screen shows in Trig, from ${shortestTriggerSpan} to ${longestTriggerSpan}.`,
      actions: [
        {
          gesture: "Drag or scroll",
          effect: "step through 1-2-5 values, with Shift finer",
        },
        {
          gesture: "Double-click",
          effect: "type one, such as 20 ms; a number alone is in milliseconds",
        },
      ],
      notes: ["Longer spans are shown in Free."],
    },
    freeSpan: {
      title: "Time span",
      lead: `How much time the screen shows in Free, from ${shortestFreeSpan} to ${longestFreeSpan}.`,
      actions: [
        {
          gesture: "Drag or scroll",
          effect: "step through 1-2-5 values, with Shift finer",
        },
        {
          gesture: "Double-click",
          effect:
            "type one with its unit, such as 2 s; a number alone is in milliseconds",
        },
      ],
      notes: [
        "Shorter spans are shown in Trig, where the trigger holds the trace still.",
      ],
    },
    beatSpan: {
      title: "Span",
      lead: `How many bars the screen shows at the host tempo, from ${shortestBeatSpan} to ${longestBeatSpan}.`,
      actions: [
        { gesture: "Drag or scroll", effect: "change it" },
        {
          gesture: "Double-click",
          effect: `type it, a number alone included: ${bareAmount(longestBeatSpan)} for ${longestBeatSpan}, ${bareAmount(shortestBeatSpan)} for ${shortestBeatSpan}`,
        },
      ],
    },
    timeBase: {
      title: "Time base",
      lead: "How the screen follows time.",
      choices: choicesOf(choiceLabels.timeBase, {
        beats:
          "sweeps in time with the host, bar line to bar line, with a playhead",
        free: "a fixed amount of time ending at the newest audio; times count back from the right edge",
        trigger:
          "a fixed amount of time placed by the trigger, so a repeating signal stands still",
      }),
    },
    verticalScale: {
      title: "Vertical scale",
      lead: "The level at the top of the screen, from −60 to +6 dBFS.",
      actions: [
        {
          gesture: "Drag or scroll",
          effect: "step by 3 dB, with Shift by 1 dB",
        },
        { gesture: "Double-click", effect: "type a level" },
      ],
    },
    viewLeft: {
      title: "Left",
      lead: "Shows or hides the left channel.",
      notes: ["At least one view stays on."],
    },
    viewRight: {
      title: "Right",
      lead: "Shows or hides the right channel.",
      notes: ["At least one view stays on."],
    },
    viewMid: {
      title: "Mid",
      lead: "Shows or hides the mid signal, (L + R) / 2: what both channels share.",
      notes: ["At least one view stays on."],
    },
    viewSide: {
      title: "Side",
      lead: "Shows or hides the side signal, (L − R) / 2, which carries the stereo width.",
      notes: ["At least one view stays on."],
    },
    layout: {
      title: "Layout",
      lead: "How the views share the screen.",
      choices: choicesOf(choiceLabels.viewLayout, {
        overlaid: "all views on one screen",
        stacked: "each view in its own lane",
      }),
    },
    trigger: {
      title: "Trigger",
      lead: "Decides when the display draws, so a repeating signal stands still.",
      choices: choicesOf(choiceLabels.triggerMode, {
        auto: "draws on every trigger, and without one when none comes",
        normal: "draws only on a trigger; otherwise the last trace stays",
        single: "captures one trace and freezes it; Re-arm waits for the next",
      }),
      actions: [
        {
          gesture: "Click",
          effect: "set the mode, source, slope, level, hysteresis and hold-off",
        },
      ],
      notes: [
        "Shown in Trig only: Beats follows the host tempo, and Free the newest audio.",
      ],
    },
    modeAuto: {
      title: "Auto",
      lead: "Draws on every trigger, and keeps drawing without one when none arrives for a moment.",
      notes: ["The usual choice."],
    },
    modeNormal: {
      title: "Normal",
      lead: "Draws only when the signal triggers; otherwise the last trace stays.",
    },
    modeSingle: {
      title: "Single",
      lead: "Captures one triggered trace and freezes it.",
      notes: ["Re-arm waits for the next."],
    },
    rearm: {
      title: "Re-arm",
      lead: "In Single mode, waits for the next trigger again.",
    },
    triggerSource: {
      title: "Trigger source",
      lead: "Which signal the trigger watches, among the views drawn.",
      notes: [
        "While the one chosen is hidden, it watches the first view drawn.",
        "It changes only what the trigger reads: the Meter, the hover and snapping keep their own channel, chosen in Views, under Measure.",
        "Not used in Multi, which triggers on the channel chosen in the Group popover.",
      ],
    },
    triggerSlope: {
      title: "Slope",
      lead: "Which way the signal must cross the level to trigger.",
      choices: choicesOf(choiceLabels.triggerSlope, {
        rising: "crossing it going up",
        falling: "crossing it going down",
      }),
    },
    triggerLevel: {
      title: "Trigger level",
      lead: "The sample value the signal must cross, from −1 to +1.",
      actions: [
        { gesture: "Drag or scroll", effect: "change it" },
        { gesture: "Double-click", effect: "type it" },
      ],
      notes: ["Or drag the arrow at the screen's left edge."],
    },
    triggerHysteresis: {
      title: "Hysteresis",
      lead: "How far the signal must move away from the level before it can trigger again.",
      notes: ["Raise it when noise causes false triggers."],
    },
    triggerHoldOff: {
      title: "Hold-off",
      lead: "The least time between triggers.",
      actions: [
        {
          gesture: "Drag or scroll",
          effect: "step through 1-2-5 values, with Shift finer",
        },
        { gesture: "Double-click", effect: "type it" },
      ],
      notes: [
        "Use it to lock onto the start of a repeating pattern instead of a hit inside it.",
      ],
    },
    triggerPosition: {
      title: "Position",
      lead: "Where the trigger point sits; the dashed line marks it, and time counts from it.",
      actions: [
        {
          gesture: "Drag or scroll",
          effect: `step ${positionCoarse}, with Shift ${positionFine}`,
        },
        { gesture: "Double-click", effect: "type it" },
      ],
      notes: [
        `From ${positionLowest}, only what follows the trigger, to ${positionHighest}, only what led up to it; ${positionDefault} by default.`,
        "A low position waits longer on long spans, for audio after the trigger.",
        "Frozen, the trace follows, as far as the held audio allows.",
      ],
    },
    hoverReadout: {
      title: "Hover readout",
      lead: "When on, resting the pointer on a trace shows what is under it.",
      notes: [
        "It shows the position, the level, and the length, frequency and note of the cycle around it.",
        "A dotted line marks the column or point read, and a faint band the cycle measured.",
      ],
    },
    readoutSource: {
      title: "Measure",
      lead: "The channel the Meter reads: Left, Right, Mid or Side.",
      actions: [{ gesture: "Click", effect: "choose among the views drawn" }],
      notes: [
        "With the views overlaid, the hover and the cursors' snapping read it too; stacked, each lane reads its own trace.",
        "While the one chosen is hidden, the first view drawn is read.",
        "A mono track has only Left. Multi reads the channel chosen in the Group popover.",
      ],
    },
    grid: {
      title: "Grid",
      lead: "The screen's grid: what it draws, and how closely it divides time and level.",
      actions: [{ gesture: "Click", effect: "choose" }],
    },
    gridType: {
      title: "Grid type",
      lead: "What the grid draws.",
      choices: choicesOf(choiceLabels.gridType, {
        none: "each lane's zero line alone",
        ticks: "the time and level labels, without lines",
        grid: "the time and level lines and their labels",
      }),
    },
    gridTime: {
      title: "Time lines",
      lead: "How closely lines divide time.",
      choices: choicesOf(choiceLabels.gridDensity, {
        coarse: `${String(gridColumnsCoarse)} equal divisions in Trig and Free; in Beats, one step coarser than Medium`,
        medium: `${String(gridColumnsMedium)} in Trig and Free; in Beats, as the zoom gives (bars, beats, sixteenths)`,
        fine: `${String(gridColumnsFine)} in Trig and Free; in Beats, one step finer where its lines stay ${String(gridFineBeatLineGap)} pixels apart`,
      }),
      notes: [
        "Before the host reports a tempo, Beats shows the equal divisions.",
        "Labels follow it in Beats.",
      ],
    },
    gridLevel: {
      title: "Level lines",
      lead: "How closely lines divide each lane's level, down from its top.",
      choices: choicesOf(choiceLabels.gridDensity, {
        coarse: `every ${coarseLevelDb} dB`,
        medium: `every ${mediumLevelDb} dB`,
        fine: `every ${mediumLevelDb} dB in the brighter colour, with ${fineLevelDb} dB lines between them`,
      }),
      notes: [
        `With Ticks or Grid, Coarse labels every ${coarseLevelDb} dB, Medium and Fine every ${mediumLevelDb} dB.`,
        "A short lane shows fewer.",
      ],
    },
    beatDivision: {
      title: "Beat division",
      lead: "How Beats divides the beat.",
      choices: choicesOf(choiceLabels.beatDivision, {
        regular: "halves, quarters and finer",
        triplets:
          "thirds, sixths and finer, labelled as fractions of a beat such as +1/3",
      }),
      notes: ["Positions count as before."],
    },
    concertPitch: {
      title: "Concert pitch",
      lead: "The pitch of A4 that note names refer to, from 400 to 500 Hz.",
      actions: [
        {
          gesture: "Drag or scroll",
          effect: "change it, with Shift by 0.1 Hz",
        },
        { gesture: "Double-click", effect: "type it" },
      ],
    },
    freeze: {
      title: "Freeze",
      lead: "Holds the display so you can study it.",
      actions: [{ gesture: "Click again", effect: "go live" }],
      notes: [
        "Scale, views, zoom and cursors keep working, and in Trig and Free you can swipe to look back.",
      ],
    },
    autoFreeze: {
      title: "Auto-freeze",
      lead: "Freezes the display when the host stops, and goes live again when it plays.",
      notes: [
        "On by default.",
        "While stopped it stays frozen even if you play an instrument through the track; click Freeze to go live.",
        "Turning it off stops later freezes and keeps the current one.",
        "A freeze you make yourself is kept.",
      ],
    },
    timeCursors: {
      title: "Time cursors",
      lead: "Two vertical lines, A and B, for measuring time.",
      actions: [{ gesture: "Drag", effect: "move them" }],
      notes: [
        "The readout shows where each is: in Beats on the project's clock, in Trig from the trigger, in Free back from the newest audio.",
        "It also shows the time between them, its frequency and nearest note, the samples between them and, in Beats, the bars and beats.",
      ],
    },
    levelCursors: {
      title: "Level cursors",
      lead: "Two horizontal lines, 1 and 2, for measuring levels.",
      actions: [{ gesture: "Drag", effect: "move them" }],
      notes: ["The readout shows each level in dBFS and the difference in dB."],
    },
  } as const satisfies Record<string, HelpText>;
}

export const helpTexts = helpTextsFor(isMac);

export type HelpTopic = keyof typeof helpTexts;

export function isHelpTopic(name: string): name is HelpTopic {
  return Object.hasOwn(helpTexts, name);
}

// Which topic explains each of a setting's values, or why none lists them. Every setting the
// protocol makes a choice of one value is here: one added without an entry does not compile.
type EnumSetting = {
  [
    Name in keyof typeof settings
  ]: (typeof settings)[Name]["kind"] extends "enum" ? Name : never;
}[keyof typeof settings];

// `labels` names the control's labels where several settings share them (the grid's Time and
// Level).
export const choiceHelp: {
  readonly [Name in EnumSetting]:
    | { readonly topic: HelpTopic; readonly labels?: keyof typeof choiceLabels }
    | { readonly exempt: string };
} = {
  displayMode: { topic: "displayMode" },
  timeBase: { topic: "timeBase" },
  viewLayout: { topic: "layout" },
  triggerSlope: { topic: "triggerSlope" },
  triggerMode: { topic: "trigger" },
  group: { exempt: "letters A to D, which say nothing to explain one by one" },
  compare: { exempt: "an on/off switch, whose topic says what on does" },
  autoFreeze: { exempt: "an on/off switch, whose topic says what on does" },
  hoverReadout: { exempt: "an on/off switch, whose topic says what on does" },
  meter: { exempt: "an on/off switch, whose topic says what on does" },
  beatSpan: {
    exempt: "a range of twelve spans, named by its shortest and longest",
  },
  triggerSource: { exempt: "channel letters, as the views name them" },
  readoutSource: { exempt: "channel names, explained in the topic's lead" },
  laneChannel: { exempt: "channel names, explained in the topic's lead" },
  gridType: { topic: "gridType" },
  gridTime: { topic: "gridTime", labels: "gridDensity" },
  gridLevel: { topic: "gridLevel", labels: "gridDensity" },
  beatDivision: { topic: "beatDivision" },
};

// Each trigger mode's button explains its own mode.
export const triggerModeHelp: Readonly<
  Record<keyof typeof TriggerMode, HelpTopic>
> = {
  auto: "modeAuto",
  normal: "modeNormal",
  single: "modeSingle",
};
