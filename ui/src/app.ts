// The oscilloscope UI: wires the controls, screen and readouts to the backend and
// the settings store. Controls show only engine-reported values and send requests.
import { ChannelView, ViewLayout } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  minimumEditorWidth,
  readoutIntervalSeconds,
  summingMarginDb,
} from "../../protocol/src/limits.ts";
import type {
  ErrorMessage,
  SettingValue,
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
import { DragField } from "./components/drag-field.ts";
import { numberFields, type NumberField } from "./view/fields.ts";
import { HelpTips } from "./components/help-tips.ts";
import { Segmented } from "./components/segmented.ts";
import { streamGap } from "./components/wheel-steps.ts";
import { RenderLoop } from "./render/render-loop.ts";
import { fullScale, lanes } from "./render/trace-geometry.ts";
import {
  TraceRenderer,
  type Rgba,
  type TraceStyle,
  type TraceView,
} from "./render/trace-renderer.ts";
import type { SettingsStore } from "./store/settings-store.ts";
import {
  amplitudeLabels,
  timeLabels,
  triggerPosition,
} from "./view/axis-labels.ts";
import {
  formatDbfs,
  formatGain,
  formatOffset,
  formatPitch,
  formatFrequency,
  parseQuantity,
  signed,
} from "./view/format.ts";
import {
  cursorReadout,
  initialCursors,
  initialLevels,
  levelReadout,
  measure,
  type CursorPair,
  type LevelPair,
} from "./view/cursors.ts";
import { drawGrid } from "./view/grid.ts";
import {
  hoverReadout,
  traceAt,
  type HoverContext,
  type HoverTrace,
} from "./view/hover.ts";
import {
  cssColour,
  laneStyles,
  sumLaneColour,
  type LaneStyle,
} from "./view/lanes.ts";
import { isMac, type HelpTopic } from "./view/help.ts";
import {
  extremePositions,
  levelTargets,
  snap,
  zeroCrossings,
} from "./view/snap.ts";
import { beatSpanLabels, musicalGrid } from "./view/musical-grid.ts";
import { laneHoldsInvalid, statusChips } from "./view/status.ts";
import {
  fullView,
  isZoomed,
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
  readoutView,
  viewOrder,
  viewToggleState,
  type ViewName,
} from "./view/screen-state.ts";

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
const viewNames: Record<ChannelView, string> = {
  [ChannelView.left]: "Left",
  [ChannelView.right]: "Right",
  [ChannelView.mid]: "Mid",
  [ChannelView.side]: "Side",
};
const viewColorTokens: Record<ViewName, string> = {
  left: "--left",
  right: "--right",
  mid: "--mid",
  side: "--side",
};
// How fast ⌘ + scroll and pinch zoom, per wheel delta unit.
const wheelZoomRate = 0.01;
// The most one wheel event zooms by, in wheel delta units: about 1.3×, which is also what a lone
// mouse-wheel notch zooms by on every system. A trackpad's events are a few units each.
const maxWheelZoomDelta = 25;
// A wheel line in pixels, as browsers scroll it.
const wheelLinePixels = 40;
// The Compare view's sum lane: an id no track has, and a neutral colour.
const sumLaneId = -100;
// Cursors snap to a target within this distance (CSS px) unless Option is held.
const snapRadius = 10;
// A drag shorter than this (CSS px) is a click, not a zoom selection.
const minimumSelectionPixels = 4;

// WebKit's trackpad pinch event; not in the standard DOM types.
interface GestureEvent extends UIEvent {
  readonly scale: number;
  readonly clientX: number;
}

// Room taken by the amplitude labels at the left edge (CSS px).
const amplitudeLabelWidth = 56;
const readoutInterval = readoutIntervalSeconds * 1000; // ms, as the engine measures them.
// 60 engine frames a second on any display rate (at most the display's): more would cost work
// in every part of the pipeline for motion the eye barely tells apart.
const frameInterval = 1000 / 60;
// Group titles that open a popover: the group, its title button and the popover.
const popovers = [
  ["trigger-group", "trigger-details", "trigger-popover"],
  ["display-group", "display-details", "display-popover"],
] as const;
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

function range(name: SettingName): NumberSetting {
  return settingOf(name) as NumberSetting;
}

function clampTo(name: SettingName, value: number): number {
  const { minimum, maximum } = range(name);
  return Math.min(maximum, Math.max(minimum, value));
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
  private graphicsMessage = false; // Whether the message shown is about the graphics.
  // Whether an engine error is shown. Until a snapshot clears it, it outranks the graphics message:
  // it says why the screen is empty and what to do, and the engine does not send it again.
  private engineError = false;
  private readonly loop: RenderLoop;
  private lastFrame: Frame | undefined;
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
  private readonly viewToggles = new Map<ViewName, HTMLButtonElement>();
  private beatSpanField: DragField | undefined;
  private beatSpanNames: string[] = [];
  private cycleKey = "";
  private view: ViewWindow = fullView;
  // Time and level cursors, while shown; temporary like the view.
  private timeCursors: CursorPair | undefined;
  private shownFrozen: boolean | undefined;
  // The pointer over the traces, as fractions of the screen, for the hover readout.
  private hover: { x: number; y: number } | undefined;
  private levelCursors: LevelPair | undefined;
  // The Compare view: the fix preview, and the lane labels' readouts of each compared track (by
  // lane index) and of the sum, updated in place.
  private previewFix = false;
  private matchNodes: (HTMLElement | undefined)[] = [];
  // Each lane's name, marked while the lane shows samples that are not finite.
  private laneNameNodes: HTMLElement[] = [];
  private sumNode: HTMLElement | undefined;
  private laneLabelsKey = "";
  // Multi-track lanes by lane set, as the lanes messages describe them; a few are kept, so a
  // frame drawn just before or after a change finds its own.
  private readonly laneSets = new Map<number, LaneStyle[]>();
  private latestLaneSet = 0;
  private hiddenTracks = 0;
  private drawnLaneCount = 0;
  private drawnLaneSet = 0;

  constructor(backend: Backend, store: SettingsStore, debug: DebugState) {
    this.backend = backend;
    this.store = store;
    this.debug = debug;
    document.documentElement.style.setProperty(
      "--minimum-width",
      `${String(minimumEditorWidth)}px`,
    );
    // Resolved on first use: in development the stylesheet may apply after start-up.
    let colors: Map<ChannelView, Rgba> | undefined;
    const resolveColors = (): ReadonlyMap<ChannelView, Rgba> => {
      if (colors === undefined && token("--left") !== "") {
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
      // Tracks left out of a comparison are drawn faint.
      laneColors: () => {
        const comparing = this.lastFrame?.comparison !== undefined;
        return this.lanesOf(this.lastFrame).map((lane) => {
          const [r, g, b, a] = rgba(cssColour(lane.colour));
          const faint = comparing && !lane.compared && lane.id !== sumLaneId;
          return [r, g, b, faint ? a * 0.2 : a] as const;
        });
      },
      comparedLanes: () =>
        this.lanesOf(this.lastFrame).map((lane) => lane.compared),
      sweepColors: () => {
        const [r, g, b] = rgba(token("--screen"));
        return { cursor: rgba(token("--ink")), previous: [r, g, b, 0.5] };
      },
      lineWidthPixels: 1.5 * window.devicePixelRatio,
    };
    const traceView = (): TraceView => ({
      verticalScaleDb: store.number("verticalScale"),
      layout: this.layout(),
      dim: this.waiting,
      view: this.view,
    });
    this.renderer = this.createRenderer(style, traceView);
    // A lost context comes back only if the page says it will rebuild what it draws with (the
    // WebGL specification, "The Context Lost Event"); until then nothing is drawn.
    this.traceCanvas.addEventListener("webglcontextlost", (event) => {
      event.preventDefault();
      this.renderer = undefined;
      if (!this.engineError) {
        this.showGraphicsMessage(
          "The system's graphics stopped drawing the traces. They return when it recovers.",
        );
      }
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
        draw: (frame) => {
          this.renderer?.draw(frame);
          this.showSweep(frame);
        },
      },
      () => {
        backend.requestFrame();
      },
      frameInterval,
    );
    this.buildControls();
    this.listenToScreen();
    this.listenToBackend();
    store.subscribe((name, value) => {
      this.showSetting(name, value);
    });
    for (const name of settingNames) {
      this.showSetting(name, store.get(name));
    }
    window.addEventListener("resize", () => {
      this.resize();
    });
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

  // The views asked for, as the toggles show them.
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
      parse: (text) => parseQuantity(text, spec.quantity, spec.bareScale),
      step: (value, delta, fine) =>
        clampTo(name, spec.step(value, delta, fine)),
      onRequest: (value) => {
        this.request(name, clampTo(name, value));
      },
    });
    element(spec.host).append(field.element);
    this.fields.set(name, field);
  }

  private buildControls(): void {
    // Tempo sync: the beat span steps through its named values.
    const beatSpanNames = valueNames("BeatSpan");
    const beatSpanField = new DragField({
      label: "Beat span",
      format: (index) => beatSpanLabels[index] ?? "",
      parse: (text) => {
        const index = beatSpanLabels.findIndex(
          (label) => label === text.trim().toLowerCase(),
        );
        return index < 0 ? undefined : index;
      },
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
    this.beatSpanField = beatSpanField;
    this.beatSpanNames = beatSpanNames;

    for (const [name, field] of Object.entries(numberFields)) {
      this.numberField(name as SettingName, field);
    }

    const views = element("views");
    for (const name of viewOrder) {
      const button = document.createElement("button");
      button.className = "toggle";
      button.textContent = viewLabels[name];
      button.dataset["help"] = viewHelp[name];
      button.style.setProperty("--color", `var(${viewColorTokens[name]})`);
      button.setAttribute("aria-pressed", "false");
      button.addEventListener("click", () => {
        const shown = new Set(this.shownViews());
        if (shown.has(name)) {
          shown.delete(name);
        } else {
          shown.add(name);
        }
        if (shown.size > 0) {
          this.request(
            "channelViews",
            viewOrder.filter((view) => shown.has(view)),
          );
        }
      });
      views.append(button);
      this.viewToggles.set(name, button);
    }

    const segment = (
      name: SettingName,
      host: string,
      choices: [
        value: string,
        label: string,
        help?: HelpTopic | undefined,
        badge?: string,
      ][],
    ) => {
      this.segmented.set(
        name,
        new Segmented(
          element(host),
          choices.map(([value, label, help, badge]) => ({
            value,
            label,
            ...(help === undefined ? {} : { help }),
            ...(badge === undefined ? {} : { badge }),
          })),
          (value) => {
            this.request(name, value);
          },
        ),
      );
    };
    segment("displayMode", "display-mode", [
      ["single", "Single"],
      ["multi", "Multi"],
      ["compare", "Compare", undefined, "beta"],
    ]);
    segment("group", "group", [
      ["a", "A"],
      ["b", "B"],
      ["c", "C"],
      ["d", "D"],
    ]);
    segment("timeBase", "time-base", [
      ["beats", "Beats"],
      ["seconds", "Sec"],
    ]);
    segment("viewLayout", "layout", [
      ["overlaid", "Overlay"],
      ["stacked", "Stack"],
    ]);
    segment("triggerMode", "mode", [
      ["auto", "Auto", "modeAuto"],
      ["normal", "Normal", "modeNormal"],
      ["free", "Free", "modeFree"],
      ["single", "Single", "modeSingle"],
    ]);
    segment(
      "triggerSource",
      "source",
      viewOrder.map((name) => [name, viewLabels[name]]),
    );
    segment("triggerSlope", "slope", [
      ["rising", "Rising"],
      ["falling", "Falling"],
    ]);

    // A group's title opens its popover of further settings; a press elsewhere closes it.
    for (const [group, button, id] of popovers) {
      const details = element(button);
      const popover = element(id);
      details.addEventListener("click", () => {
        popover.hidden = !popover.hidden;
        details.setAttribute("aria-expanded", String(!popover.hidden));
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

    element("freeze").addEventListener("click", () => {
      const frozen = this.lastFrame?.frozen !== true;
      this.panSeconds = 0;
      this.backend.freeze(frozen);
    });
    const help = new HelpTips(element("help-tip"));
    element("help").addEventListener("click", () => {
      const on = element("help").getAttribute("aria-pressed") !== "true";
      element("help").setAttribute("aria-pressed", String(on));
      help.setEnabled(on);
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

  // A tempo-synced sweep is not triggered: in Beats mode the trigger controls are disabled, and
  // the popover closes. Re-arm applies to Single mode only.
  // Multi mode shows each track's Mid: the channel views and the trigger source do not apply.
  private updateMultiControls(): void {
    const multi = this.multiMode();
    for (const id of ["views", "source"]) {
      element(id).classList.toggle("inactive", multi);
    }
    for (const button of element("source").querySelectorAll("button")) {
      button.disabled = multi;
    }
    this.updateViewToggles();
    this.updateStatus();
  }

  // The view toggles: pressed as asked for; disabled in Multi and Compare, which show each
  // track's Mid, and for R, M and S on a mono track, which would only repeat L. The one place
  // that decides, for every case.
  private updateViewToggles(): void {
    const shown = this.shownViews();
    const multi = this.multiMode();
    for (const [view, button] of this.viewToggles) {
      const state = viewToggleState(view, shown, multi, this.inputChannels);
      button.setAttribute("aria-pressed", String(state.pressed));
      button.disabled = state.disabled;
    }
  }

  private updateTriggerControls(): void {
    const beats = this.beatsMode();
    const group = element("trigger-group");
    group.classList.toggle("inactive", beats);
    for (const button of group.querySelectorAll("button")) {
      button.disabled =
        beats ||
        (button.id === "arm" && this.store.get("triggerMode") !== "single");
    }
    if (beats) {
      element("trigger-popover").hidden = true;
      element("trigger-details").setAttribute("aria-expanded", "false");
    }
  }

  // Opens the trigger popover above the Trigger group, aligned with its left edge, kept inside
  // the window.
  // Above its group, left-aligned with it as far as the strip allows.
  private placePopover(id: string, groupId: string): void {
    const popover = element(id);
    if (popover.hidden) {
      return;
    }
    const group = element(groupId);
    const strip = popover.offsetParent?.clientWidth ?? window.innerWidth;
    const margin = 8;
    const left = Math.min(
      group.offsetLeft,
      strip - popover.offsetWidth - margin,
    );
    popover.style.left = `${String(Math.max(margin, left))}px`;
  }

  private showSetting(name: SettingName, value: SettingValue): void {
    if (typeof value === "number") {
      this.fields.get(name)?.show(value);
    } else if (typeof value === "string") {
      this.segmented.get(name)?.show(value);
    }
    if (name === "channelViews") {
      this.updateViewToggles();
    }
    if (name === "triggerMode" || name === "timeBase") {
      this.updateTriggerControls();
    }
    if (name === "displayMode") {
      this.updateMultiControls();
      if (value !== "compare" && this.previewFix) {
        this.previewFix = false;
        this.backend.previewFix(false);
      }
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
    if (name === "autoFreeze") {
      element("auto-freeze").setAttribute(
        "aria-pressed",
        String(value === "on"),
      );
    }
    if (name === "beatSpan" && typeof value === "string") {
      this.beatSpanField?.show(this.beatSpanNames.indexOf(value));
    }
    if (name === "timeBase") {
      const beats = value === "beats";
      element("span-field").hidden = beats;
      element("beat-span-field").hidden = !beats;
    }
    this.updateTriggerSummary();
    if (name === "concertPitch" && typeof value === "number") {
      element("display-summary").textContent = `A4 ${formatPitch(value)}`;
      this.updateCursorReadout();
    }
    // Zoom is a temporary view of one span; a new span starts unzoomed.
    if (
      (name === "timeSpan" || name === "timeBase" || name === "beatSpan") &&
      isZoomed(this.view)
    ) {
      this.setView(fullView);
    }
    // Settings that move the grid, labels or trigger line; the engine's next frame follows.
    const screenSettings: SettingName[] = [
      "verticalScale",
      "viewLayout",
      "channelViews",
      "timeSpan",
      "triggerMode",
      "timeBase",
    ];
    if (screenSettings.includes(name)) {
      this.drawScreen();
      this.loop.invalidate();
    }
    this.placeTriggerMarker();
  }

  private updateTriggerSummary(): void {
    const source = viewLabels[this.store.get("triggerSource") as ViewName];
    const slope = this.store.get("triggerSlope") === "rising" ? "↑" : "↓";
    element("trigger-summary").textContent =
      `${source} ${slope} ${signed(this.store.number("triggerLevel"), 2)}`;
  }

  // ----- Screen -----

  private resize(): void {
    for (const [group, , id] of popovers) {
      this.placePopover(id, group);
    }
    this.placeCursors();
    const ratio = window.devicePixelRatio;
    for (const canvas of [this.gridCanvas, this.traceCanvas]) {
      canvas.width = Math.round(this.screen.clientWidth * ratio);
      canvas.height = Math.round(this.screen.clientHeight * ratio);
    }
    this.backend.setViewport(this.screen.clientWidth, ratio);
    this.drawScreen();
    this.loop.invalidate();
  }

  private laneCount(): number {
    if (this.layout() !== ViewLayout.stacked) {
      return 1;
    }
    const lanes = this.lastFrame?.lanes.length ?? 0;
    return Math.max(1, lanes > 0 ? lanes : this.drawnViews().length);
  }

  // Multi and Compare both show lanes of the group's tracks.
  private multiMode(): boolean {
    return this.store.get("displayMode") !== "single";
  }

  private compareMode(): boolean {
    return this.store.get("displayMode") === "compare";
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
    // A Compare frame's last lane is the sum of the compared tracks.
    if (frame?.comparison !== undefined && styles.length > 0) {
      styles = [
        ...styles,
        {
          id: sumLaneId,
          name: "Sum",
          colour: sumLaneColour,
          own: false,
          compared: false,
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
            compared: false,
          })),
        );
  }

  // Lane names: at the top of each stacked lane, or as a legend when overlaid, after the top
  // level label.
  private placeLaneLabels(): void {
    const container = element("lane-labels");
    const frame = this.lastFrame;
    const styles = this.lanesOf(frame);
    const height = this.screen.clientHeight;
    const comparing = frame?.comparison !== undefined;
    // Rebuilt only when what they show changes, so a click on a label is never lost to a redraw.
    const key = JSON.stringify([
      styles,
      comparing,
      this.previewFix,
      this.layout(),
      height,
      this.compareMode() && frame !== undefined,
      this.store.get("group"),
    ]);
    if (key === this.laneLabelsKey) {
      return;
    }
    this.laneLabelsKey = key;
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
    // In the Compare view, each track's swatch is its check box, followed by how it lines up with
    // this track when compared; the sum shows its gain and the fix preview.
    const compareMode = this.compareMode();
    this.matchNodes = [];
    this.sumNode = undefined;
    this.laneNameNodes = [];
    const name = (style: LaneStyle, index: number) => {
      const node = document.createElement("span");
      this.laneNameNodes[index] = node;
      node.style.setProperty("--swatch", cssColour(style.colour));
      node.classList.toggle("own", style.own);
      if (style.id === sumLaneId) {
        const gain = document.createElement("span");
        gain.className = "lane-readout";
        gain.dataset["help"] = "sumGain";
        this.sumNode = gain;
        const preview = button("Preview fix", "previewFix", () => {
          this.previewFix = !this.previewFix;
          this.backend.previewFix(this.previewFix);
          this.placeLaneLabels();
        });
        preview.className = "preview-fix";
        preview.setAttribute("aria-pressed", String(this.previewFix));
        node.append(style.name, gain, preview);
        return node;
      }
      if (!compareMode) {
        node.textContent = style.name;
        return node;
      }
      node.classList.add("checkable");
      const check = button(style.name, "compareTrack", () => {
        this.toggleCompared(style.id);
      });
      check.className = "compare-check";
      check.setAttribute("role", "checkbox");
      check.setAttribute("aria-checked", String(style.compared));
      check.disabled = style.own;
      node.append(check);
      if (comparing && style.compared && !style.own) {
        const match = document.createElement("span");
        match.className = "lane-readout";
        match.dataset["help"] = "match";
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
    if (this.layout() === ViewLayout.stacked) {
      container.replaceChildren(
        ...lanes(styles.length, ViewLayout.stacked).map((lane, index) => {
          const node = document.createElement("div");
          const style = styles[index];
          if (style !== undefined) {
            node.append(name(style, index));
          }
          return place(
            node,
            ((1 - (lane.center + lane.halfHeight)) / 2) * height + 3,
          );
        }),
      );
    } else {
      const legend = document.createElement("div");
      legend.append(...styles.map(name));
      container.replaceChildren(
        ...(styles.length > 0 ? [place(legend, 3)] : []),
      );
    }
    // The Compare view without another track says why.
    if (this.compareMode() && frame?.lanes.length === 1) {
      const note = document.createElement("div");
      note.className = "compare-note";
      note.textContent = `No other track in group ${String(this.store.get("group")).toUpperCase()} to compare with. Insert Sineless on another track.`;
      container.append(note);
    }
    this.updateMatches(frame);
  }

  // Adds a track to the comparison, or takes it out.
  private toggleCompared(id: number): void {
    const others = (this.laneSets.get(this.latestLaneSet) ?? []).filter(
      (style) => !style.own,
    );
    this.backend.compare(
      others
        .filter((style) => style.compared !== (style.id === id))
        .map((style) => style.id),
    );
  }

  // The Compare view's readouts in the lane labels: each compared track's offset and polarity
  // (or that it matches nowhere clearly) and correlation against this track, and the sum's gain (with the fix, while it is previewed).
  private updateMatches(frame: Frame | undefined): void {
    this.laneNameNodes.forEach((node, index) => {
      node.classList.toggle(
        "invalid",
        frame !== undefined && laneHoldsInvalid(frame, index),
      );
    });
    const comparison = frame?.comparison;
    this.matchNodes.forEach((node, index) => {
      const match = comparison?.matches[index];
      if (node === undefined || match === undefined || frame === undefined) {
        return;
      }
      if (Number.isNaN(match.correlation)) {
        node.textContent = "—"; // They never sound together: nothing measured.
        return;
      }
      const offsetMs = (match.offset / frame.sampleRate) * 1000;
      const alignment = match.related
        ? `${formatOffset(offsetMs)}${match.inverted ? " Ø" : ""}`
        : "unrelated";
      node.textContent = `${alignment} · ${signed(match.correlation, 2)}`;
    });
    if (this.sumNode !== undefined && comparison !== undefined) {
      const gain = this.previewFix ? comparison.fixedGain : comparison.sumGain;
      this.sumNode.textContent = formatGain(gain);
      this.sumNode.className = `lane-readout ${gain < -summingMarginDb ? "cancels" : gain > summingMarginDb ? "reinforces" : ""}`;
    }
  }

  private beatsMode(): boolean {
    return this.store.get("timeBase") === "beats";
  }

  private readoutView(): ChannelView {
    return readoutView(
      this.beatsMode(),
      this.store.get("triggerSource") as ViewName,
    );
  }

  // The musical grid of the last synced frame, when in Beats mode.
  private musicalLayout(): ReturnType<typeof musicalGrid> | undefined {
    const frame = this.lastFrame;
    if (!this.beatsMode() || frame?.synced !== true) {
      return undefined;
    }
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
    );
  }

  private drawScreen(): void {
    drawGrid(
      this.gridCanvas,
      this.laneCount(),
      { grid: token("--grid"), axis: token("--grid-axis") },
      this.musicalLayout()?.lines,
    );
    this.drawnLaneCount = this.laneCount();
    this.placeLabels();
    this.placeLaneLabels();
    this.placeTriggerMarker(); // It sits in a lane: lanes may have changed.
  }

  // Moves the sweep cursor, and redraws grid and labels when a new cycle starts.
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

  private placeLabels(): void {
    const container = element("labels");
    const height = this.screen.clientHeight;
    const width = this.screen.clientWidth;
    const span = this.store.number("timeSpan");
    const roll = this.rolling();
    const labels: HTMLElement[] = [];
    for (const label of amplitudeLabels(
      this.store.number("verticalScale"),
      this.laneCount(),
      this.layout(),
      height,
    )) {
      const node = document.createElement("div");
      node.className = "axis-label";
      node.textContent = label.text;
      // Clear of the trigger level marker, which sits on the left edge.
      node.style.left = "13px";
      node.style.top = `${String(Math.min(height - 14, Math.max(2, label.position * height - 6)))}px`;
      labels.push(node);
    }
    for (const label of this.musicalLayout()?.labels ??
      timeLabels(span, roll, this.view)) {
      const node = document.createElement("div");
      node.className = "axis-label";
      node.textContent = label.text;
      if (label.detail !== undefined) {
        const detail = document.createElement("span");
        detail.className = "axis-detail";
        detail.textContent = label.detail;
        node.append(detail);
      }
      // The left edge label sits clear of the amplitude labels.
      node.style.left = `${String(Math.max(label.position * width + 4, label.position === 0 ? amplitudeLabelWidth : 0))}px`;
      node.style.bottom = "3px";
      labels.push(node);
    }
    container.replaceChildren(...labels);
    const triggerLine = element("trigger-line");
    const triggerX = toScreen(this.view, triggerPosition);
    triggerLine.style.left = `${String(triggerX * 100)}%`;
    triggerLine.hidden =
      roll ||
      this.store.get("triggerMode") === "free" ||
      this.beatsMode() ||
      triggerX < 0 ||
      triggerX > 1;
  }

  // The level marker sits in the lane of the trigger source when stacked, else on the full height.
  // In the multi-track view the own track triggers, so the marker sits in its lane.
  private markerGeometry(): { center: number; halfHeight: number } {
    // The source's lane if it is drawn; the first lane otherwise (a mono track triggers on L).
    const shown = this.drawnViews();
    const source = this.store.get("triggerSource") as ViewName;
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
    const lane = lanes(this.laneCount(), this.layout())[index] ?? {
      center: 0,
      halfHeight: 1,
    };
    const height = this.screen.clientHeight;
    return {
      center: ((1 - lane.center) / 2) * height,
      halfHeight: (lane.halfHeight / 2) * height,
    };
  }

  // Whether the trace rolls, as the engine decided for the latest frame.
  private rolling(): boolean {
    return this.lastFrame?.roll === true;
  }

  private placeTriggerMarker(): void {
    const marker = element("trigger-marker");
    const hidden =
      this.rolling() ||
      this.store.get("triggerMode") === "free" ||
      this.beatsMode();
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
        : `var(${viewColorTokens[this.store.get("triggerSource") as ViewName]})`,
    );
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
    const laneList = lanes(this.laneCount(), this.layout());
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

  // The view whose trace cursors snap to: the one in `lane` when stacked, otherwise the trigger
  // source when shown, or the first view shown.
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
      return frame.lanes[
        lane !== undefined && this.layout() === ViewLayout.stacked ? lane : own
      ];
    }
    const views = [...frame.views.values()];
    if (lane !== undefined && this.layout() === ViewLayout.stacked) {
      return views[lane];
    }
    // Overlaid, the channel the readouts measure; the first view if that one is not drawn.
    return frame.views.get(this.readoutView()) ?? views[0];
  }

  // The traces as the hover readout sees them, in lane order, named.
  private hoverContext(frame: Frame): HoverContext {
    let traces: HoverTrace[];
    let overlaid = 0;
    if (frame.lanes.length > 0) {
      const styles = this.lanesOf(frame);
      traces = frame.lanes.map((values, index) => ({
        name: styles[index]?.name ?? `Track ${String(index + 1)}`,
        values,
      }));
      overlaid = Math.max(
        0,
        styles.findIndex((style) => style.own),
      );
    } else {
      const source = this.readoutView();
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
      roll: frame.roll,
    };
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
    if (readout === undefined || this.hover === undefined) {
      node.hidden = true;
      line.hidden = true;
      band.hidden = true;
      return;
    }
    // The marks cover the trace's lane only.
    const width = this.screen.clientWidth;
    const screenHeight = this.screen.clientHeight;
    const laneTop = `${String(readout.top * screenHeight)}px`;
    const laneHeight = `${String((readout.bottom - readout.top) * screenHeight)}px`;
    line.style.left = `${String(readout.x * width)}px`;
    line.style.top = laneTop;
    line.style.height = laneHeight;
    line.hidden = false;
    if (readout.cycle === undefined) {
      band.hidden = true;
    } else {
      band.style.left = `${String(readout.cycle.start * width)}px`;
      band.style.width = `${String((readout.cycle.end - readout.cycle.start) * width)}px`;
      band.style.top = laneTop;
      band.style.height = laneHeight;
      band.hidden = false;
    }
    const title = document.createElement("strong");
    title.textContent = readout.title;
    node.replaceChildren(
      title,
      ...readout.lines.map((text) => {
        const line = document.createElement("div");
        line.textContent = text;
        return line;
      }),
    );
    node.hidden = false;
    // Below and right of the pointer, flipped where the screen ends.
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
    node.style.left = `${String(Math.max(0, left))}px`;
    node.style.top = `${String(Math.max(0, top))}px`;
  }

  private updateCursorReadout(): void {
    const readout = element("cursor-readout");
    const frame = this.lastFrame;
    const spanSamples =
      frame === undefined
        ? 0
        : frame.visibleSamples / (frame.viewEnd - frame.viewStart);
    const lines: string[] = [];
    if (
      this.timeCursors !== undefined &&
      frame !== undefined &&
      spanSamples > 0
    ) {
      lines.push(
        ...cursorReadout(
          measure(
            this.timeCursors,
            spanSamples / frame.sampleRate,
            frame.synced ? frame.spanQuarterNotes : undefined,
          ),
          frame.sampleRate,
          this.store.number("concertPitch"),
        ),
      );
    }
    if (this.levelCursors !== undefined) {
      lines.push(...levelReadout(this.levelCursors));
    }
    readout.hidden = lines.length === 0;
    readout.replaceChildren(
      ...lines.map((text) => {
        const line = document.createElement("div");
        line.textContent = text;
        return line;
      }),
    );
  }

  private listenToScreen(): void {
    // The hover readout follows the pointer over the traces while no button is held.
    this.screen.addEventListener("pointermove", (event) => {
      const rectangle = this.screen.getBoundingClientRect();
      this.hover =
        event.buttons === 0 && event.target instanceof HTMLCanvasElement
          ? {
              x: (event.clientX - rectangle.left) / rectangle.width,
              y: (event.clientY - rectangle.top) / rectangle.height,
            }
          : undefined;
      this.updateHover();
    });
    for (const type of ["pointerleave", "pointerdown", "wheel"] as const) {
      this.screen.addEventListener(
        type,
        () => {
          this.hover = undefined;
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
        line.setPointerCapture(event.pointerId);
        const move = (moveEvent: PointerEvent) => {
          const frame = this.lastFrame;
          if (this.timeCursors === undefined) {
            return;
          }
          const rectangle = this.screen.getBoundingClientRect();
          // Stacked, the cursor snaps to the trace in the lane under the pointer.
          const values = this.snapValues(
            this.layout() === ViewLayout.stacked
              ? traceAt(
                  this.laneCount(),
                  ViewLayout.stacked,
                  (moveEvent.clientY - rectangle.top) / rectangle.height,
                  0,
                )
              : undefined,
          );
          const x = Math.min(
            1,
            Math.max(0, (moveEvent.clientX - rectangle.left) / rectangle.width),
          );
          let position = toFrame(this.view, x);
          if (
            !moveEvent.altKey &&
            frame !== undefined &&
            values !== undefined
          ) {
            const radius =
              (snapRadius / rectangle.width) *
              (this.view.end - this.view.start);
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
        };
        const up = () => {
          line.removeEventListener("pointermove", move);
          line.removeEventListener("pointerup", up);
        };
        line.addEventListener("pointermove", move);
        line.addEventListener("pointerup", up);
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
      line.setPointerCapture(event.pointerId);
      const laneIndex = Number(line.dataset["lane"]);
      const key = line.dataset["key"] === "first" ? "first" : "second";
      const move = (moveEvent: PointerEvent) => {
        const lane = lanes(this.laneCount(), this.layout())[laneIndex];
        if (this.levelCursors === undefined || lane === undefined) {
          return;
        }
        const rectangle = this.screen.getBoundingClientRect();
        const center = ((1 - lane.center) / 2) * rectangle.height;
        const halfHeight = (lane.halfHeight / 2) * rectangle.height;
        const scale = fullScale(this.store.number("verticalScale"));
        const relative = Math.max(
          -1,
          Math.min(
            1,
            (center - (moveEvent.clientY - rectangle.top)) / halfHeight,
          ),
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
      };
      const up = () => {
        line.removeEventListener("pointermove", move);
        line.removeEventListener("pointerup", up);
      };
      line.addEventListener("pointermove", move);
      line.addEventListener("pointerup", up);
    });
    levelLines.addEventListener("dblclick", (event) => {
      event.stopPropagation();
    });
    const marker = element("trigger-marker");
    marker.addEventListener("pointerdown", (event) => {
      event.stopPropagation();
      marker.setPointerCapture(event.pointerId);
      const move = (moveEvent: PointerEvent) => {
        const rectangle = this.screen.getBoundingClientRect();
        const { center, halfHeight } = this.markerGeometry();
        const relative =
          (center - (moveEvent.clientY - rectangle.top)) / halfHeight;
        const level = relative * fullScale(this.store.number("verticalScale"));
        this.request(
          "triggerLevel",
          Number(clampTo("triggerLevel", level).toFixed(3)),
        );
      };
      const up = () => {
        marker.removeEventListener("pointermove", move);
        marker.removeEventListener("pointerup", up);
      };
      marker.addEventListener("pointermove", move);
      marker.addEventListener("pointerup", up);
    });

    // View zoom: ⌘ + scroll or pinch zooms around the pointer, a horizontal
    // swipe pans, dragging zooms to the selection, double-click resets.
    const screenPosition = (clientX: number) => {
      const rectangle = this.screen.getBoundingClientRect();
      return Math.min(
        1,
        Math.max(0, (clientX - rectangle.left) / rectangle.width),
      );
    };
    this.screen.addEventListener(
      "wheel",
      (event) => {
        // In pixels: Windows reports lines, or pages with "scroll one screen at a time", which
        // would otherwise zoom and pan by almost nothing.
        const unit =
          event.deltaMode === WheelEvent.DOM_DELTA_LINE
            ? wheelLinePixels
            : event.deltaMode === WheelEvent.DOM_DELTA_PAGE
              ? this.screen.clientHeight
              : 1;
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
            zoomAround(this.view, screenPosition(event.clientX), factor),
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
      this.setView(zoomAround(this.view, gestureAnchor, scale / gestureScale));
      gestureScale = scale;
    });

    const selection = element("selection");
    this.screen.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) {
        return;
      }
      const from = screenPosition(event.clientX);
      let to = from;
      this.screen.setPointerCapture(event.pointerId);
      const move = (moveEvent: PointerEvent) => {
        to = screenPosition(moveEvent.clientX);
        const dragged =
          Math.abs(to - from) * this.screen.clientWidth >=
          minimumSelectionPixels;
        selection.hidden = !dragged;
        selection.style.left = `${String(Math.min(from, to) * 100)}%`;
        selection.style.width = `${String(Math.abs(to - from) * 100)}%`;
      };
      const up = () => {
        if (!selection.hidden) {
          this.setView(zoomTo(this.view, from, to));
        }
        selection.hidden = true;
        this.screen.removeEventListener("pointermove", move);
        this.screen.removeEventListener("pointerup", up);
      };
      this.screen.addEventListener("pointermove", move);
      this.screen.addEventListener("pointerup", up);
    });
    this.screen.addEventListener("dblclick", () => {
      this.setView(fullView);
    });
  }

  // A horizontal swipe of `screenWidths`: pans the zoomed view, or, frozen and not zoomed, pans
  // back through the held data. A frozen sweep keeps its cycle.
  private swipe(screenWidths: number): void {
    if (isZoomed(this.view)) {
      this.setView(panBy(this.view, screenWidths));
    } else if (this.lastFrame?.frozen === true && !this.lastFrame.synced) {
      // No further back than the engine says this frozen view reaches.
      const span = this.store.number("timeSpan");
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
  }

  // ----- Engine events -----

  private listenToBackend(): void {
    this.backend.on("frame", (frame) => {
      const rolled = this.rolling();
      this.lastFrame = frame;
      if (frame.roll !== rolled) {
        this.placeLabels();
        this.placeTriggerMarker();
      }
      this.lastFrameTime = performance.now();
      this.debug.lastFrame = frame;
      this.debug.frameCount++;
      if (this.waiting) {
        this.waiting = false;
      }
      // Only changes touch the page: each one costs the page work to restyle.
      if (frame.frozen !== this.shownFrozen) {
        this.shownFrozen = frame.frozen;
        element("freeze").setAttribute("aria-pressed", String(frame.frozen));
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
      // A track joined or left, or the views drawn changed (a track turned mono): lanes, grid
      // and labels change with it. A new lane set (a track checked, a renamed track) changes the
      // labels.
      if (this.laneCount() !== this.drawnLaneCount) {
        this.drawScreen();
        this.placeCursors();
      } else if (frame.laneSet !== this.drawnLaneSet) {
        this.placeLaneLabels();
        this.placeTriggerMarker();
      }
      this.drawnLaneSet = frame.laneSet;
      this.statusDue();
    });
    this.backend.on("error", (message) => {
      this.showError(message);
    });
    this.backend.on("lanes", (message) => {
      this.laneSets.set(message.laneSet, laneStyles(message.lanes));
      for (const old of [...this.laneSets.keys()]) {
        if (old < message.laneSet - 3) {
          this.laneSets.delete(old);
        }
      }
      this.latestLaneSet = message.laneSet;
      this.hiddenTracks = message.hidden;
      this.drawScreen();
      this.loop.invalidate();
      this.updateStatus();
    });
    this.backend.on("snapshot", (message) => {
      this.engineError = false;
      element("message").hidden = !this.graphicsMessage;
      this.inputChannels = message.inputChannels;
      this.showSetting("channelViews", this.store.get("channelViews"));
      this.drawScreen();
      this.placeTriggerMarker();
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
    }
    if (this.multiMode() && this.hiddenTracks > 0) {
      chips.push({
        text: `+${String(this.hiddenTracks)} hidden`,
        tone: "warn",
      });
    }
    if (isZoomed(this.view)) {
      chips.push({
        text: `Zoom \u00d7${zoomFactor(this.view).toFixed(1)}`,
        tone: "neutral",
      });
    }
    const shown = JSON.stringify(chips);
    if (shown !== this.shownChips) {
      this.shownChips = shown;
      element("chips").replaceChildren(
        ...chips.map((chip) => {
          const node = document.createElement("span");
          node.className = `chip ${chip.tone}`;
          node.textContent = chip.text;
          return node;
        }),
      );
    }
    this.updateCursorReadout();
    this.updateHover(); // Live data changes under a resting pointer.
    const frame = this.lastFrame;
    if (frame !== undefined) {
      element("peak").textContent = formatDbfs(frame.peak);
      element("rms").textContent = formatDbfs(frame.rms);
      element("frequency").textContent = formatFrequency(frame.frequency);
      this.updateMatches(frame);
    }
  }

  // The renderer, or undefined with the message shown when the web view gives the page no
  // WebGL2 or the shaders do not build.
  private createRenderer(
    style: TraceStyle,
    view: () => TraceView,
  ): TraceRenderer | undefined {
    try {
      return new TraceRenderer(this.traceCanvas, style, view);
    } catch {
      this.showGraphicsMessage(
        "This system gives the editor no WebGL 2 graphics to draw the traces with. Updating the graphics driver may help. Audio still passes through unchanged.",
      );
      return undefined;
    }
  }

  private showGraphicsMessage(text: string): void {
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
