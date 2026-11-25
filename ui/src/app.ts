// The oscilloscope UI: wires the controls, screen and readouts to the backend and
// the settings store. Controls show only engine-reported values and send requests.
import {
  ChannelView,
  CompareLane,
  TraceShape,
  ViewLayout,
} from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  maxShownTracks,
  minimumEditorWidth,
  readoutIntervalSeconds,
  screenBorder,
  screenMargin,
  screenRadius,
  statusBarHeight,
  summingMarginDb,
  toolbarHeight,
} from "../../protocol/src/limits.ts";
import type {
  ErrorMessage,
  LaneLabel,
  SettingValue,
  TraceViewMessage,
  TrackEntry,
} from "../../protocol/src/messages.ts";
import {
  settingNames,
  settingOf,
  valueNames,
} from "../../protocol/src/setting-values.ts";
import type {
  NumberSetting,
  SettingName,
} from "../../protocol/src/settings.ts";
import type { Backend } from "./bridge/backend.ts";
import { CheckList } from "./components/check-list.ts";
import { cutName, fitCutNames } from "./components/cut-name.ts";
import { DragField } from "./components/drag-field.ts";
import { followDrag } from "./components/follow-drag.ts";
import { numberFields, type NumberField } from "./view/fields.ts";
import { HelpTips } from "./components/help-tips.ts";
import { attachResizeGrip } from "./components/resize-grip.ts";
import { Segmented } from "./components/segmented.ts";
import { StackedSummary } from "./components/stacked-summary.ts";
import { streamGap, wheelUnit } from "./components/wheel-steps.ts";
import { RenderLoop } from "./render/render-loop.ts";
import {
  fullScale,
  traceLayering,
  screenLaneAt,
  screenLaneOf,
  screenLanes,
  traceInScreenLane,
  traceLanes,
  type Lane,
} from "./render/trace-geometry.ts";
import {
  TraceMemoryError,
  TraceRenderer,
  type Rgba,
  type TraceStyle,
  type TraceView,
} from "./render/trace-renderer.ts";
import type { SettingsStore } from "./store/settings-store.ts";
import { amplitudeLabels, timeLabels } from "./view/axis-labels.ts";
import {
  formatCorrelation,
  formatGain,
  formatOffset,
  parseQuantity,
} from "./view/format.ts";
import {
  cursorReadout,
  initialCursors,
  initialLevels,
  levelReadout,
  meterReadout,
  meterValuesOf,
  type MeterValues,
  type ReadoutRow,
  measure,
  type CursorPair,
  type LevelPair,
} from "./view/cursors.ts";
import { choiceLabels } from "./view/choice-labels.ts";
import { drawGrid, gridColumns } from "./view/grid.ts";
import {
  drawsLabels,
  gridChoice,
  gridSettingName,
  isGridSetting,
  type GridChoice,
  type GridSettingKind,
  type TimeBaseName,
} from "./view/grid-settings.ts";
import {
  hoverReadout,
  timeAt,
  type HoverContext,
  type HoverTrace,
} from "./view/hover.ts";
import {
  cssColour,
  fallbackPalette,
  laneName,
  laneStyles,
  sumLaneColour,
  type LaneStyle,
} from "./view/lanes.ts";
import { isMac, type HelpTopic, triggerModeHelp } from "./view/help.ts";
import { brand } from "@brand/brand.ts";
import {
  extremePositions,
  levelTargets,
  snap,
  zeroCrossings,
} from "./view/snap.ts";
import {
  beatSpanLabels,
  musicalGrid,
  parseBeatSpan,
} from "./view/musical-grid.ts";
import {
  laneHoldsInvalid,
  shownChipCount,
  statusChips,
  triggerModeNames,
} from "./view/status.ts";
import {
  fullView,
  isZoomed,
  minimumWidth,
  panBy,
  toFrame,
  toScreen,
  zoomAround,
  zoomFactor,
  zoomTo,
  type ViewWindow,
} from "./view/zoom.ts";
import {
  clampPan,
  markerLane,
  meterSource,
  readoutView,
  viewNames,
  viewOrder,
  viewToggleState,
  type ViewName,
} from "./view/screen-state.ts";

// What the page says when the traces cannot be drawn (DESIGN.md §8): no WebGL2, or the graphics
// memory for the traces refused, which also comes when the window grows; and, while the macOS
// editor draws them, its native drawing stopped for good.
export const graphicsMessages = {
  noWebGL2:
    "This system gives the editor no WebGL 2 graphics to draw the traces with. Updating the graphics driver may help. Audio still passes through unchanged.",
  memory:
    "The system's graphics could not give the editor the memory to draw the traces. Reopening the editor may help. Audio still passes through unchanged.",
  // macOS native drawing stopped for good this session (a removed GPU, persistent errors).
  lost: "The system's graphics stopped drawing the traces. Close and reopen the editor to draw them again. Audio still passes through unchanged.",
};

const viewHelp: Record<ViewName, HelpTopic> = {
  left: "viewLeft",
  right: "viewRight",
  mid: "viewMid",
  side: "viewSide",
};
const viewLabels: Record<ViewName, string> = {
  left: "L",
  right: "R",
  mid: "M",
  side: "S",
};
const viewColorTokens: Record<ViewName, string> = {
  left: "--sl-left",
  right: "--sl-right",
  mid: "--sl-mid",
  side: "--sl-side",
};
// How fast ⌘ + scroll and pinch zoom, per wheel delta unit.
const wheelZoomRate = 0.01;
// The most one wheel event zooms by, in wheel delta units: about 1.3×, which is also what a lone
// mouse-wheel notch zooms by on every system. A trackpad's events are a few units each.
const maxWheelZoomDelta = 25;
// The Compare view's sum lane: an id no track has, and a neutral colour.
const sumLaneId = -100;
// A Single view's label, below any lane's id: this less the view's number.
const viewLabelId = -200;

// A lane label: a track's lane, or a view of this track, with its swatch's colour as CSS.
type LabelStyle = LaneStyle & { readonly swatch: string };
// Cursors snap to a target within this distance (CSS px) unless Option is held.
const snapRadius = 10;
// A drag shorter than this (CSS px) is a click, not a zoom selection.
const minimumSelectionPixels = 4;

// WebKit's trackpad pinch event; not in the standard DOM types.
interface GestureEvent extends UIEvent {
  readonly scale: number;
  readonly clientX: number;
}

// Room taken by the amplitude labels at the left edge (CSS px), which the lane labels keep clear of.
const amplitudeLabelWidth = 56;
const readoutInterval = readoutIntervalSeconds * 1000; // ms, as the engine measures them.
// Toolbar fields that open a popover: the field's container, its button and the popover.
const popovers = [
  ["mode-group", "mode-details", "mode-popover"],
  ["time-base-group", "time-base-details", "time-base-popover"],
  ["layout-group", "layout-details", "layout-popover"],
  ["views-group", "views-details", "views-popover"],
  ["trigger-group", "trigger-details", "trigger-popover"],
  ["about-group", "about-details", "about-popover"],
  ["grid-group", "grid-details", "grid-popover"],
] as const;
// The settings chosen in a toolbar field's popover, whose button names the value.
type ChoiceSetting = "displayMode" | "timeBase" | "viewLayout";
interface ChoiceSummary {
  readonly summary: StackedSummary;
  readonly details: HTMLElement;
  readonly word: string;
  readonly labels: Readonly<Record<string, string>>;
}
// The status bar's popovers, which CSS places above their buttons; the others open under their
// field.
const placedByStyle = new Set<string>(["about-popover", "grid-popover"]);
// The hover readout keeps this far (CSS px) from the pointer.
const hoverOffset = 14;

function element(id: string): HTMLElement {
  const found = document.getElementById(id);
  if (found === null) {
    throw new Error(`Missing element #${id}`);
  }
  return found;
}

function token(name: string): string {
  return getComputedStyle(document.documentElement)
    .getPropertyValue(name)
    .trim();
}

function rgba(color: string): Rgba {
  const hex = color.replace("#", "");
  const value = (offset: number) =>
    parseInt(hex.slice(offset, offset + 2), 16) / 255;
  return [value(0), value(2), value(4), 1];
}

// A lane's name as it is shown wherever a lane is named: the name alone, or, for one of several
// instances on a track, the name with its number small and lowered after it (styled, as the
// embedded fonts have no subscript digits) and, for a screen reader, the words that say what the
// number is. A name too long for its place is cut in the middle (cut-name.ts), whole to a screen
// reader; `live` where it stays on screen, so it follows its width and rolls under the pointer.
function laneNameNodes(
  lane: {
    readonly name: string;
    readonly instance: number;
  },
  live: boolean,
  trigger?: HTMLElement,
): HTMLElement[] {
  const { name: text, number, spoken } = laneName(lane);
  const name = cutName(
    text,
    trigger === undefined ? { live } : { live, trigger },
  );
  if (number === undefined || spoken === undefined) {
    return [name];
  }
  const shown = document.createElement("span");
  shown.className = "instance";
  shown.setAttribute("aria-hidden", "true");
  shown.textContent = number;
  const said = document.createElement("span");
  said.className = "visually-hidden";
  said.textContent = spoken;
  const whole = document.createElement("span");
  whole.className = "lane-name";
  whole.append(name, shown, said);
  return [whole];
}

function range(name: SettingName): NumberSetting {
  return settingOf(name) as NumberSetting;
}

function clampTo(name: SettingName, value: number): number {
  const { minimum, maximum } = range(name);
  return Math.min(maximum, Math.max(minimum, value));
}

// A typed value that must be a whole number of its bare unit (`scale`: 0.01 for percent): that
// value, divided rather than multiplied back so 57% is 0.57 exactly; undefined if not whole.
function wholeOf(value: number | undefined, scale = 1): number | undefined {
  if (value === undefined) {
    return undefined;
  }
  const units = value / scale;
  const whole = Math.round(units);
  return Math.abs(units - whole) < 1e-9
    ? whole / Math.round(1 / scale)
    : undefined;
}

export interface DebugState {
  frameCount: number;
  lastFrame?: Frame;
}

export class OscilloscopeApp {
  private readonly backend: Backend;
  private readonly store: SettingsStore;
  private readonly debug: DebugState;
  private readonly screen = element("screen");
  private readonly gridCanvas = element("grid") as HTMLCanvasElement;
  private readonly traceCanvas = element("trace") as HTMLCanvasElement;
  // Undefined while the web view gives the page no WebGL2 or has lost its context: the traces
  // are not drawn, and the message says why.
  private renderer: TraceRenderer | undefined;
  // Makes the renderer, once the first snapshot has said whether the editor draws the traces
  // instead; undefined once chosen.
  private makeRenderer: (() => void) | undefined;
  private graphicsMessage = false; // Whether the message shown is about the graphics.
  // Whether an engine error is shown. Until a snapshot clears it, it outranks the graphics message:
  // it says why the screen is empty and what to do, and the engine does not send it again.
  private engineError = false;
  // macOS: the editor draws the traces itself (snapshot.nativeTraces), and the page sends it what
  // only the page knows, at most once per animation frame and only when it changed.
  private nativeTraces = false;
  // The diagnostic switch: every picture of the traces carries the frame barcode, from the
  // editor's snapshot (SINELESS_FRAME_BARCODE=1) or, in the browser preview, ?barcode.
  private frameBarcode = new URLSearchParams(window.location.search).has(
    "barcode",
  );
  private traceViewQueued = false;
  private lastTraceView = "";
  // The screen's size and the pixel ratio the canvases were last sized for.
  private sized: { width: number; height: number; ratio: number } | undefined;
  // Native drawing stopped for good this session: never cleared, so a snapshot read before the
  // editor's message, or a reloaded page, can only show it.
  private nativeGraphicsLost = false;
  private traceStyle: TraceStyle | undefined;
  private readonly loop: RenderLoop;
  private lastFrame: Frame | undefined;
  // The span in seconds the time labels were last placed for.
  private labelledSpan = 0;
  // The trigger point the time labels were last placed for.
  private labelledZero = Number.NaN;
  private lastFrameTime = 0;
  // Readouts refresh on a fixed schedule of one per readoutInterval, as frames are drawn (see
  // RenderLoop), whether frames or the idle timer prompt them.
  private nextReadoutTime = -Infinity;
  private shownChips = "";
  private lastZoomWheel = -Infinity; // When the last zooming wheel event came (ms).
  private panSeconds = 0;
  private inputChannels = 2; // The track's, from the snapshot.
  private waiting = false;
  private readonly fields = new Map<SettingName, DragField>();
  private readonly segmented = new Map<SettingName, Segmented>();
  // The fields whose button names the value chosen in their popover.
  private readonly choiceSummaries = new Map<ChoiceSetting, ChoiceSummary>();
  private triggerSummary: StackedSummary | undefined;
  // What the Meter shows: the frame's readouts as last taken on the readout schedule, so a
  // cursor drag between two refreshes redraws the corner without refreshing them, and its Source,
  // naming the channel the levels were measured on, from the same frame.
  private meterValues: { values: MeterValues; source: string } = {
    values: {
      peak: Number.NaN,
      truePeak: Number.NaN,
      rms: Number.NaN,
      dcOffset: Number.NaN,
      loudness: Number.NaN,
      correlation: Number.NaN,
    },
    source: meterSource(undefined),
  };
  // The readouts' channel, the Views popover's Measure row: lit as the frame measured it, not as the
  // setting asks.
  private readoutChoices: Segmented | undefined;
  // What the Measure row last showed: the view lit and the views enabled.
  private readoutShown: string | undefined;
  // Freeze's label, "Freeze" or "Frozen", both in one place so freezing resizes nothing (AD-26).
  private freezeLabel: StackedSummary | undefined;
  // The grid's popover: a row per choice, each reading and writing its one setting, in force in
  // every time base, and the field naming the type drawn.
  private readonly gridRows = new Map<
    GridSettingKind | "Division",
    Segmented
  >();
  private gridSummary: StackedSummary | undefined;
  // The trigger's source: lit as the frame's trigger reads, not as the setting asks.
  private sourceChoices: Segmented | undefined;
  // The view the trigger level marker was last placed for, in lane and colour.
  private markerView: ViewName | undefined;
  // What the Source row last showed: the view lit and the views enabled.
  private sourceShown = "";
  private viewList: CheckList | undefined;
  private beatSpanField: DragField | undefined;
  private beatSpanNames: string[] = [];
  private cycleKey = "";
  private view: ViewWindow = fullView;
  // Time and level cursors, while shown; temporary like the view.
  private timeCursors: CursorPair | undefined;
  private shownFrozen: boolean | undefined;
  // The pointer over the traces, as fractions of the screen, for the hover readout.
  private hover: { x: number; y: number } | undefined;
  // One overlaid trace highlighted (FR-16.4), by its lane label's id, so it names the same trace in
  // every frame: the label under the pointer, else the trace the hover readout reads; never saved.
  private labelUnderPointer: number | undefined;
  private readoutTrace: number | undefined;
  private highlight: number | undefined;
  private levelCursors: LevelPair | undefined;
  // The Compare view: the fix preview, and the lane labels' readouts of each compared track (by
  // lane index) and of the sum, updated in place.
  private previewFix = false;
  // Compare's switch in the Views popover (Off, or the lane it adds).
  private compareChoice: Segmented | undefined;
  private matchNodes: (HTMLElement | undefined)[] = [];
  // Each lane's name, marked while the lane shows samples that are not finite.
  private laneNameNodes: HTMLElement[] = [];
  private sumNode: HTMLElement | undefined;
  private laneLabelsKey = "";
  // Multi-track lanes by lane set, as the lanes messages describe them, and as they are named
  // and coloured; a few are kept, so a frame drawn just before or after a change finds its own.
  private readonly laneLabelSets = new Map<number, readonly LaneLabel[]>();
  private readonly laneSets = new Map<number, LaneStyle[]>();
  private latestLaneSet = 0;
  // Every Sineless instance in the host, as the tracks message lists them, and which Multi shows:
  // the Views popover's Channels list in Multi, and its count in the Views field.
  private tracks: readonly TrackEntry[] = [];
  private trackList: CheckList | undefined;
  private drawnLaneCount = 0;
  private drawnViewSet = 0;
  private drawnLaneSet = 0;
  // What the last frame's Compare lane was (the sum or the difference): its label follows it.
  private drawnCompareLane: CompareLane | undefined;
  private viewSummary: string | undefined;
  // The Views field's names for screen readers, Single's and Multi's: the button carries the
  // current mode's.
  private viewsNames = { single: "", multi: "" };
  // The mode the Views popover's rows last followed, so that a mode change closes it and a
  // setting resent unchanged, or Compare chosen in it, does not.
  private viewsMultiShown: boolean | undefined;

  constructor(backend: Backend, store: SettingsStore, debug: DebugState) {
    this.backend = backend;
    this.store = store;
    this.debug = debug;
    // Sizes the stylesheet shares with the engine: the minimum width, and the frame around the
    // screen, which the macOS editor draws its native traces inside.
    for (const [name, pixels] of [
      ["--minimum-width", minimumEditorWidth],
      ["--toolbar-height", toolbarHeight],
      ["--status-bar-height", statusBarHeight],
      ["--screen-margin", screenMargin],
      ["--screen-border", screenBorder],
      ["--screen-radius", screenRadius],
    ] as const) {
      document.documentElement.style.setProperty(name, `${String(pixels)}px`);
    }
    // Resolved on first use: in development the stylesheet may apply after start-up.
    let colors: Map<ChannelView, Rgba> | undefined;
    const resolveColors = (): ReadonlyMap<ChannelView, Rgba> => {
      if (colors === undefined && token("--sl-left") !== "") {
        colors = new Map(
          viewOrder.map((name) => [
            ChannelView[name],
            rgba(token(viewColorTokens[name])),
          ]),
        );
      }
      return colors ?? new Map();
    };
    const style: TraceStyle = {
      colors: resolveColors,
      laneColors: () =>
        this.lanesOf(this.lastFrame).map((lane) =>
          rgba(cssColour(lane.colour)),
        ),
      ownLane: () =>
        Math.max(
          0,
          this.lanesOf(this.lastFrame).findIndex((lane) => lane.own),
        ),
      differenceColors: () => ({
        gained: rgba(token("--sl-gained")),
        lost: rgba(token("--sl-lost")),
      }),
      sweepColors: () => {
        return { cursor: rgba(token("--sl-ink")) };
      },
      screenColor: () => rgba(token("--sl-screen")),
      frameBarcode: () => this.frameBarcode,
      // Cancelling is a problem in the audio: the problem colour, faint behind the sum.
      cancellationColor: () => {
        const [r, g, b] = rgba(token("--sl-warn"));
        return [r, g, b, 0.16];
      },
      // Read at each draw: the ratio changes when the window moves to another display.
      get lineWidthPixels() {
        return 1.5 * window.devicePixelRatio;
      },
    };
    this.traceStyle = style;
    const traceView = (): TraceView => ({
      verticalScaleDb: store.number("verticalScale"),
      layout: this.layout(),
      traceShape: this.traceShape(),
      dim: this.waiting,
      view: this.view,
      ...this.highlightIndex(),
    });
    // In native mode the page creates no WebGL context: the editor draws the traces under it.
    this.makeRenderer = () => {
      this.renderer = this.createRenderer(style, traceView);
    };
    // A lost context comes back only if the page says it will rebuild what it draws with (the
    // WebGL specification, "The Context Lost Event"); until then nothing is drawn.
    this.traceCanvas.addEventListener("webglcontextlost", (event) => {
      event.preventDefault();
      this.renderer = undefined;
      this.showGraphicsMessage(
        "The system's graphics stopped drawing the traces. They return when it recovers.",
      );
    });
    this.traceCanvas.addEventListener("webglcontextrestored", () => {
      this.renderer = this.createRenderer(style, traceView);
      if (this.renderer !== undefined && this.graphicsMessage) {
        this.graphicsMessage = false;
        element("message").hidden = true;
      }
      // The restored canvas starts empty, and while frozen no frame may come to fill it.
      this.loop.invalidate();
    });
    this.loop = new RenderLoop(
      (callback) => requestAnimationFrame(callback),
      {
        draw: (frame, tickTime) => {
          try {
            this.renderer?.draw(frame, tickTime);
          } catch (error) {
            if (!(error instanceof TraceMemoryError)) {
              throw error;
            }
            // After a resize, the graphics memory for the larger traces was refused.
            this.renderer = undefined;
            this.showGraphicsMessage(graphicsMessages.memory);
          }
          this.showSweep(frame);
        },
      },
      () => {
        backend.requestFrame();
      },
    );
    this.buildControls();
    this.listenToScreen();
    this.listenToBackend();
    store.subscribe((name, value) => {
      this.showSetting(name, value);
      this.queueTraceView();
    });
    for (const name of settingNames) {
      this.showSetting(name, store.get(name));
    }
    window.addEventListener("resize", () => {
      this.resize();
    });
    // The screen can change size with no window resize: at start-up its layout settles after the
    // first measurement, with the window already at its size. Seen after the frame's draw, so the
    // traces are drawn again at once: resizing the canvases cleared them. A window resize sizes
    // them earlier in the same rendering update, and this then finds nothing to do.
    new ResizeObserver(() => {
      const sized = this.sized;
      if (
        sized?.width === this.screen.clientWidth &&
        sized.height === this.screen.clientHeight &&
        sized.ratio === window.devicePixelRatio
      ) {
        return;
      }
      this.sizeScreen();
      this.loop.redrawNow();
    }).observe(this.screen);
    this.watchPixelRatio();
    window.setInterval(() => {
      this.statusDue(); // Without frames too: the chips say when none arrive.
    }, readoutInterval);
    this.resize();
  }

  start(): void {
    this.backend.hello(this.screen.clientWidth, window.devicePixelRatio);
    this.backend.requestFrame();
  }

  private layout(): ViewLayout {
    return this.store.get("viewLayout") === "stacked"
      ? ViewLayout.stacked
      : ViewLayout.overlaid;
  }

  private traceShape(): TraceShape {
    return this.store.get("traceShape") === "filled"
      ? TraceShape.filled
      : TraceShape.line;
  }

  // The views asked for, as Single's Channels list ticks them.
  private shownViews(): ViewName[] {
    const value = this.store.get("channelViews");
    const names = Array.isArray(value) ? value : [];
    return viewOrder.filter((name) => names.includes(name));
  }

  // The views the engine draws: those in the latest frame (a mono track's L alone, whatever was
  // asked for), or those asked for until a frame says.
  private drawnViews(): ViewName[] {
    const views = this.lastFrame?.views;
    return views !== undefined && views.size > 0
      ? viewOrder.filter((name) => views.has(ChannelView[name]))
      : this.shownViews();
  }

  private request(name: SettingName, value: SettingValue): void {
    this.backend.setSetting(name, value);
  }

  // ----- Controls -----

  private numberField(name: SettingName, spec: NumberField): void {
    const field = new DragField({
      label: spec.label,
      format: spec.format,
      parse: (text) => {
        const value = parseQuantity(text, spec.quantity, spec.bareScale);
        return spec.whole === true ? wholeOf(value, spec.bareScale) : value;
      },
      step: (value, delta, fine) =>
        clampTo(name, spec.step(value, delta, fine)),
      onRequest: (value) => {
        this.request(name, clampTo(name, value));
      },
    });
    element(spec.host).append(field.element);
    this.attachToPill(field, spec.host);
    this.fields.set(name, field);
  }

  // A field in a bar's pill (Span, Scale) answers anywhere on the pill, its icon or label
  // included, which looks like one control; the popovers' fields have no pill.
  private attachToPill(field: DragField, host: string): void {
    const pill = element(host).closest<HTMLElement>(".icon-field");
    if (pill !== null) {
      field.attach(pill);
    }
  }

  private buildControls(): void {
    // Tempo sync: the beat span steps through its named values.
    const beatSpanNames = valueNames("BeatSpan");
    const beatSpanField = new DragField({
      label: "Beat span",
      format: (index) => beatSpanLabels[index] ?? "",
      parse: parseBeatSpan,
      step: (index, delta) =>
        Math.min(
          beatSpanNames.length - 1,
          Math.max(0, Math.round(index) + delta),
        ),
      onRequest: (index) => {
        const name = beatSpanNames[Math.round(index)];
        if (name !== undefined) {
          this.request("beatSpan", name);
        }
      },
    });
    element("beat-span-field").append(beatSpanField.element);
    this.attachToPill(beatSpanField, "beat-span-field");
    this.beatSpanField = beatSpanField;
    this.beatSpanNames = beatSpanNames;

    for (const [name, field] of Object.entries(numberFields)) {
      this.numberField(name as SettingName, field);
    }

    // Single's Channels: the four views of this track, those ticked drawn. One stays ticked; a
    // mono track has Left alone, and None and All change nothing there.
    this.viewList = new CheckList(element("channels"), {
      labelledBy: "channels-label",
      onToggle: (id, checked) => {
        const shown = new Set(this.shownViews());
        if (checked) {
          shown.add(id as ViewName);
        } else {
          shown.delete(id as ViewName);
        }
        this.showViews(viewOrder.filter((view) => shown.has(view)));
      },
      onNone: () => {
        this.showViews(viewOrder.slice(0, 1));
      },
      onAll: () => {
        this.showViews(viewOrder);
      },
    });

    const segment = (
      name: SettingName,
      host: string,
      choices: [value: string, label: string, help?: HelpTopic | undefined][],
    ) => {
      this.segmented.set(
        name,
        new Segmented(
          element(host),
          choices.map(([value, label, help]) => ({
            value,
            label,
            ...(help === undefined ? {} : { help }),
          })),
          (value) => {
            this.request(name, value);
          },
        ),
      );
    };
    // Display mode, time base and layout: a field naming the value, its popover the choices.
    const choiceField = (
      name: ChoiceSetting,
      id: string,
      host: string,
      word: string,
    ) => {
      const labels = choiceLabels[name];
      segment(name, host, Object.entries(labels));
      this.choiceSummaries.set(name, {
        summary: new StackedSummary(element(`${id}-summary`), labels),
        details: element(`${id}-details`),
        word,
        labels,
      });
    };
    choiceField("displayMode", "mode", "display-mode", "Mode");
    // Multi's Channels: every Sineless instance in the host, those ticked shown as lanes. The
    // list shows what the engine says is shown, and asks it for any change.
    this.trackList = new CheckList(element("tracks"), {
      labelledBy: "tracks-label",
      onToggle: (id, checked) => {
        const asked = Number(id);
        this.showTracks(
          this.tracks
            .filter((track) =>
              track.id === asked ? checked : track.shown && !track.own,
            )
            .map((track) => track.id),
        );
      },
      onNone: () => {
        this.showTracks([]);
      },
      onAll: () => {
        this.showTracks(
          this.tracks.filter((track) => !track.own).map((track) => track.id),
        );
      },
    });
    // One channel for every lane: single choice, unlike Single's views.
    segment(
      "laneChannel",
      "lane-channel",
      viewOrder.map((name) => [name, viewLabels[name]]),
    );
    // Compare's switch chooses the lane it adds too: one control over two saved settings, so the
    // lane last chosen is kept while Compare is off. The lane first, so that the first Compare
    // frame already shows it.
    this.compareChoice = new Segmented(
      element("compare"),
      [
        { value: "off", label: "Off" },
        ...Object.entries(choiceLabels.compareLane).map(([value, label]) => ({
          value,
          label,
        })),
      ],
      (value) => {
        if (value !== "off") {
          this.request("compareLane", value);
        }
        this.request("compare", value === "off" ? "off" : "on");
      },
    );
    segment(
      "traceShape",
      "trace-shape-choice",
      Object.entries(choiceLabels.traceShape),
    );
    segment("laneLabels", "lane-labels-choice", [
      ["off", "Off"],
      ["on", "On"],
    ]);
    choiceField("timeBase", "time-base", "time-base", "Time");
    choiceField("viewLayout", "layout", "layout", "Layout");
    segment(
      "triggerMode",
      "mode",
      Object.entries(choiceLabels.triggerMode).map(([mode, label]) => [
        mode,
        label,
        triggerModeHelp[mode as keyof typeof triggerModeHelp],
      ]),
    );
    this.sourceChoices = new Segmented(
      element("source"),
      viewOrder.map((name) => ({ value: name, label: viewLabels[name] })),
      (value) => {
        this.request("triggerSource", value);
      },
    );
    segment("triggerSlope", "slope", Object.entries(choiceLabels.triggerSlope));
    this.readoutChoices = new Segmented(
      element("readout-source"),
      viewOrder.map((name) => ({ value: name, label: viewLabels[name] })),
      (value) => {
        this.request("readoutSource", value);
      },
    );

    const gridRow = (
      kind: GridSettingKind | "Division",
      host: string,
      labels: Readonly<Record<string, string>>,
    ) => {
      this.gridRows.set(
        kind,
        new Segmented(
          element(host),
          Object.entries(labels).map(([value, label]) => ({ value, label })),
          (value) => {
            this.request(
              kind === "Division" ? "beatDivision" : gridSettingName(kind),
              value,
            );
          },
        ),
      );
    };
    gridRow("Type", "grid-type", choiceLabels.gridType);
    gridRow("Density", "grid-density", choiceLabels.gridDensity);
    gridRow("Division", "beat-division", choiceLabels.beatDivision);
    this.gridSummary = new StackedSummary(
      element("grid-summary"),
      choiceLabels.gridType,
    );
    this.freezeLabel = new StackedSummary(element("freeze-label"), {
      live: "Freeze",
      frozen: "Frozen",
    });
    this.freezeLabel.show("live");

    element("brand-mark").innerHTML = brand.mark;
    element("wordmark").textContent = brand.name;
    document.title = brand.name;
    element("about-name").textContent = brand.name;
    element("about-details").setAttribute("aria-label", `About ${brand.name}`);
    element("contact").textContent = brand.vendor;

    // A field's button opens its popover, and closes any other; a press elsewhere closes it.
    for (const [group, button, id] of popovers) {
      const details = element(button);
      const popover = element(id);
      details.addEventListener("click", () => {
        if (details.getAttribute("aria-disabled") === "true") {
          return;
        }
        const open = popover.hidden;
        for (const [, otherButton, otherId] of popovers) {
          if (otherId !== id) {
            this.closePopover(otherButton, otherId);
          }
        }
        popover.hidden = !open;
        // Names in it are cut to the room they have, which they have only on show.
        fitCutNames(popover);
        details.setAttribute("aria-expanded", String(open));
        this.placePopover(id, group);
      });
      document.addEventListener("pointerdown", (event) => {
        const target = event.target as Node;
        if (
          !popover.hidden &&
          !popover.contains(target) &&
          !details.contains(target)
        ) {
          popover.hidden = true;
          details.setAttribute("aria-expanded", "false");
        }
      });
    }

    element("contact").addEventListener("click", () => {
      this.backend.contact();
      this.closePopover("about-details", "about-popover");
    });

    element("freeze").addEventListener("click", () => {
      const frozen = this.lastFrame?.frozen !== true;
      this.panSeconds = 0;
      this.backend.freeze(frozen);
    });
    attachResizeGrip(element("resize-grip"), (width, height) => {
      this.backend.resize(width, height);
    });
    const help = new HelpTips(element("help-tip"));
    element("help").addEventListener("click", () => {
      const on = element("help").getAttribute("aria-pressed") !== "true";
      element("help").setAttribute("aria-pressed", String(on));
      help.setEnabled(on, element("help"));
    });
    element("time-cursors").addEventListener("click", () => {
      this.timeCursors =
        this.timeCursors === undefined ? initialCursors(this.view) : undefined;
      element("time-cursors").setAttribute(
        "aria-pressed",
        String(this.timeCursors !== undefined),
      );
      this.placeCursors();
    });
    element("level-cursors").addEventListener("click", () => {
      this.levelCursors =
        this.levelCursors === undefined
          ? initialLevels(fullScale(this.store.number("verticalScale")))
          : undefined;
      element("level-cursors").setAttribute(
        "aria-pressed",
        String(this.levelCursors !== undefined),
      );
      this.placeCursors();
    });
    element("hover-toggle").addEventListener("click", () => {
      this.request(
        "hoverReadout",
        this.store.get("hoverReadout") === "on" ? "off" : "on",
      );
    });
    element("meter-toggle").addEventListener("click", () => {
      this.request("meter", this.store.get("meter") === "on" ? "off" : "on");
    });
    element("auto-freeze").addEventListener("click", () => {
      this.request(
        "autoFreeze",
        this.store.get("autoFreeze") === "on" ? "off" : "on",
      );
    });
    element("arm").addEventListener("click", () => {
      this.panSeconds = 0;
      this.backend.arm();
      this.backend.freeze(false);
    });
  }

  // Only the controls that apply are shown. Multi shows each track's lane channel: the channel views
  // and the trigger source do not apply, and the track list (with its lane channel and Compare) does
  // only there.
  private updateMultiControls(): void {
    const multi = this.multiMode();
    element("source-row").hidden = multi;
    // One Views field: its popover's rows are the mode's, A4 in both; its button shows the mode's
    // summary, the other's keeping its room out of sight, and the mode's help. The rows are set
    // before the popover can open, as it is placed by its width when it opens.
    if (multi !== this.viewsMultiShown) {
      this.viewsMultiShown = multi;
      this.closePopover("views-details", "views-popover");
      for (const row of element("views-popover").querySelectorAll<HTMLElement>(
        "[data-mode]",
      )) {
        row.hidden = (row.dataset["mode"] === "multi") !== multi;
      }
      element("views-single").classList.toggle("on", !multi);
      element("views-multi").classList.toggle("on", multi);
      element("views-group").dataset["help"] = multi
        ? "tracks"
        : "channelViews";
      this.nameViews();
    }
    this.updateViewList();
    this.updateReadoutChoices();
    this.updateSourceChoices();
    this.updateStatus();
  }

  private closePopover(button: string, id: string): void {
    element(id).hidden = true;
    element(button).setAttribute("aria-expanded", "false");
  }

  // The channel the Meter, snapping and the overlaid hover read (DESIGN.md 1.2.1), as the frame on
  // screen names it, whatever the settings now say until the next frame comes: in Single only, once
  // a Single frame has come. Multi reads this track's lane channel, chosen in the Views popover, and
  // its frames carry a lane set, a frame with no tempo included.
  private readChannel(): ViewName | undefined {
    const frame = this.lastFrame;
    return frame === undefined || frame.laneSet !== 0
      ? undefined
      : viewOrder[readoutView(frame)];
  }

  // The Views popover's Measure row: lights the channel read and offers only the views drawn (L
  // alone on a mono track, as the Source row), nothing before a Single frame says what is read.
  private updateReadoutChoices(): void {
    const read = this.readChannel();
    const offered: readonly ViewName[] =
      read === undefined
        ? []
        : this.inputChannels < 2
          ? ["left"]
          : this.drawnViews();
    // Touched only when what it shows changes: it is asked on every frame.
    const shown = `${read ?? ""} ${offered.join(" ")}`;
    if (shown !== this.readoutShown) {
      this.readoutShown = shown;
      this.readoutChoices?.show(read ?? "");
      this.readoutChoices?.enable((value) =>
        offered.includes(value as ViewName),
      );
    }
    this.updateViewSummary();
  }

  // Single's summary in the Views field: how many of the four views the frame draws (a mono
  // track's one, whatever was asked for), then the channel read, "—" until a Single frame says:
  // "2/4·L", at one width whatever it says. Its name for screen readers says the same in words.
  private updateViewSummary(): void {
    const drawn = this.drawnViews();
    const read = this.readChannel();
    const text = `${String(drawn.length)} of ${String(viewOrder.length)} channels, measures ${
      read === undefined ? "—" : viewNames[ChannelView[read]]
    }`;
    if (text === this.viewSummary) {
      return;
    }
    this.viewSummary = text;
    element("views-shown").textContent =
      `${String(drawn.length)} of ${String(viewOrder.length)}`;
    this.viewsNames.single = `Views: ${text}`;
    this.nameViews();
  }

  // The Views field's name for screen readers: the current mode's summary, in words.
  private nameViews(): void {
    element("views-details").setAttribute(
      "aria-label",
      this.multiMode() ? this.viewsNames.multi : this.viewsNames.single,
    );
  }

  // Asks the engine to draw `views`, in Single: at least one, and nothing on a mono track, whose
  // one channel is Left whatever was asked for before it turned mono.
  private showViews(views: readonly ViewName[]): void {
    if (views.length > 0 && this.inputChannels >= 2) {
      this.request("channelViews", [...views]);
    }
  }

  // Single's Channels list: each view ticked as asked for, in its trace's colour. The last one
  // ticked cannot be unticked, and on a mono track Left alone is ticked and nothing can change,
  // as the other views would only repeat it. The one place that decides, for every case.
  private updateViewList(): void {
    const asked = this.shownViews();
    const states = viewOrder.map((view) => ({
      view,
      ...viewToggleState(view, asked, this.inputChannels),
    }));
    const ticked = states.filter((state) => state.pressed && !state.disabled);
    this.updateViewSummary();
    this.viewList?.show(
      states.map(({ view, pressed, disabled }) => {
        const checked = pressed && !disabled;
        const locked = disabled
          ? "A mono track has one channel"
          : checked && ticked.length === 1
            ? "One channel always shows"
            : this.inputChannels < 2
              ? "A mono track has one channel"
              : undefined;
        const name = viewNames[ChannelView[view]];
        return {
          id: view,
          name: () => [document.createTextNode(name)],
          nameKey: name,
          colour: `var(${viewColorTokens[view]})`,
          checked,
          help: viewHelp[view],
          ...(locked === undefined ? {} : { locked }),
        };
      }),
    );
  }

  // Only the Trigger time base is triggered: in Beats and Free the trigger controls are hidden,
  // with their group, so no empty group keeps its space, and the popover closes. Re-arm shows in
  // the Single trigger mode only.
  private updateTriggerControls(): void {
    const triggered = this.store.get("timeBase") === "trigger";
    element("trigger-group").hidden = !triggered;
    element("trigger-tools").hidden = !triggered;
    element("arm").hidden =
      !triggered || this.store.get("triggerMode") !== "single";
    if (!triggered) {
      this.closePopover("trigger-details", "trigger-popover");
    }
    this.updateReadoutChoices();
    this.updateSourceChoices();
  }

  // Under its field, left-aligned with it as far as the window allows.
  private placePopover(id: string, groupId: string): void {
    const popover = element(id);
    if (popover.hidden || placedByStyle.has(id)) {
      return;
    }
    const group = element(groupId);
    const bar = popover.offsetParent?.clientWidth ?? window.innerWidth;
    const margin = 8;
    const left = Math.min(group.offsetLeft, bar - popover.offsetWidth - margin);
    popover.style.left = `${String(Math.max(margin, left))}px`;
  }

  private showSetting(name: SettingName, value: SettingValue): void {
    if (typeof value === "number") {
      this.fields.get(name)?.show(value);
    } else if (typeof value === "string") {
      this.segmented.get(name)?.show(value);
      if (name === "compare" || name === "compareLane") {
        const lane = String(this.store.get("compareLane"));
        this.compareChoice?.show(
          this.store.get("compare") === "on" ? lane : "off",
        );
      }
      const field = this.choiceSummaries.get(name as ChoiceSetting);
      if (field !== undefined) {
        field.summary.show(value);
        field.details.setAttribute(
          "aria-label",
          `${field.word}: ${field.labels[value] ?? value}`,
        );
      }
    }
    if (name === "channelViews") {
      this.updateViewList();
    }
    if (name === "channelViews" || name === "triggerSource") {
      this.updateSourceChoices();
    }
    if (name === "triggerMode" || name === "timeBase") {
      this.updateTriggerControls();
    }
    if (name === "displayMode" || name === "compare") {
      this.updateMultiControls();
    }
    if (
      name === "displayMode" ||
      name === "compare" ||
      name === "compareLane"
    ) {
      // Preview fix is the sum's; it resets when the sum leaves the screen, with Compare or for
      // the difference.
      const sumShown =
        this.compareMode() && this.store.get("compareLane") !== "difference";
      if (!sumShown && this.previewFix) {
        this.previewFix = false;
        this.backend.previewFix(false);
      }
    }
    // Compare's note and controls follow the switch at once: a track shown alone gets no
    // new lanes from it, so no frame would place them.
    if (name === "compare" || name === "laneLabels" || name === "align") {
      this.placeLaneLabels();
    }
    // Level cursors are drawn at their level in each lane.
    if (
      name === "verticalScale" ||
      name === "viewLayout" ||
      name === "channelViews"
    ) {
      this.placeCursors();
    }
    if (name === "hoverReadout") {
      element("hover-toggle").setAttribute(
        "aria-pressed",
        String(value === "on"),
      );
      this.updateHover();
    }
    if (name === "meter") {
      element("meter-toggle").setAttribute(
        "aria-pressed",
        String(value === "on"),
      );
      this.updateCursorReadout();
    }
    if (name === "autoFreeze") {
      element("auto-freeze").setAttribute(
        "aria-pressed",
        String(value === "on"),
      );
    }
    if (name === "beatSpan" && typeof value === "string") {
      this.beatSpanField?.show(this.beatSpanNames.indexOf(value));
    }
    // One span pill, of the time base's own span; its help names that span.
    if (name === "timeBase") {
      element("span-field").hidden = value !== "trigger";
      element("free-span-field").hidden = value !== "free";
      element("beat-span-field").hidden = value !== "beats";
      element("time-group").dataset.help =
        value === "free"
          ? "freeSpan"
          : value === "beats"
            ? "beatSpan"
            : "triggerSpan";
    }
    this.updateTriggerSummary();
    if (name === "concertPitch") {
      this.updateCursorReadout();
    }
    if (
      name === "laneChannel" ||
      name === "compare" ||
      name === "compareLane" ||
      name === "laneLabels" ||
      name === "displayMode"
    ) {
      this.updateGroupSummary();
      this.updateViewSummary();
    }
    // Zoom is a temporary view of one span; a new span starts unzoomed.
    if (
      (name === "triggerSpan" ||
        name === "freeSpan" ||
        name === "timeBase" ||
        name === "beatSpan") &&
      isZoomed(this.view)
    ) {
      this.setView(fullView);
    }
    // Settings that move the grid, labels or trigger line; the engine's next frame follows.
    const screenSettings: SettingName[] = [
      "verticalScale",
      "viewLayout",
      // Redraws the traces at once, whatever the next frame brings.
      "traceShape",
      "channelViews",
      "triggerSpan",
      "freeSpan",
      "triggerMode",
      "timeBase",
    ];
    if (name === "timeBase" || isGridSetting(name)) {
      this.updateGridControls();
    }
    if (screenSettings.includes(name) || isGridSetting(name)) {
      this.drawScreen();
      this.loop.invalidate();
    }
    this.placeTriggerMarker();
  }

  // The Trigger field names the mode, as wide as the longest; the popover holds the rest.
  private updateTriggerSummary(): void {
    const mode = this.store.get("triggerMode") as string;
    const name = triggerModeNames[mode] ?? mode;
    this.triggerSummary ??= new StackedSummary(
      element("trigger-summary"),
      triggerModeNames,
    );
    this.triggerSummary.show(mode);
    element("trigger-details").setAttribute("aria-label", `Trigger: ${name}`);
  }

  // ----- Screen -----

  // Moving the window to a display of another pixel ratio changes no size, so no resize event
  // comes. A query for the current resolution stops matching then; it is asked again for each
  // new ratio.
  private watchPixelRatio(): void {
    window
      .matchMedia(`(resolution: ${String(window.devicePixelRatio)}dppx)`)
      .addEventListener(
        "change",
        () => {
          this.watchPixelRatio();
          this.resize();
        },
        { once: true },
      );
  }

  private resize(): void {
    for (const [group, , id] of popovers) {
      this.placePopover(id, group);
    }
    this.sizeScreen();
  }

  // What follows the screen's size: the canvases in device pixels, the engine's width, the level
  // cursors (placed in pixels from its height) and the picture. The size is read once, so both
  // canvases and the record take the same one.
  private sizeScreen(): void {
    const width = this.screen.clientWidth;
    const height = this.screen.clientHeight;
    const ratio = window.devicePixelRatio;
    this.sized = { width, height, ratio };
    this.placeCursors();
    for (const canvas of [this.gridCanvas, this.traceCanvas]) {
      canvas.width = Math.round(width * ratio);
      canvas.height = Math.round(height * ratio);
    }
    this.backend.setViewport(width, ratio);
    this.drawScreen();
    this.loop.invalidate();
    // The line width is in device pixels: a move to a display of another ratio changes it.
    this.queueTraceView();
  }

  // The traces the latest frame draws, which the lanes are laid out for: its lanes, its views, or,
  // with neither, the lanes an empty frame takes.
  private traceCount(): number {
    const lanes = this.lastFrame?.lanes.length ?? 0;
    const views = this.lastFrame?.views.size ?? 0;
    return lanes > 0
      ? lanes
      : views > 0
        ? Math.max(1, this.drawnViews().length)
        : this.emptyFrameLanes();
  }

  // Whether the latest frame is a Compare frame, whose lane has a place of its own (FR-15.1).
  private compared(): boolean {
    return this.lastFrame?.comparison !== undefined;
  }

  // The lanes on the screen (screenLanes), which the grid, level labels and level cursors follow.
  private screenLaneList(): Lane[] {
    return screenLanes(this.traceCount(), this.layout(), this.compared());
  }

  private laneCount(): number {
    return this.screenLaneList().length;
  }

  // The lanes for a frame that carries no views and no lanes (No tempo, before the first frame):
  // one per view shown when stacked. The editor's native grid lays such a frame out the same way.
  private emptyFrameLanes(): number {
    return this.layout() === ViewLayout.stacked
      ? Math.max(1, this.shownViews().length)
      : 1;
  }

  // Multi shows lanes of the tracks ticked, with or without Compare.
  private multiMode(): boolean {
    return this.store.get("displayMode") !== "single";
  }

  // Compare is on show: switched on, in Multi. It keeps its state in Single.
  private compareMode(): boolean {
    return this.multiMode() && this.store.get("compare") === "on";
  }

  // Multi's summary in the Views field: how many of the host's tracks are shown, as "3 of 7".
  // Its name for screen readers also says the lane channel and what Compare adds.
  private updateGroupSummary(): void {
    const channel = this.store.get("laneChannel") as ViewName;
    const compare = this.store.get("compare") === "on";
    // Before the list comes, this track alone of the one there is known to be.
    const total = Math.max(this.tracks.length, 1);
    const shown = Math.max(
      this.tracks.filter((track) => track.shown).length,
      1,
    );
    const count = `${String(shown)} of ${String(total)}`;
    element("views-count").textContent = count;
    const lane =
      this.store.get("compareLane") === "difference" ? "difference" : "sum";
    this.viewsNames.multi = `Views: ${String(shown)} of ${String(total)} ${
      total === 1 ? "track" : "tracks"
    }, ${viewNames[ChannelView[channel]]}${compare ? `, Compare ${lane}` : ""}`;
    this.nameViews();
  }

  // Asks the engine to show `ids`, the other tracks, in Multi: at most as many as it shows with
  // this one, the first in the host's order.
  private showTracks(ids: readonly number[]): void {
    this.backend.showTracks(ids.slice(0, maxShownTracks - 1));
  }

  // The Channels list in Multi, as the tracks message has it: each instance with its lane's
  // colour and name, this track's marked and always ticked, and, once as many are ticked as Multi
  // shows, the others not to be ticked.
  private updateTrackList(): void {
    const styles = laneStyles(this.tracks);
    const full =
      this.tracks.filter((track) => track.shown).length >= maxShownTracks;
    this.trackList?.show(
      this.tracks.map((track, index) => {
        const style = styles[index] ?? { name: "", colour: 0, instance: 0 };
        const locked = track.own
          ? "This track is always shown"
          : !track.shown && full
            ? `At most ${String(maxShownTracks)} tracks are shown`
            : undefined;
        return {
          id: String(track.id),
          name: (row: HTMLElement) => laneNameNodes(style, true, row),
          nameKey: `${style.name}/${String(style.instance)}`,
          colour: cssColour(style.colour),
          checked: track.shown,
          ...(locked === undefined ? {} : { locked }),
          ...(track.own ? { tag: "this track" } : {}),
        };
      }),
    );
  }

  // Names and colours every kept lane set again: a lane with no host name or colour takes those
  // of its place in the list of tracks, which a new list may have moved.
  private styleLanes(): void {
    const places = new Map(
      this.tracks.map((track, index) => [track.id, index]),
    );
    for (const [laneSet, labels] of this.laneLabelSets) {
      this.laneSets.set(laneSet, laneStyles(labels, places));
    }
  }

  // Whether the lane labels show, in Single and in Multi: as Labels says. Compare's own lane
  // keeps its label whatever it says (placeLaneLabels).
  private labelsShown(): boolean {
    return this.store.get("laneLabels") !== "off";
  }

  // What the lane labels name: a multi-track frame's lanes, or a Single frame's views, each view
  // by its name in its trace's colour, in the order drawn (the frame's, not the setting's, so the
  // labels never disagree with the traces).
  private labelsOf(frame: Frame | undefined): LabelStyle[] {
    if (frame !== undefined && frame.lanes.length === 0) {
      return viewOrder
        .filter((view) => frame.views.has(ChannelView[view]))
        .map((view) => ({
          id: viewLabelId - ChannelView[view],
          name: viewNames[ChannelView[view]],
          colour: 0,
          swatch: `var(${viewColorTokens[view]})`,
          own: false,
          instance: 0,
        }));
    }
    return this.lanesOf(frame).map((lane) => ({
      ...lane,
      swatch: cssColour(lane.colour),
    }));
  }

  // The names and colours of a multi-track frame's lanes: its own lane set, or the latest.
  private lanesOf(frame: Frame | undefined): LaneStyle[] {
    const count = frame?.lanes.length ?? 0;
    if (count === 0) {
      return [];
    }
    let styles =
      this.laneSets.get(frame?.laneSet ?? 0) ??
      this.laneSets.get(this.latestLaneSet) ??
      [];
    // A Compare frame's last lane is the sum of the compared tracks, or their difference, as the
    // frame says.
    if (frame?.comparison !== undefined && styles.length > 0) {
      styles = [
        ...styles,
        {
          id: sumLaneId,
          name:
            frame.comparison.lane === CompareLane.difference
              ? "Difference"
              : "Sum",
          colour: sumLaneColour,
          own: false,
          instance: 0,
        },
      ];
    }
    // A set that does not match the frame's lanes yet: fallback names and colours by position.
    return styles.length === count
      ? styles
      : laneStyles(
          Array.from({ length: count }, (_, index) => ({
            id: -1 - index,
            name: "",
            colour: null,
            own: false,
            instance: 0,
          })),
        );
  }

  // Lane names: at the top of each stacked lane, or as a legend when overlaid, after the top
  // level label. Labels Off hides them, except with Compare on, whose controls they hold.
  private placeLaneLabels(): void {
    const container = element("lane-labels");
    const frame = this.lastFrame;
    const styles = this.labelsOf(frame);
    const height = this.screen.clientHeight;
    const comparing = frame?.comparison !== undefined;
    const shown = this.labelsShown();
    // Rebuilt only when what they show changes, so a click on a label is never lost to a redraw.
    const key = JSON.stringify([
      shown,
      styles,
      comparing,
      this.previewFix,
      frame?.comparison?.lane,
      this.store.get("align"),
      this.layout(),
      height,
      this.compareMode() && frame !== undefined,
      this.tracks.length > 1,
    ]);
    if (key === this.laneLabelsKey) {
      return;
    }
    this.laneLabelsKey = key;
    // The chip under the pointer is gone: no leave event comes from it (Pointer Events 3, 4.1.3),
    // so its highlight ends here, until the pointer moves again.
    this.labelUnderPointer = undefined;
    this.updateHighlight();
    // Labels Off: no track or view is named, nor marked for samples that are not finite (the
    // status chip reports them), and no match shows. Compare's own lane keeps its label, which
    // holds its figure and its switch.
    const labelled = (style: LabelStyle) => shown || style.id === sumLaneId;
    const button = (
      text: string,
      help: HelpTopic,
      onClick: (event: MouseEvent) => void,
    ) => {
      const node = document.createElement("button");
      node.textContent = text;
      node.dataset["help"] = help;
      node.addEventListener("click", onClick);
      return node;
    };
    // With Compare on, each other track's name is followed by how it lines up with this track;
    // the sum shows its gain and the fix preview.
    this.matchNodes = [];
    this.sumNode = undefined;
    this.laneNameNodes = [];
    const name = (style: LabelStyle, index: number) => {
      const node = document.createElement("span");
      this.laneNameNodes[index] = node;
      node.style.setProperty("--swatch", style.swatch);
      node.dataset["trace"] = String(style.id);
      node.classList.toggle("own", style.own);
      if (style.id === sumLaneId) {
        // What the lane shows, which the frame says: the sum or the difference (Compare's
        // switch in the Views popover chooses), each with its figure; the sum with Preview fix,
        // the difference with Align.
        const difference = frame?.comparison?.lane === CompareLane.difference;
        const figure = document.createElement("span");
        figure.className = "lane-readout";
        figure.dataset["help"] = difference ? "nullDepth" : "sumGain";
        this.sumNode = figure;
        node.append(style.name, figure);
        if (difference) {
          // The difference's switch, where the sum has Preview fix: whether the checked tracks
          // are lined up with this one first.
          const on = this.store.get("align") === "on";
          const align = button("Align", "align", () => {
            this.request("align", on ? "off" : "on");
          });
          align.className = "align";
          align.setAttribute("aria-pressed", String(on));
          node.append(align);
        } else {
          const preview = button("Preview fix", "previewFix", () => {
            this.previewFix = !this.previewFix;
            this.backend.previewFix(this.previewFix);
            this.placeLaneLabels();
          });
          preview.className = "preview-fix";
          preview.setAttribute("aria-pressed", String(this.previewFix));
          node.append(preview);
        }
        return node;
      }
      node.append(...laneNameNodes(style, true));
      if (comparing && !style.own) {
        const match = document.createElement("span");
        match.className = "lane-readout";
        match.dataset["help"] =
          frame.comparison.lane === CompareLane.difference
            ? "matchDifference"
            : "match";
        this.matchNodes[index] = match;
        node.append(match);
      }
      return node;
    };
    const place = (node: HTMLElement, top: number) => {
      node.className = "lane-label";
      node.style.left = `${String(amplitudeLabelWidth + 10)}px`;
      node.style.top = `${String(top)}px`;
      return node;
    };
    // A label in each lane of the screen, at its top, naming the traces in it: stacked, one each;
    // overlaid, a legend of the views or tracks, and with Compare on, Compare's label in its own
    // lane below theirs.
    const layout = this.layout();
    container.replaceChildren(
      ...screenLanes(styles.length, layout, comparing).flatMap((lane, at) => {
        const node = document.createElement("div");
        styles.forEach((style, index) => {
          if (
            labelled(style) &&
            screenLaneOf(styles.length, layout, comparing, index) === at
          ) {
            node.append(name(style, index));
          }
        });
        return node.childElementCount > 0
          ? [
              place(
                node,
                ((1 - (lane.center + lane.halfHeight)) / 2) * height + 3,
              ),
            ]
          : [];
      }),
    );
    // The Compare view without another track says why.
    if (this.compareMode() && frame?.lanes.length === 1) {
      const note = document.createElement("div");
      note.className = "compare-note";
      // Other instances there may be, not shown: the Channels list is where they are ticked.
      note.textContent =
        this.tracks.length > 1
          ? "No other track to compare with. Tick one under Channels, in Views."
          : `No other track to compare with. Insert ${brand.name} on another track.`;
      container.append(note);
    }
    fitCutNames(container);
    this.updateMatches(frame);
  }

  // The Compare view's readouts in the lane labels: each compared track's offset and polarity
  // (or that it matches nowhere clearly) and correlation against this track, and the sum's gain
  // (with the fix, while it is previewed); beside the difference, what Align takes out of each
  // track (its delay, its gain and a flip) and how deep the difference nulls.
  private updateMatches(frame: Frame | undefined): void {
    this.laneNameNodes.forEach((node, index) => {
      node.classList.toggle(
        "invalid",
        frame !== undefined &&
          frame.lanes.length > 0 &&
          laneHoldsInvalid(frame, index),
      );
    });
    const comparison = frame?.comparison;
    this.matchNodes.forEach((node, index) => {
      const match = comparison?.matches[index];
      if (node === undefined || match === undefined || frame === undefined) {
        return;
      }
      // No correlation: they never sound together as they are. A track can still match at an
      // offset (a short hit and its late copy); otherwise nothing was measured.
      const measured = !Number.isNaN(match.correlation);
      if (!measured && !match.related) {
        node.textContent = "—";
        return;
      }
      const offsetMs = (match.offset / frame.sampleRate) * 1000;
      if (comparison?.lane === CompareLane.difference) {
        // What Align takes out: the delay, and a flip.
        const parts = [
          formatOffset(offsetMs),
          ...(match.inverted ? ["Ø"] : []),
        ];
        node.textContent = match.related ? parts.join(" · ") : "unrelated";
        return;
      }
      const alignment = match.related
        ? `${formatOffset(offsetMs)}${match.inverted ? " Ø" : ""}`
        : "unrelated";
      node.textContent = `${alignment} · ${formatCorrelation(match.correlation)}`;
    });
    if (this.sumNode !== undefined && comparison !== undefined) {
      if (comparison.lane === CompareLane.difference) {
        // How deep the difference nulls against this track: a level, neither good nor bad.
        this.sumNode.textContent = formatGain(comparison.nullDepth);
        this.sumNode.className = "lane-readout";
      } else {
        const gain = this.previewFix
          ? comparison.fixedGain
          : comparison.sumGain;
        this.sumNode.textContent = formatGain(gain);
        this.sumNode.className = `lane-readout ${gain < -summingMarginDb ? "cancels" : gain > summingMarginDb ? "reinforces" : ""}`;
      }
    }
  }

  private beatsMode(): boolean {
    return this.store.get("timeBase") === "beats";
  }

  private timeBaseName(): TimeBaseName {
    return this.store.get("timeBase") as TimeBaseName;
  }

  // The grid the screen draws, the same in every time base.
  private gridChoice(): GridChoice {
    return gridChoice((name) => this.store.get(name));
  }

  // The grid's field and popover show the grid drawn. The Density and the Beat division apply only
  // with labels to place (and lines, which come with them); the Beat division in Beats only.
  private updateGridControls(): void {
    const grid = this.gridChoice();
    const timeBase = this.timeBaseName();
    const steps = drawsLabels(grid.type);
    this.gridSummary?.show(grid.type);
    element("grid-details").setAttribute(
      "aria-label",
      `Grid: ${choiceLabels.gridType[grid.type]}`,
    );
    const rows = [
      ["Type", grid.type, () => true],
      ["Density", grid.time, () => steps],
      ["Division", grid.division, () => steps && timeBase === "beats"],
    ] as const;
    for (const [kind, value, enabled] of rows) {
      const row = this.gridRows.get(kind);
      row?.show(value);
      row?.enable(enabled);
    }
  }

  // The musical grid of the last synced frame, when in Beats mode.
  private musicalLayout(): ReturnType<typeof musicalGrid> | undefined {
    const frame = this.lastFrame;
    if (!this.beatsMode() || frame?.synced !== true || frame.noTempo) {
      return undefined;
    }
    const grid = this.gridChoice();
    const covered = frame.viewEnd - frame.viewStart;
    const spanSeconds =
      covered > 0 ? frame.visibleSamples / frame.sampleRate / covered : 0;
    return musicalGrid(
      frame.cycleStart,
      frame.spanQuarterNotes,
      frame.barQuarterNotes,
      this.view,
      frame.spanQuarterNotes > 0 && spanSeconds > 0
        ? {
            cycleSeconds: frame.cycleSeconds,
            secondsPerQuarter: spanSeconds / frame.spanQuarterNotes,
          }
        : undefined,
      {
        time: grid.time,
        division: grid.division,
        widthCss: this.screen.clientWidth,
      },
    );
  }

  private drawScreen(): void {
    // In native mode the editor draws the grid with the traces.
    if (!this.nativeTraces) {
      const grid = this.gridChoice();
      drawGrid(
        this.gridCanvas,
        this.laneCount(),
        { grid: token("--sl-grid"), axis: token("--sl-grid-strong") },
        {
          type: grid.type,
          level: grid.level,
          columns: gridColumns[grid.time],
          verticalLines: this.musicalLayout()?.lines,
        },
      );
    }
    this.drawnLaneCount = this.laneCount();
    this.placeLabels();
    this.placeLaneLabels();
    this.placeTriggerMarker(); // It sits in a lane: lanes may have changed.
  }

  // Redraws grid and labels when a tempo-synced sweep starts a new cycle. The playhead itself is
  // drawn with the traces.
  private showSweep(frame: Frame): void {
    if (!frame.synced || frame.cursor === undefined) {
      return;
    }
    const key = `${String(frame.cycleStart)}/${String(frame.spanQuarterNotes)}/${String(frame.barQuarterNotes)}/${frame.cycleSeconds.toFixed(3)}`;
    if (key !== this.cycleKey) {
      this.cycleKey = key;
      this.drawScreen();
    }
  }

  // The time the whole screen width shows in Trigger or Free, as the engine says: the span, or
  // what is kept of it when limited. The time base's span setting until such a frame comes.
  private secondsSpan(): number {
    const frame = this.lastFrame;
    return frame !== undefined && !frame.synced && frame.spanSeconds > 0
      ? frame.spanSeconds
      : this.store.number(
          this.store.get("timeBase") === "free" ? "freeSpan" : "triggerSpan",
        );
  }

  // Where time zero sits across the span, as the engine placed it: the trigger point. The
  // Position setting until a frame comes; NaN while running free or in Beats.
  private triggerPoint(): number {
    const frame = this.lastFrame;
    return frame === undefined
      ? this.store.number("triggerPosition")
      : (frame.triggerPosition ?? Number.NaN);
  }

  // Whether the trigger point moved since the labels were placed: by more than float rounding,
  // which would otherwise place them again on every frame.
  private triggerPointMoved(): boolean {
    const zero = this.triggerPoint();
    return (
      Number.isNaN(zero) !== Number.isNaN(this.labelledZero) ||
      Math.abs(zero - this.labelledZero) > 1e-6
    );
  }

  // Leaves out the level labels that reach into the band the time labels take at the bottom, the
  // screen's full width, so that the first time label can sit at its line on the left edge. With no
  // time labels there is no band. All boxes are read before any label is removed: one layout.
  private dropLevelsInTimeBand(
    levels: readonly HTMLElement[],
    times: readonly HTMLElement[],
  ): void {
    if (times.length === 0) {
      return;
    }
    const bandTop = Math.min(
      ...times.map((node) => node.getBoundingClientRect().top),
    );
    const inBand = levels.filter(
      (node) => node.getBoundingClientRect().bottom > bandTop,
    );
    for (const node of inBand) {
      node.remove();
    }
  }

  private placeLabels(): void {
    const container = element("labels");
    const height = this.screen.clientHeight;
    const width = this.screen.clientWidth;
    const span = this.secondsSpan();
    const zero = this.triggerPoint();
    this.labelledSpan = span;
    this.labelledZero = zero;
    const free = this.freeRunning();
    const grid = this.gridChoice();
    // None places no label; the trigger line below stays in every type.
    const labelled = drawsLabels(grid.type);
    const levels: HTMLElement[] = [];
    for (const label of labelled
      ? amplitudeLabels(
          this.store.number("verticalScale"),
          this.laneCount(),
          height,
          grid.level,
        )
      : []) {
      const node = document.createElement("div");
      node.className = "axis-label";
      node.textContent = label.text;
      // Clear of the trigger level marker, which sits on the left edge.
      node.style.left = "13px";
      node.style.top = `${String(Math.min(height - 14, Math.max(2, label.position * height - 6)))}px`;
      levels.push(node);
    }
    // Beats without the host's tempo: no positions to label.
    const untimed = this.beatsMode() && this.lastFrame?.noTempo === true;
    const times: HTMLElement[] = [];
    for (const label of labelled
      ? (this.musicalLayout()?.labels ??
        (untimed ? [] : timeLabels(span, free, this.view, zero)))
      : []) {
      const node = document.createElement("div");
      node.className = "axis-label";
      node.textContent = label.text;
      if (label.detail !== undefined) {
        const detail = document.createElement("span");
        detail.className = "axis-detail";
        detail.textContent = label.detail;
        node.append(detail);
      }
      node.style.left = `${String(label.position * width + 4)}px`;
      node.style.bottom = "3px";
      times.push(node);
    }
    container.replaceChildren(...levels, ...times);
    this.dropLevelsInTimeBand(levels, times);
    const triggerLine = element("trigger-line");
    const triggerX = toScreen(this.view, zero);
    triggerLine.style.left = `${String(triggerX * 100)}%`;
    triggerLine.hidden =
      free ||
      this.beatsMode() ||
      !Number.isFinite(triggerX) ||
      triggerX < 0 ||
      triggerX > 1;
  }

  // The level marker sits in the lane of the trace the trigger reads (traceLanes): stacked, the
  // trigger source's; overlaid, the views' or tracks' shared lane, the upper half with Compare on.
  // In the multi-track view the own track triggers, so the marker sits in its lane.
  private markerGeometry(): { center: number; halfHeight: number } {
    // The lane of the view the trigger reads, which a Single frame always draws.
    const shown = this.drawnViews();
    const source = this.triggerView();
    const ownLane = Math.max(
      0,
      this.lanesOf(this.lastFrame).findIndex((style) => style.own),
    );
    const index = markerLane(
      this.layout(),
      (this.lastFrame?.lanes.length ?? 0) > 0,
      ownLane,
      shown,
      source,
    );
    const lane = traceLanes(this.traceCount(), this.layout(), this.compared())[
      index
    ] ?? {
      center: 0,
      halfHeight: 1,
    };
    const height = this.screen.clientHeight;
    return {
      center: ((1 - lane.center) / 2) * height,
      halfHeight: (lane.halfHeight / 2) * height,
    };
  }

  // Whether the trace runs free, as the engine decided for the latest frame.
  private freeRunning(): boolean {
    return this.lastFrame?.freeRunning === true;
  }

  private placeTriggerMarker(): void {
    const marker = element("trigger-marker");
    this.markerView = this.triggerView();
    const hidden = this.freeRunning() || this.beatsMode();
    marker.hidden = hidden;
    if (hidden) {
      return;
    }
    const { center, halfHeight } = this.markerGeometry();
    const level =
      this.store.number("triggerLevel") /
      fullScale(this.store.number("verticalScale"));
    marker.style.top = `${String(center - Math.max(-1, Math.min(1, level)) * halfHeight)}px`;
    const ownStyle = this.lanesOf(this.lastFrame).find((style) => style.own);
    marker.style.setProperty(
      "--marker",
      ownStyle !== undefined
        ? cssColour(ownStyle.colour)
        : `var(${viewColorTokens[this.triggerView()]})`,
    );
  }

  // The view the trigger reads (DESIGN.md 1.2.1): the one a Single frame names, the trigger source
  // when drawn, else the first view drawn; before any Single frame (none yet, or a Multi or
  // Compare one not replaced yet), the setting.
  private triggerView(): ViewName {
    const frame = this.lastFrame;
    return frame !== undefined && frame.laneSet === 0
      ? viewOrder[frame.triggerView]
      : (this.store.get("triggerSource") as ViewName);
  }

  // The Source row lights the view the trigger reads and offers only views drawn: on a mono
  // track L alone, also while the latest frame draws none (no tempo yet), as Single's Channels list does.
  private updateSourceChoices(): void {
    const drawn: readonly ViewName[] =
      this.inputChannels < 2 ? ["left"] : this.drawnViews();
    // Touched only when what it shows changes: it is asked on every frame.
    const shown = `${this.triggerView()} ${drawn.join(" ")}`;
    if (shown !== this.sourceShown) {
      this.sourceShown = shown;
      this.sourceChoices?.show(this.triggerView());
      this.sourceChoices?.enable((value) => drawn.includes(value as ViewName));
    }
    // A frame naming another view moves the marker to its lane and colour.
    if (this.triggerView() !== this.markerView) {
      this.placeTriggerMarker();
    }
  }

  // Positions the cursors where their frame positions and levels are on screen, and their
  // readout.
  private placeCursors(): void {
    for (const [id, position] of [
      ["cursor-a", this.timeCursors?.a],
      ["cursor-b", this.timeCursors?.b],
    ] as const) {
      const line = element(id);
      const x = position === undefined ? -1 : toScreen(this.view, position);
      line.hidden = x < 0 || x > 1;
      line.style.left = `${String(x * 100)}%`;
    }
    this.placeLevelCursors();
    this.updateCursorReadout();
  }

  // Level cursors are drawn in every lane. Their elements are only rebuilt when the number of
  // lanes changes, so that a drag in progress keeps its element.
  private placeLevelCursors(): void {
    const container = element("level-cursor-lines");
    const laneList = this.screenLaneList();
    const wanted = this.levelCursors === undefined ? 0 : laneList.length * 2;
    if (container.children.length !== wanted) {
      const lines: HTMLElement[] = [];
      // None while the cursors are off: lines built then would never be placed.
      (wanted === 0 ? [] : laneList).forEach((_, lane) => {
        for (const key of ["first", "second"] as const) {
          const line = document.createElement("div");
          line.className = "level-cursor";
          line.dataset["lane"] = String(lane);
          line.dataset["key"] = key;
          line.dataset["help"] = "levelCursor" satisfies HelpTopic;
          const label = document.createElement("span");
          label.textContent = key === "first" ? "1" : "2";
          line.append(label);
          lines.push(line);
        }
      });
      container.replaceChildren(...lines);
    }
    const levels = this.levelCursors;
    if (levels === undefined) {
      return;
    }
    const scale = fullScale(this.store.number("verticalScale"));
    const height = this.screen.clientHeight;
    for (const line of container.children as HTMLCollectionOf<HTMLElement>) {
      const lane = laneList[Number(line.dataset["lane"])] ?? laneList[0];
      const relative =
        levels[line.dataset["key"] === "first" ? "first" : "second"] / scale;
      line.hidden = lane === undefined || Math.abs(relative) > 1;
      if (lane !== undefined) {
        line.style.top = `${String(((1 - (lane.center + relative * lane.halfHeight)) / 2) * height)}px`;
      }
    }
  }

  // The trace cursors snap to in the screen lane `lane` (screenLanes): the lane's own trace, or in
  // a lane traces share (overlaid), the readout channel when shown, or the first view shown.
  private snapValues(lane?: number): Float32Array | undefined {
    const frame = this.lastFrame;
    if (frame === undefined) {
      return undefined;
    }
    if (frame.lanes.length > 0) {
      const own = Math.max(
        0,
        this.lanesOf(frame).findIndex((style) => style.own),
      );
      // The lane's own trace, or in the lane the tracks share, this track's (traceInScreenLane).
      return frame.lanes[
        lane === undefined
          ? own
          : traceInScreenLane(
              frame.lanes.length,
              this.layout(),
              frame.comparison !== undefined,
              lane,
              own,
            )
      ];
    }
    const views = [...frame.views.values()];
    if (lane !== undefined && this.layout() === ViewLayout.stacked) {
      return views[lane];
    }
    // Overlaid, the channel the readouts measure, which the engine draws.
    return frame.views.get(readoutView(frame));
  }

  // The traces as the hover readout sees them, in lane order, named.
  private hoverContext(frame: Frame): HoverContext {
    let traces: HoverTrace[];
    let overlaid = 0;
    if (frame.lanes.length > 0) {
      const styles = this.lanesOf(frame);
      traces = frame.lanes.map((values, index) => ({
        name: styles[index]?.name ?? `Track ${String(index + 1)}`,
        instance: styles[index]?.instance ?? 0,
        values,
      }));
      overlaid = Math.max(
        0,
        styles.findIndex((style) => style.own),
      );
    } else {
      const source = readoutView(frame);
      traces = [];
      for (const [view, values] of frame.views) {
        if (view === source) {
          overlaid = traces.length;
        }
        traces.push({ name: viewNames[view], values });
      }
    }
    return {
      frame,
      traces,
      overlaid,
      layout: this.layout(),
      view: this.view,
      a4: this.store.number("concertPitch"),
      freeRunning: frame.freeRunning,
    };
  }

  // The overlaid trace highlighted (FR-16.4), by its label's id: the lane label under the pointer,
  // else the trace the hover readout reads; none where traceLayering highlights nothing (Stack,
  // Compare's lane, a lane of one) and none for a label with no track to name (Compare's, or a lane
  // whose set has not arrived, which the editor could not find either). Redraws the traces and
  // tells the editor when it changes, live or frozen.
  private updateHighlight(): void {
    const frame = this.lastFrame;
    const labels = this.labelsOf(frame);
    const id =
      this.labelUnderPointer ??
      (this.readoutTrace === undefined
        ? undefined
        : labels[this.readoutTrace]?.id);
    const index = labels.findIndex((label) => label.id === id);
    const named = id !== undefined && (id >= 0 || id <= viewLabelId);
    const highlight =
      frame !== undefined &&
      named &&
      traceLayering(
        labels.length,
        this.layout(),
        frame.comparison !== undefined,
        index,
      ).dimmed.includes(true)
        ? id
        : undefined;
    if (highlight === this.highlight) {
      return;
    }
    this.highlight = highlight;
    this.loop.invalidate();
    this.queueTraceView();
  }

  // The highlighted trace's index in the last frame, as the page's renderer takes it.
  private highlightIndex(): { highlight?: number } {
    const index = this.labelsOf(this.lastFrame).findIndex(
      (label) => label.id === this.highlight,
    );
    return this.highlight === undefined || index < 0
      ? {}
      : { highlight: index };
  }

  // The highlighted trace as the editor finds it in a frame: a view's name, or a lane's track id.
  private highlightNamed(): TraceViewMessage["highlight"] {
    const id = this.highlight;
    if (id === undefined) {
      return null;
    }
    if (id <= viewLabelId) {
      const view = viewOrder.find(
        (name) => ChannelView[name] === viewLabelId - id,
      );
      return view === undefined ? null : { view };
    }
    return { lane: id };
  }

  // Shows what lies under the pointer, beside it, with a line at the column or point read and a
  // band over the cycle measured; hidden while a help tooltip shows.
  private updateHover(): void {
    const node = element("hover-readout");
    const line = element("hover-line");
    const band = element("hover-cycle");
    const frame = this.lastFrame;
    const readout =
      this.store.get("hoverReadout") === "on" &&
      this.hover !== undefined &&
      frame !== undefined &&
      element("help-tip").hidden
        ? hoverReadout(this.hoverContext(frame), this.hover.x, this.hover.y)
        : undefined;
    // Shown over the overlaid traces, the readout's trace is highlighted (FR-16.4).
    this.readoutTrace = readout?.trace;
    this.updateHighlight();
    if (readout === undefined || this.hover === undefined) {
      node.hidden = true;
      line.hidden = true;
      band.hidden = true;
      return;
    }
    // The marks cover the trace's lane only, in fractions of the screen as the readout gives
    // them: its size in whole pixels need not be the trace's.
    const percent = (fraction: number) => `${String(fraction * 100)}%`;
    const laneTop = percent(readout.top);
    const laneHeight = percent(readout.bottom - readout.top);
    line.style.left = percent(readout.x);
    line.style.top = laneTop;
    line.style.height = laneHeight;
    line.hidden = false;
    if (readout.cycle === undefined) {
      band.hidden = true;
    } else {
      band.style.left = percent(readout.cycle.start);
      band.style.width = percent(readout.cycle.end - readout.cycle.start);
      band.style.top = laneTop;
      band.style.height = laneHeight;
      band.hidden = false;
    }
    const title = document.createElement("strong");
    title.append(
      ...laneNameNodes(
        { name: readout.title, instance: readout.instance },
        false,
      ),
    );
    node.replaceChildren(
      title,
      ...readout.rows.map(({ label, value }) => {
        const line = document.createElement("div");
        line.className = "cursor-row";
        const name = document.createElement("span");
        name.textContent = label;
        const shown = document.createElement("b");
        shown.textContent = value;
        line.append(name, shown);
        return line;
      }),
    );
    node.hidden = false;
    fitCutNames(node);
    // Below and right of the pointer, flipped where the screen ends.
    const width = this.screen.clientWidth;
    const height = this.screen.clientHeight;
    const x = this.hover.x * width;
    const y = this.hover.y * height;
    const left =
      x + hoverOffset + node.offsetWidth <= width
        ? x + hoverOffset
        : x - hoverOffset - node.offsetWidth;
    const top =
      y + hoverOffset + node.offsetHeight <= height
        ? y + hoverOffset
        : y - hoverOffset - node.offsetHeight;
    // At whole pixels: the pointer comes in fractions of one, and text placed at a fraction snaps
    // to the pixel grid apart from its box, so it would step a pixel up and down as the hand moves.
    node.style.left = `${String(Math.round(Math.max(0, left)))}px`;
    node.style.top = `${String(Math.round(Math.max(0, top)))}px`;
  }

  private updateCursorReadout(): void {
    const readout = element("cursor-readout");
    const frame = this.lastFrame;
    const spanSamples =
      frame === undefined
        ? 0
        : frame.visibleSamples / (frame.viewEnd - frame.viewStart);
    // A titled section for the Meter, on top as its button comes first, then one per kind of
    // cursor, its rows a label and a value, as a popover's.
    const sections: { title: string; rows: ReadoutRow[] }[] = [];
    if (this.store.get("meter") === "on") {
      sections.push({
        title: "Meter",
        rows: meterReadout(this.meterValues.values, this.meterValues.source),
      });
    }
    if (
      this.timeCursors !== undefined &&
      frame !== undefined &&
      spanSamples > 0
    ) {
      const { a, b } = this.timeCursors;
      sections.push({
        title: "Time cursors",
        rows: cursorReadout(
          measure(
            this.timeCursors,
            spanSamples / frame.sampleRate,
            frame.synced ? frame.spanQuarterNotes : undefined,
            frame.synced ? frame.barQuarterNotes : undefined,
          ),
          frame.sampleRate,
          this.store.number("concertPitch"),
          {
            a: timeAt(frame, a, frame.freeRunning),
            b: timeAt(frame, b, frame.freeRunning),
          },
        ),
      });
    }
    if (this.levelCursors !== undefined) {
      sections.push({
        title: "Level cursors",
        rows: levelReadout(this.levelCursors),
      });
    }
    readout.hidden = sections.length === 0;
    readout.replaceChildren(
      ...sections.flatMap(({ title, rows }, index) => {
        const heading = document.createElement("div");
        heading.className = "cursor-title";
        heading.textContent = title;
        const lines = rows.map(({ label, value }) => {
          const line = document.createElement("div");
          line.className = "cursor-row";
          const name = document.createElement("span");
          name.textContent = label;
          const shown = document.createElement("b");
          shown.textContent = value;
          line.append(name, shown);
          return line;
        });
        const parts = [heading, ...lines];
        if (index > 0) {
          const rule = document.createElement("hr");
          rule.className = "cursor-rule";
          parts.unshift(rule);
        }
        return parts;
      }),
    );
  }

  // Where the pointer is on the screen, inside its border: as fractions of the trace, which fills
  // the screen's padding box in fractional px, and in px from the trace's top. Elements drawn at a
  // fraction of the screen (time cursors, hover, the selection) use the fractions; those drawn in
  // px at clientHeight scale (level cursors, the trigger marker) use `top`, as clientHeight is a
  // whole number while the screen need not be (Windows at 125% or 150%).
  private screenPoint(event: { clientX: number; clientY: number }): {
    x: number;
    y: number;
    top: number;
    width: number;
  } {
    const box = this.traceCanvas.getBoundingClientRect();
    return {
      x: (event.clientX - box.left) / box.width,
      y: (event.clientY - box.top) / box.height,
      top: event.clientY - box.top,
      width: box.width,
    };
  }

  private listenToScreen(): void {
    // The hover readout follows the pointer over the traces while no button is held.
    // A lane label under it highlights its trace (FR-16.4); the pointer is on a label or on the
    // traces, never both.
    this.screen.addEventListener("pointermove", (event) => {
      const point = this.screenPoint(event);
      this.hover =
        event.buttons === 0 && event.target instanceof HTMLCanvasElement
          ? { x: point.x, y: point.y }
          : undefined;
      const chip =
        event.buttons === 0 && event.target instanceof Element
          ? event.target.closest<HTMLElement>(".lane-label > span")
          : null;
      const trace = Number(chip?.dataset["trace"]);
      this.labelUnderPointer = Number.isFinite(trace) ? trace : undefined;
      this.updateHover();
    });
    for (const type of ["pointerleave", "pointerdown", "wheel"] as const) {
      this.screen.addEventListener(
        type,
        () => {
          this.hover = undefined;
          this.labelUnderPointer = undefined;
          this.updateHover();
        },
        { passive: true },
      );
    }
    // Buttons on the screen (the Compare view's check boxes and fix preview) take their own
    // clicks: the screen would otherwise claim the pointer for a zoom drag, or reset the zoom.
    const overlay = element("lane-labels");
    for (const type of ["pointerdown", "dblclick"] as const) {
      overlay.addEventListener(type, (event) => {
        if ((event.target as Element).closest("button") !== null) {
          event.stopPropagation();
        }
      });
    }
    for (const [id, key] of [
      ["cursor-a", "a"],
      ["cursor-b", "b"],
    ] as const) {
      const line = element(id);
      line.addEventListener("pointerdown", (event) => {
        event.stopPropagation();
        followDrag(line, event, (moveEvent) => {
          const frame = this.lastFrame;
          if (this.timeCursors === undefined) {
            return;
          }
          const point = this.screenPoint(moveEvent);
          // The cursor snaps to the trace in the screen lane under the pointer: stacked, that
          // lane's; overlaid, the readout channel, or in Compare's own lane, Compare's.
          const values = this.snapValues(
            screenLaneAt(
              this.traceCount(),
              this.layout(),
              this.compared(),
              point.y,
            ),
          );
          const x = Math.min(1, Math.max(0, point.x));
          let position = toFrame(this.view, x);
          if (
            !moveEvent.altKey &&
            frame !== undefined &&
            values !== undefined
          ) {
            const radius =
              (snapRadius / point.width) * (this.view.end - this.view.start);
            position = snap(
              position,
              [
                ...zeroCrossings(frame, values),
                ...extremePositions(frame, values),
              ],
              radius,
            );
          }
          this.timeCursors = { ...this.timeCursors, [key]: position };
          this.placeCursors();
        });
      });
      // A double-click on a cursor must not reset the zoom.
      line.addEventListener("dblclick", (event) => {
        event.stopPropagation();
      });
    }
    const levelLines = element("level-cursor-lines");
    levelLines.addEventListener("pointerdown", (event) => {
      const line = (event.target as Element).closest<HTMLElement>(
        ".level-cursor",
      );
      if (line === null) {
        return;
      }
      event.stopPropagation();
      const laneIndex = Number(line.dataset["lane"]);
      const key = line.dataset["key"] === "first" ? "first" : "second";
      followDrag(line, event, (moveEvent) => {
        const lane = this.screenLaneList()[laneIndex];
        if (this.levelCursors === undefined || lane === undefined) {
          return;
        }
        // In px at clientHeight scale, as the line is drawn.
        const height = this.screen.clientHeight;
        const center = ((1 - lane.center) / 2) * height;
        const halfHeight = (lane.halfHeight / 2) * height;
        const scale = fullScale(this.store.number("verticalScale"));
        const relative = Math.max(
          -1,
          Math.min(1, (center - this.screenPoint(moveEvent).top) / halfHeight),
        );
        let level = relative * scale;
        const values = this.snapValues(laneIndex);
        if (!moveEvent.altKey && values !== undefined) {
          level = snap(
            level,
            levelTargets(values),
            (snapRadius / halfHeight) * scale,
          );
        }
        this.levelCursors = { ...this.levelCursors, [key]: level };
        this.placeCursors();
      });
    });
    levelLines.addEventListener("dblclick", (event) => {
      event.stopPropagation();
    });
    const marker = element("trigger-marker");
    marker.addEventListener("pointerdown", (event) => {
      event.stopPropagation();
      followDrag(marker, event, (moveEvent) => {
        const { center, halfHeight } = this.markerGeometry();
        const relative =
          (center - this.screenPoint(moveEvent).top) / halfHeight;
        const level = relative * fullScale(this.store.number("verticalScale"));
        this.request(
          "triggerLevel",
          Number(clampTo("triggerLevel", level).toFixed(3)),
        );
      });
    });

    // View zoom: ⌘ + scroll or pinch zooms around the pointer, a horizontal
    // swipe pans, dragging zooms to the selection, double-click resets.
    const screenPosition = (clientX: number) => {
      return Math.min(
        1,
        Math.max(0, this.screenPoint({ clientX, clientY: 0 }).x),
      );
    };
    this.screen.addEventListener(
      "wheel",
      (event) => {
        // In pixels, so lines and pages zoom and pan as far as pixels do.
        const unit = wheelUnit(event, this.screen.clientHeight);
        const deltaX = event.deltaX * unit;
        const deltaY = event.deltaY * unit;
        if (event.metaKey || event.ctrlKey) {
          // Trackpad pinches arrive as wheel events with ctrlKey set (on a Mac, where the zoom
          // key is ⌘). A mouse wheel's notch on its own zooms one fixed step on every system
          // (WebKit reports a few pixels per notch, Windows 100), as it steps a value field;
          // events in a stream (a trackpad, a pinch, a wheel spun fast) zoom by their distance.
          event.preventDefault();
          const pinch = isMac && event.ctrlKey && !event.metaKey;
          const alone = event.timeStamp - this.lastZoomWheel > streamGap;
          this.lastZoomWheel = event.timeStamp;
          const delta =
            alone && !pinch
              ? Math.sign(deltaY) * maxWheelZoomDelta
              : Math.max(
                  -maxWheelZoomDelta,
                  Math.min(maxWheelZoomDelta, deltaY),
                );
          const factor = Math.exp(-delta * wheelZoomRate);
          this.setView(
            zoomAround(
              this.view,
              screenPosition(event.clientX),
              factor,
              this.minimumViewWidth(),
            ),
          );
        } else if (Math.abs(deltaX) > Math.abs(deltaY)) {
          event.preventDefault();
          this.swipe(deltaX / this.screen.clientWidth);
        }
      },
      { passive: false },
    );
    // WebKit reports trackpad pinches as gesture events.
    let gestureScale = 1;
    let gestureAnchor = 0.5;
    this.screen.addEventListener("gesturestart", (event) => {
      event.preventDefault();
      gestureScale = 1;
      gestureAnchor = screenPosition((event as GestureEvent).clientX);
    });
    this.screen.addEventListener("gesturechange", (event) => {
      event.preventDefault();
      const scale = (event as GestureEvent).scale;
      this.setView(
        zoomAround(
          this.view,
          gestureAnchor,
          scale / gestureScale,
          this.minimumViewWidth(),
        ),
      );
      gestureScale = scale;
    });

    const selection = element("selection");
    this.screen.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) {
        return;
      }
      const from = screenPosition(event.clientX);
      let to = from;
      const move = (moveEvent: PointerEvent) => {
        to = screenPosition(moveEvent.clientX);
        const dragged =
          Math.abs(to - from) * this.screen.clientWidth >=
          minimumSelectionPixels;
        selection.hidden = !dragged;
        selection.style.left = `${String(Math.min(from, to) * 100)}%`;
        selection.style.width = `${String(Math.abs(to - from) * 100)}%`;
      };
      followDrag(this.screen, event, move, (released) => {
        if (released && !selection.hidden) {
          this.setView(zoomTo(this.view, from, to, this.minimumViewWidth()));
        }
        selection.hidden = true;
      });
    });
    this.screen.addEventListener("dblclick", () => {
      this.setView(fullView);
    });
  }

  // The narrowest view of the span the last frame showed, which the engine reports in samples.
  private minimumViewWidth(): number {
    const frame = this.lastFrame;
    if (frame === undefined || frame.viewEnd <= frame.viewStart) {
      return minimumWidth(0);
    }
    return minimumWidth(
      frame.visibleSamples / (frame.viewEnd - frame.viewStart),
    );
  }

  // A horizontal swipe of `screenWidths`: pans the zoomed view, or, frozen and not zoomed, pans
  // back through the held data. A frozen sweep keeps its cycle.
  private swipe(screenWidths: number): void {
    if (isZoomed(this.view)) {
      this.setView(panBy(this.view, screenWidths));
    } else if (this.lastFrame?.frozen === true && !this.lastFrame.synced) {
      // No further back than the engine says this frozen view reaches.
      const span = this.secondsSpan();
      this.panSeconds = clampPan(
        this.panSeconds - screenWidths * span,
        this.lastFrame.panReach,
      );
      this.backend.pan(this.panSeconds);
    }
  }

  private setView(view: ViewWindow): void {
    this.view = view;
    // The engine renders the visible part at full resolution; until that frame arrives, the
    // last one is stretched into place.
    this.backend.setView(view.start, view.end);
    this.drawScreen();
    this.loop.invalidate();
    if (this.lastFrame !== undefined) {
      this.showSweep(this.lastFrame);
    }
    this.placeCursors();
    this.updateStatus();
    this.queueTraceView();
  }

  // Sends the trace view at the next animation frame if it changed. Native mode only.
  private queueTraceView(): void {
    if (!this.nativeTraces || this.traceViewQueued) {
      return;
    }
    this.traceViewQueued = true;
    requestAnimationFrame(() => {
      this.traceViewQueued = false;
      const message = this.traceViewMessage();
      const text = JSON.stringify(message);
      if (text !== this.lastTraceView) {
        this.lastTraceView = text;
        this.backend.traceView(message);
      }
    });
  }

  // Everything the editor reads besides the frame to draw the traces as this page would.
  private traceViewMessage(): TraceViewMessage {
    const style = this.traceStyle;
    const colour = (value: number) => rgba(cssColour(value));
    const views = style?.colors() ?? new Map<ChannelView, Rgba>();
    const sweep = style?.sweepColors();
    const white: Rgba = [1, 1, 1, 1];
    const layout = this.layout();
    return {
      type: "traceView",
      viewStart: this.view.start,
      viewEnd: this.view.end,
      layout:
        (Object.keys(ViewLayout) as (keyof typeof ViewLayout)[]).find(
          (name) => ViewLayout[name] === layout,
        ) ?? "overlaid",
      traceShape:
        (Object.keys(TraceShape) as (keyof typeof TraceShape)[]).find(
          (name) => TraceShape[name] === this.traceShape(),
        ) ?? "line",
      verticalScaleDb: this.store.number("verticalScale"),
      dim: this.waiting,
      highlight: this.highlightNamed(),
      lineWidth: style?.lineWidthPixels ?? 1,
      emptyFrameLanes: this.emptyFrameLanes(),
      viewColours: viewOrder.map(
        (name) => views.get(ChannelView[name]) ?? white,
      ),
      laneSets: [...this.laneSets].map(([laneSet, lanes]) => ({
        laneSet,
        lanes: lanes.map((lane) => ({
          id: lane.id,
          colour: colour(lane.colour),
          own: lane.own,
        })),
      })),
      fallbackColours: fallbackPalette.map(colour),
      sumColour: colour(sumLaneColour),
      screen: rgba(token("--sl-screen")),
      grid: rgba(token("--sl-grid")),
      axis: rgba(token("--sl-grid-strong")),
      cursor: sweep?.cursor ?? white,
      cancellation: style?.cancellationColor() ?? white,
      gained: style?.differenceColors().gained ?? white,
      lost: style?.differenceColors().lost ?? white,
      gridChoice: this.gridChoice(),
    };
  }

  // Native drawing stopped for good: what to do, unless an engine error outranks it.
  private showNativeGraphicsLost(): void {
    if (this.engineError) {
      return;
    }
    element("message-title").textContent = "Graphics unavailable";
    element("message-text").textContent = graphicsMessages.lost;
    element("message").hidden = false;
  }

  // ----- Engine events -----

  private listenToBackend(): void {
    this.backend.on("frame", (frame) => {
      const wasFree = this.freeRunning();
      const hadTempo = this.lastFrame?.noTempo !== true;
      this.lastFrame = frame;
      // The host's tempo arriving, or not yet: the grid and labels follow.
      if (frame.noTempo === hadTempo) {
        this.drawScreen();
      }
      // A limited span labels the axis anew; a change below what the labels show does not.
      const span = this.secondsSpan();
      if (
        frame.freeRunning !== wasFree ||
        Math.abs(span - this.labelledSpan) > 1e-3 * this.labelledSpan ||
        this.triggerPointMoved()
      ) {
        this.placeLabels();
        this.placeTriggerMarker();
      }
      this.lastFrameTime = performance.now();
      this.debug.lastFrame = frame;
      this.debug.frameCount++;
      if (this.waiting) {
        this.waiting = false;
        // A frame ends the wait: an editor drawing the traces stops dimming them too.
        this.queueTraceView();
      }
      // Only changes touch the page: each one costs the page work to restyle.
      if (frame.frozen !== this.shownFrozen) {
        this.shownFrozen = frame.frozen;
        element("freeze").setAttribute("aria-pressed", String(frame.frozen));
        this.freezeLabel?.show(frame.frozen ? "frozen" : "live");
      }
      // The engine may resume on its own (auto-freeze); a new freeze starts unpanned.
      if (!frame.frozen) {
        this.panSeconds = 0;
      } else if (!frame.synced) {
        // The reach shrinks when the span grows: the pan, and its chip, follow it.
        const pan = clampPan(this.panSeconds, frame.panReach);
        if (pan !== this.panSeconds) {
          this.panSeconds = pan;
          this.backend.pan(pan);
        }
      }
      this.loop.submit(frame);
      this.updateViewSummary();
      this.updateReadoutChoices();
      this.updateSourceChoices();
      // Which views a Single frame draws, as bits: as many of other views changes their labels.
      let viewsDrawn = 0;
      for (const view of frame.views.keys()) {
        viewsDrawn |= 1 << view;
      }
      // A track joined or left, or the views drawn changed (a track turned mono): lanes, grid
      // and labels change with it. A new lane set (a track checked, a renamed track) changes the
      // labels.
      if (this.laneCount() !== this.drawnLaneCount) {
        this.drawScreen();
        this.placeCursors();
      } else if (
        frame.laneSet !== this.drawnLaneSet ||
        frame.comparison?.lane !== this.drawnCompareLane ||
        viewsDrawn !== this.drawnViewSet
      ) {
        this.placeLaneLabels();
        this.placeTriggerMarker();
      }
      this.drawnViewSet = viewsDrawn;
      this.drawnLaneSet = frame.laneSet;
      this.drawnCompareLane = frame.comparison?.lane;
      this.statusDue();
    });
    this.backend.on("error", (message) => {
      this.showError(message);
    });
    this.backend.on("tracks", (message) => {
      this.tracks = message.tracks;
      this.styleLanes();
      this.updateTrackList();
      this.updateGroupSummary();
      this.drawScreen();
      this.loop.invalidate();
      this.queueTraceView();
    });
    this.backend.on("lanes", (message) => {
      this.laneLabelSets.set(message.laneSet, message.lanes);
      for (const old of [...this.laneLabelSets.keys()]) {
        if (old < message.laneSet - 3) {
          this.laneLabelSets.delete(old);
          this.laneSets.delete(old);
        }
      }
      this.styleLanes();
      this.latestLaneSet = message.laneSet;
      this.drawScreen();
      this.loop.invalidate();
      this.queueTraceView();
      this.updateStatus();
    });
    this.backend.on("snapshot", (message) => {
      if (message.nativeTraces === true) {
        this.nativeTraces = true;
      }
      document.documentElement.classList.toggle(
        "native-traces",
        this.nativeTraces,
      );
      if (message.frameBarcode === true) {
        this.frameBarcode = true;
      }
      // The strip takes the corner the cursor readout uses.
      document.documentElement.classList.toggle(
        "frame-barcode",
        this.frameBarcode,
      );
      this.engineError = false;
      element("message").hidden = !this.graphicsMessage;
      this.inputChannels = message.inputChannels;
      this.updateReadoutChoices();
      this.updateSourceChoices();
      this.updateViewList();
      this.showSetting("channelViews", this.store.get("channelViews"));
      this.drawScreen();
      this.placeTriggerMarker();
      // Chosen at the first snapshot, never again: the editor decides before it, for the session.
      if (this.makeRenderer !== undefined) {
        if (!this.nativeTraces) {
          this.makeRenderer();
        }
        this.makeRenderer = undefined;
        this.loop.invalidate();
      }
      if (message.nativeGraphicsLost === true) {
        this.nativeGraphicsLost = true;
      }
      if (this.nativeGraphicsLost) {
        this.showNativeGraphicsLost();
      }
      this.queueTraceView();
    });
    // What the editor says of itself: shown as it says it, never worked out by the page.
    this.backend.on("about", (message) => {
      element("about-version").textContent = message.version;
      element("contact-row").hidden = !message.contact;
    });
    this.backend.on("nativeGraphicsLost", () => {
      this.nativeGraphicsLost = true;
      this.showNativeGraphicsLost();
    });
  }

  private statusDue(): void {
    const now = performance.now();
    if (now < this.nextReadoutTime - 1) {
      return;
    }
    // On schedule; more than a whole interval late, restarted from now.
    this.nextReadoutTime =
      now - this.nextReadoutTime < readoutInterval
        ? this.nextReadoutTime + readoutInterval
        : now + readoutInterval;
    this.updateStatus();
  }

  private updateStatus(): void {
    const now = performance.now();
    const sinceFrame = (now - this.lastFrameTime) / 1000;
    const mode = this.store.get("triggerMode") as string;
    const chips = statusChips(
      this.lastFrame,
      mode,
      sinceFrame,
      this.panSeconds,
    );
    const waiting = chips.some((chip) => chip.text === "Waiting");
    if (waiting !== this.waiting) {
      this.waiting = waiting;
      this.loop.invalidate();
      this.queueTraceView();
    }
    if (isZoomed(this.view)) {
      chips.push({ text: `Zoom \u00d7${zoomFactor(this.view).toFixed(1)}` });
    }
    const shown = JSON.stringify(chips);
    if (shown !== this.shownChips) {
      this.shownChips = shown;
      this.showChips(chips);
    }
    const frame = this.lastFrame;
    if (frame !== undefined) {
      // Taken on the readout schedule whether or not the Meter shows, so it shows the levels at
      // once; the engine measures its other rows from the frame after it is switched on.
      this.meterValues = {
        values: meterValuesOf(frame),
        source: meterSource(frame),
      };
    }
    this.updateCursorReadout();
    this.updateHover(); // Live data changes under a resting pointer.
    if (frame !== undefined) {
      this.updateMatches(frame);
    }
  }

  // The state chips, in the status bar's fixed width. Those that don't fit fold, from the last,
  // into one "+n" chip that lists them on hover or focus; the first always stays shown. Widths
  // are read once per change of the chips, and the fold is worked out from them.
  private showChips(chips: readonly { text: string }[]): void {
    const host = element("chips");
    const node = (chip: { text: string }) => {
      const span = document.createElement("span");
      // Orange marks Frozen alone; yellow, a problem in the audio.
      const kind =
        chip.text === "Frozen"
          ? "frozen"
          : chip.text === "Invalid samples"
            ? "problem"
            : "";
      span.className = `chip ${kind}`.trim();
      span.textContent = chip.text;
      return span;
    };
    const nodes = chips.map(node);
    const more = document.createElement("button");
    more.className = "chip-more";
    more.textContent = "+9";
    host.replaceChildren(...nodes, more);
    const gap = parseFloat(getComputedStyle(host).columnGap) || 0;
    const widths = nodes.map((chip) => chip.getBoundingClientRect().width);
    const moreWidth = more.getBoundingClientRect().width;
    const shown = shownChipCount(widths, gap, moreWidth, host.clientWidth);
    if (shown === nodes.length) {
      more.remove();
      return;
    }
    const folded = chips.slice(shown);
    const list = document.createElement("span");
    list.className = "chip-list";
    list.replaceChildren(...folded.map(node));
    more.replaceChildren(`+${String(folded.length)}`, list);
    more.setAttribute(
      "aria-label",
      `${String(folded.length)} more: ${folded.map((chip) => chip.text).join(", ")}`,
    );
    host.replaceChildren(...nodes.slice(0, shown), more);
  }

  // The renderer, or undefined with the message shown when the web view gives the page no
  // WebGL2 or the shaders do not build.
  private createRenderer(
    style: TraceStyle,
    view: () => TraceView,
  ): TraceRenderer | undefined {
    try {
      return new TraceRenderer(this.traceCanvas, style, view);
    } catch (error) {
      if (error instanceof TraceMemoryError) {
        this.showGraphicsMessage(graphicsMessages.memory);
        return undefined;
      }
      this.showGraphicsMessage(graphicsMessages.noWebGL2);
      return undefined;
    }
  }

  // Shows the graphics message, unless an engine error is showing, which outranks it (see
  // engineError): every way the graphics fail, at start, when lost, or rebuilt in vain, comes here.
  private showGraphicsMessage(text: string): void {
    if (this.engineError) {
      return;
    }
    element("message-title").textContent = "Graphics unavailable";
    element("message-text").textContent = text;
    element("message").hidden = false;
    this.graphicsMessage = true;
  }

  private showError(message: ErrorMessage): void {
    this.graphicsMessage = false;
    this.engineError = true;
    const texts: Record<string, [string, string]> = {
      protocolMismatch: [
        "Version mismatch",
        `${message.message} Reinstall the plugin so both parts come from the same release.`,
      ],
      engineUnavailable: [
        "Display unavailable",
        "The editor cannot reach the plugin's signal in this host. Audio still passes through unchanged.",
      ],
    };
    const [title, text] = texts[message.code] ?? [
      "Display error",
      message.message,
    ];
    element("message-title").textContent = title;
    element("message-text").textContent = text;
    element("message").hidden = false;
  }
}
