// The oscilloscope UI: wires the controls, screen and readouts to the backend and
// the settings store. Controls show only engine-reported values and send requests.
import { ChannelView, ViewLayout } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
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
import { HelpTips } from "./components/help-tips.ts";
import { Segmented } from "./components/segmented.ts";
import { RenderLoop } from "./render/render-loop.ts";
import { fullScale, lanes } from "./render/trace-geometry.ts";
import { TraceRenderer, type Rgba } from "./render/trace-renderer.ts";
import type { SettingsStore } from "./store/settings-store.ts";
import {
  amplitudeLabels,
  timeLabels,
  triggerPosition,
} from "./view/axis-labels.ts";
import {
  formatDb,
  formatDbfs,
  formatFrequency,
  formatSeconds,
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
import type { HelpTopic } from "./view/help.ts";
import {
  extremePositions,
  levelTargets,
  snap,
  zeroCrossings,
} from "./view/snap.ts";
import { beatSpanLabels, musicalGrid } from "./view/musical-grid.ts";
import { statusChips } from "./view/status.ts";
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
  holdOffSteps,
  spanSteps,
  stepLinear,
  stepThrough,
} from "./view/steps.ts";

const viewOrder = ["left", "right", "mid", "side"] as const;
type ViewName = (typeof viewOrder)[number];
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
  left: "--left",
  right: "--right",
  mid: "--mid",
  side: "--side",
};
// How fast ⌘ + scroll and pinch zoom, per wheel delta unit.
const wheelZoomRate = 0.01;
// Layouts of the control strip, from the widest to the narrowest (see styles.css).
const controlFits = ["full", "compact", "minimal"] as const;
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
const readoutInterval = 100; // ms: readouts stay legible while frames arrive at display rate.

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
  private readonly renderer: TraceRenderer;
  private readonly loop: RenderLoop;
  private lastFrame: Frame | undefined;
  private lastFrameTime = 0;
  private lastReadoutTime = 0;
  private panSeconds = 0;
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
  private levelCursors: LevelPair | undefined;
  private sampleRate = 48000;

  constructor(backend: Backend, store: SettingsStore, debug: DebugState) {
    this.backend = backend;
    this.store = store;
    this.debug = debug;
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
    this.renderer = new TraceRenderer(
      this.traceCanvas,
      { colors: resolveColors, lineWidthPixels: 1.5 * window.devicePixelRatio },
      () => ({
        verticalScaleDb: store.number("verticalScale"),
        layout: this.layout(),
        dim: this.waiting,
        view: this.view,
      }),
    );
    this.loop = new RenderLoop(
      (callback) => requestAnimationFrame(callback),
      {
        draw: (frame) => {
          this.renderer.draw(frame);
          this.showSweep(frame);
        },
      },
      () => {
        backend.credit();
      },
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
      this.updateStatus();
    }, readoutInterval);
    this.resize();
  }

  start(): void {
    this.backend.hello(this.screen.clientWidth, window.devicePixelRatio);
  }

  private layout(): ViewLayout {
    return this.store.get("viewLayout") === "stacked"
      ? ViewLayout.stacked
      : ViewLayout.overlaid;
  }

  private shownViews(): ViewName[] {
    const value = this.store.get("channelViews");
    const names = Array.isArray(value) ? value : [];
    return viewOrder.filter((name) => names.includes(name));
  }

  private request(name: SettingName, value: SettingValue): void {
    this.backend.setSetting(name, value);
  }

  // ----- Controls -----

  private numberField(
    name: SettingName,
    host: string,
    label: string,
    format: (value: number) => string,
    step: (value: number, delta: number, fine: boolean) => number,
    defaultUnitScale = 1,
  ): void {
    const field = new DragField({
      label,
      format,
      parse: (text) => parseQuantity(text, defaultUnitScale),
      step: (value, delta, fine) => clampTo(name, step(value, delta, fine)),
      onRequest: (value) => {
        this.request(name, clampTo(name, value));
      },
    });
    element(host).append(field.element);
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

    this.numberField(
      "timeSpan",
      "span-field",
      "Time span",
      formatSeconds,
      (value, delta) => stepThrough(spanSteps, value, delta),
      0.001,
    );
    this.numberField(
      "verticalScale",
      "scale-field",
      "Vertical scale",
      formatDb,
      (value, delta, fine) => stepLinear(value, delta, fine ? 1 : 3, -60, 6),
    );
    this.numberField(
      "triggerLevel",
      "level-field",
      "Trigger level",
      (value) => signed(value, 3),
      (value, delta, fine) =>
        stepLinear(value, delta, fine ? 0.001 : 0.01, -1, 1),
    );
    this.numberField(
      "triggerHysteresis",
      "hysteresis-field",
      "Trigger hysteresis",
      (value) => value.toFixed(3),
      (value, delta, fine) =>
        stepLinear(value, delta, fine ? 0.001 : 0.005, 0, 0.5),
    );
    this.numberField(
      "triggerHoldOff",
      "hold-off-field",
      "Trigger hold-off",
      formatSeconds,
      (value, delta) => stepThrough(holdOffSteps, value, delta),
      0.001,
    );

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
        short?: string,
        help?: HelpTopic,
      ][],
    ) => {
      this.segmented.set(
        name,
        new Segmented(
          element(host),
          choices.map(([value, label, short, help]) => ({
            value,
            label,
            ...(short === undefined ? {} : { short }),
            ...(help === undefined ? {} : { help }),
          })),
          (value) => {
            this.request(name, value);
          },
        ),
      );
    };
    segment("timeBase", "time-base", [
      ["beats", "Beats"],
      ["seconds", "Sec"],
    ]);
    segment("viewLayout", "layout", [
      ["overlaid", "Overlay"],
      ["stacked", "Stack"],
    ]);
    segment("triggerMode", "mode", [
      ["auto", "Auto", "A", "modeAuto"],
      ["normal", "Normal", "N", "modeNormal"],
      ["free", "Free", "F", "modeFree"],
      ["single", "Single", "S", "modeSingle"],
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

    const details = element("trigger-details");
    const popover = element("trigger-popover");
    details.addEventListener("click", () => {
      popover.hidden = !popover.hidden;
      details.setAttribute("aria-expanded", String(!popover.hidden));
      this.placeTriggerPopover();
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
  private placeTriggerPopover(): void {
    const popover = element("trigger-popover");
    if (popover.hidden) {
      return;
    }
    const group = element("trigger-group");
    const strip = popover.offsetParent?.clientWidth ?? window.innerWidth;
    const margin = 8;
    const left = Math.min(
      group.offsetLeft,
      strip - popover.offsetWidth - margin,
    );
    popover.style.left = `${String(Math.max(margin, left))}px`;
  }

  // Keeps the control strip on one row: the first fit, from full labels down to the essential
  // controls, whose content does not overflow. Returns whether the fit changed.
  private fitControls(): boolean {
    const controls = document.querySelector<HTMLElement>(".controls");
    if (controls === null) {
      return false;
    }
    const before = controls.dataset["fit"];
    for (const fit of controlFits) {
      controls.dataset["fit"] = fit;
      if (controls.scrollWidth <= controls.clientWidth) {
        break;
      }
    }
    return controls.dataset["fit"] !== before;
  }

  private showSetting(name: SettingName, value: SettingValue): void {
    if (typeof value === "number") {
      this.fields.get(name)?.show(value);
    } else if (typeof value === "string") {
      this.segmented.get(name)?.show(value);
    }
    if (name === "channelViews") {
      const shown = this.shownViews();
      for (const [view, button] of this.viewToggles) {
        button.setAttribute("aria-pressed", String(shown.includes(view)));
      }
    }
    if (name === "triggerMode" || name === "timeBase") {
      this.updateTriggerControls();
    }
    // Level cursors are drawn at their level in each lane.
    if (
      name === "verticalScale" ||
      name === "viewLayout" ||
      name === "channelViews"
    ) {
      this.placeCursors();
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
    // A value of another width can change which fit the strip needs, and with it the screen size.
    if (this.fitControls()) {
      this.resize();
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
    this.fitControls();
    this.placeTriggerPopover();
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
    return this.layout() === ViewLayout.stacked
      ? Math.max(1, this.shownViews().length)
      : 1;
  }

  private beatsMode(): boolean {
    return this.store.get("timeBase") === "beats";
  }

  // The musical grid of the last synced frame, when in Beats mode.
  private musicalLayout(): ReturnType<typeof musicalGrid> | undefined {
    const frame = this.lastFrame;
    if (!this.beatsMode() || frame?.synced !== true) {
      return undefined;
    }
    return musicalGrid(
      frame.cycleStart,
      frame.spanQuarterNotes,
      frame.barQuarterNotes,
      this.view,
    );
  }

  private drawScreen(): void {
    drawGrid(
      this.gridCanvas,
      this.laneCount(),
      { grid: token("--grid"), axis: token("--grid-axis") },
      this.musicalLayout()?.lines,
    );
    this.placeLabels();
  }

  // Moves the sweep cursor, and redraws grid and labels when a new cycle starts.
  private showSweep(frame: Frame): void {
    const cursor = element("sweep-cursor");
    const previous = element("sweep-previous");
    const synced = frame.synced && frame.cursor !== undefined;
    cursor.hidden = !synced;
    previous.hidden = !synced;
    if (!synced) {
      return;
    }
    const position = toScreen(this.view, frame.cursor);
    cursor.hidden = position < 0 || position > 1;
    const percent = `${String(Math.min(1, Math.max(0, position)) * 100)}%`;
    cursor.style.left = percent;
    previous.style.left = percent;
    const key = `${String(frame.cycleStart)}/${String(frame.spanQuarterNotes)}/${String(frame.barQuarterNotes)}`;
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
    const roll = span > 1;
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
  private markerGeometry(): { center: number; halfHeight: number } {
    const shown = this.shownViews();
    const source = this.store.get("triggerSource") as ViewName;
    const index =
      this.layout() === ViewLayout.stacked
        ? Math.max(0, shown.indexOf(source))
        : 0;
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

  private placeTriggerMarker(): void {
    const marker = element("trigger-marker");
    const hidden =
      this.store.number("timeSpan") > 1 ||
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
    marker.style.setProperty(
      "--marker",
      `var(${viewColorTokens[this.store.get("triggerSource") as ViewName]})`,
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
      laneList.forEach((_, lane) => {
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
    const views = [...frame.views.values()];
    if (lane !== undefined && this.layout() === ViewLayout.stacked) {
      return views[lane];
    }
    const source =
      ChannelView[this.store.get("triggerSource") as keyof typeof ChannelView];
    return frame.views.get(source) ?? views[0];
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
            spanSamples / this.sampleRate,
            frame.synced ? frame.spanQuarterNotes : undefined,
          ),
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
          const values = this.snapValues();
          if (this.timeCursors === undefined) {
            return;
          }
          const rectangle = this.screen.getBoundingClientRect();
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
        if (event.metaKey || event.ctrlKey) {
          // Trackpad pinches arrive as wheel events with ctrlKey set.
          event.preventDefault();
          const factor = Math.exp(-event.deltaY * wheelZoomRate);
          this.setView(
            zoomAround(this.view, screenPosition(event.clientX), factor),
          );
        } else if (Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
          event.preventDefault();
          this.swipe(event.deltaX / this.screen.clientWidth);
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
      const seconds = screenWidths * this.store.number("timeSpan");
      this.panSeconds = Math.max(0, this.panSeconds - seconds);
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
      this.lastFrame = frame;
      this.lastFrameTime = performance.now();
      this.debug.lastFrame = frame;
      this.debug.frameCount++;
      if (this.waiting) {
        this.waiting = false;
      }
      element("freeze").setAttribute("aria-pressed", String(frame.frozen));
      // The engine may resume on its own (auto-freeze); a new freeze starts unpanned.
      if (!frame.frozen) {
        this.panSeconds = 0;
      }
      this.loop.submit(frame);
      if (this.lastFrameTime - this.lastReadoutTime >= readoutInterval) {
        this.updateStatus();
      }
    });
    this.backend.on("error", (message) => {
      this.showError(message);
    });
    this.backend.on("snapshot", (message) => {
      element("message").hidden = true;
      this.sampleRate = message.sampleRate;
    });
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
    if (isZoomed(this.view)) {
      chips.push({
        text: `Zoom \u00d7${zoomFactor(this.view).toFixed(1)}`,
        tone: "neutral",
      });
    }
    element("chips").replaceChildren(
      ...chips.map((chip) => {
        const node = document.createElement("span");
        node.className = `chip ${chip.tone}`;
        node.textContent = chip.text;
        return node;
      }),
    );
    this.updateCursorReadout();
    const frame = this.lastFrame;
    if (frame !== undefined) {
      this.lastReadoutTime = now;
      element("peak").textContent = formatDbfs(frame.peak);
      element("rms").textContent = formatDbfs(frame.rms);
      element("frequency").textContent = formatFrequency(frame.frequency);
    }
  }

  private showError(message: ErrorMessage): void {
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
