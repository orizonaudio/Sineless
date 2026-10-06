// The oscilloscope UI (DESIGN 8.5): wires the controls, screen and readouts to the backend and
// the settings store. Controls show only engine-reported values and send requests.
import { ChannelView, ViewLayout } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import type {
  ErrorMessage,
  SettingValue,
} from "../../protocol/src/messages.ts";
import { settingNames, settingOf } from "../../protocol/src/setting-values.ts";
import type {
  NumberSetting,
  SettingName,
} from "../../protocol/src/settings.ts";
import type { Backend } from "./bridge/backend.ts";
import { DragField } from "./components/drag-field.ts";
import { Segmented } from "./components/segmented.ts";
import { RenderLoop } from "./render/render-loop.ts";
import { fullScale, lanes } from "./render/trace-geometry.ts";
import { TraceRenderer, type Rgba } from "./render/trace-renderer.ts";
import type { SettingsStore } from "./store/settings-store.ts";
import { amplitudeLabels, timeLabels } from "./view/axis-labels.ts";
import {
  formatDb,
  formatDbfs,
  formatFrequency,
  formatSeconds,
  parseQuantity,
  signed,
} from "./view/format.ts";
import { drawGrid } from "./view/grid.ts";
import { statusChips } from "./view/status.ts";
import {
  holdOffSteps,
  spanSteps,
  stepLinear,
  stepThrough,
} from "./view/steps.ts";

const viewOrder = ["left", "right", "mid", "side"] as const;
type ViewName = (typeof viewOrder)[number];
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
// Below this screen height (CSS px) only the outer and zero amplitude labels fit.
const compactScreenHeight = 140;
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
      }),
    );
    this.loop = new RenderLoop(
      (callback) => requestAnimationFrame(callback),
      this.renderer,
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
      choices: [value: string, label: string, short?: string][],
    ) => {
      this.segmented.set(
        name,
        new Segmented(
          element(host),
          choices.map(([value, label, short]) => ({
            value,
            label,
            ...(short === undefined ? {} : { short }),
          })),
          (value) => {
            this.request(name, value);
          },
        ),
      );
    };
    segment("viewLayout", "layout", [
      ["overlaid", "Overlay"],
      ["stacked", "Stack"],
    ]);
    segment("triggerMode", "mode", [
      ["auto", "Auto", "A"],
      ["normal", "Normal", "N"],
      ["free", "Free", "F"],
      ["single", "Single", "S"],
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
    element("arm").addEventListener("click", () => {
      this.panSeconds = 0;
      this.backend.arm();
      this.backend.freeze(false);
    });
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
    if (name === "triggerMode") {
      (element("arm") as HTMLButtonElement).disabled = value !== "single";
    }
    this.updateTriggerSummary();
    // Settings that move the grid, labels or trigger line; the engine's next frame follows.
    const screenSettings: SettingName[] = [
      "verticalScale",
      "viewLayout",
      "channelViews",
      "timeSpan",
      "triggerMode",
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

  private drawScreen(): void {
    drawGrid(this.gridCanvas, this.laneCount(), {
      grid: token("--grid"),
      axis: token("--grid-axis"),
    });
    this.placeLabels();
  }

  private placeLabels(): void {
    const container = element("labels");
    const height = this.screen.clientHeight;
    const width = this.screen.clientWidth;
    const span = this.store.number("timeSpan");
    const roll = span > 1;
    const labels: HTMLElement[] = [];
    for (const label of amplitudeLabels(
      fullScale(this.store.number("verticalScale")),
      this.laneCount(),
      this.layout(),
      height < compactScreenHeight,
    )) {
      const node = document.createElement("div");
      node.className = "axis-label";
      node.textContent = label.text;
      node.style.left = "5px";
      node.style.top = `${String(Math.min(height - 14, Math.max(2, label.position * height - 6)))}px`;
      labels.push(node);
    }
    for (const label of timeLabels(span, roll)) {
      const node = document.createElement("div");
      node.className = "axis-label";
      node.textContent = label.text;
      node.style.left = `${String(label.position * width + 4)}px`;
      node.style.bottom = "3px";
      labels.push(node);
    }
    container.replaceChildren(...labels);
    element("trigger-line").hidden =
      roll || this.store.get("triggerMode") === "free";
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
      this.store.get("triggerMode") === "free";
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

  private listenToScreen(): void {
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

    // While frozen, a horizontal drag pans back through the held data (FR-6).
    this.screen.addEventListener("pointerdown", (event) => {
      if (this.lastFrame?.frozen !== true) {
        return;
      }
      this.screen.setPointerCapture(event.pointerId);
      this.screen.classList.add("panning");
      const startX = event.clientX;
      const startPan = this.panSeconds;
      const move = (moveEvent: PointerEvent) => {
        const seconds =
          ((moveEvent.clientX - startX) / this.screen.clientWidth) *
          this.store.number("timeSpan");
        this.panSeconds = Math.max(0, startPan + seconds);
        this.backend.pan(this.panSeconds);
      };
      const up = () => {
        this.screen.classList.remove("panning");
        this.screen.removeEventListener("pointermove", move);
        this.screen.removeEventListener("pointerup", up);
      };
      this.screen.addEventListener("pointermove", move);
      this.screen.addEventListener("pointerup", up);
    });
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
      this.loop.submit(frame);
      if (this.lastFrameTime - this.lastReadoutTime >= readoutInterval) {
        this.updateStatus();
      }
    });
    this.backend.on("error", (message) => {
      this.showError(message);
    });
    this.backend.on("snapshot", () => {
      element("message").hidden = true;
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
    element("chips").replaceChildren(
      ...chips.map((chip) => {
        const node = document.createElement("span");
        node.className = `chip ${chip.tone}`;
        node.textContent = chip.text;
        return node;
      }),
    );
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
