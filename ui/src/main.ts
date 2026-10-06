// Entry point: connects the backend, the settings store and the render loop.
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import type {
  ErrorMessage,
  SnapshotMessage,
  StateChangedMessage,
} from "../../protocol/src/messages.ts";
import { Backend } from "./bridge/backend.ts";
import { MockTransport } from "./bridge/mock-transport.ts";
import {
  NativeTransport,
  type NativeReceiver,
  type Transport,
} from "./bridge/transport.ts";
import { ChannelView, ViewLayout } from "../../protocol/src/enums.ts";
import { RenderLoop } from "./render/render-loop.ts";
import { TraceRenderer, type Rgba } from "./render/trace-renderer.ts";
import { SettingsStore } from "./store/settings-store.ts";

declare global {
  interface Window {
    webkit?: {
      messageHandlers: Record<
        string,
        { postMessage(text: string): void } | undefined
      >;
    };
    orizonBridge?: NativeReceiver;
    // Session state exposed for the editor's automated tests.
    orizonDebug?: {
      snapshot?: SnapshotMessage;
      error?: ErrorMessage;
      lastState?: StateChangedMessage;
      lastFrame?: Frame;
      frameCount: number;
      drawMilliseconds?: () => number;
    };
  }
}

// Inside the plugin the native channel exists; in a desktop browser the mock stands in.
const channel = window.webkit?.messageHandlers["orizon"];
const transport: Transport =
  channel !== undefined
    ? new NativeTransport({ channel, receiver: window })
    : new MockTransport();
const backend = new Backend(transport);
const store = new SettingsStore();

const status = document.createElement("p");
status.className = "status";
const canvas = document.createElement("canvas");
canvas.className = "trace";
document.body.append(canvas, status);
status.textContent = "Connecting...";

// Provisional colours, to be settled in the design review (M3).
const colors = new Map<ChannelView, Rgba>([
  [ChannelView.left, [0.35, 0.85, 0.45, 1]],
  [ChannelView.right, [0.95, 0.45, 0.4, 1]],
  [ChannelView.mid, [0.95, 0.8, 0.3, 1]],
  [ChannelView.side, [0.45, 0.7, 1, 1]],
]);
const renderer = new TraceRenderer(
  canvas,
  { colors, lineWidthPixels: 1.5 * window.devicePixelRatio },
  () => ({
    verticalScaleDb: store.number("verticalScale"),
    layout:
      store.get("viewLayout") === "stacked"
        ? ViewLayout.stacked
        : ViewLayout.overlaid,
  }),
);
const loop = new RenderLoop(
  (callback) => requestAnimationFrame(callback),
  renderer,
  () => {
    backend.credit();
  },
);

const debug: NonNullable<Window["orizonDebug"]> = {
  frameCount: 0,
  drawMilliseconds: () => renderer.drawMilliseconds,
};
window.orizonDebug = debug;

backend.on("snapshot", (message) => {
  debug.snapshot = message;
  status.textContent = "";
  store.applySnapshot(message);
});
// Vertical scale and layout are applied at draw time: redraw without waiting for a frame.
store.subscribe((name) => {
  if (name === "verticalScale" || name === "viewLayout") {
    loop.invalidate();
  }
});
backend.on("stateChanged", (message) => {
  debug.lastState = message;
  store.applyState(message.name, message.value);
});
backend.on("error", (message) => {
  debug.error = message;
  status.textContent = message.message;
});
backend.on("frame", (frame) => {
  debug.lastFrame = frame;
  debug.frameCount++;
  loop.submit(frame);
});

function sizeCanvas(): void {
  canvas.width = Math.round(canvas.clientWidth * window.devicePixelRatio);
  canvas.height = Math.round(canvas.clientHeight * window.devicePixelRatio);
}

sizeCanvas();
backend.hello(canvas.clientWidth, window.devicePixelRatio);
window.addEventListener("resize", () => {
  sizeCanvas();
  backend.setViewport(canvas.clientWidth, window.devicePixelRatio);
  loop.invalidate();
});
