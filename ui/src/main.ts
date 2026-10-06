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
import { RenderLoop, type FrameRenderer } from "./render/render-loop.ts";
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
document.body.append(status);
status.textContent = "Connecting...";

// Placeholder until the WebGL2 trace renderer (T-031).
const renderer: FrameRenderer = {
  draw(frame) {
    status.textContent = `Frame ${String(frame.sequence)}: ${String(frame.pointCount)} points`;
  },
};
const loop = new RenderLoop(
  (callback) => requestAnimationFrame(callback),
  renderer,
  () => {
    backend.credit();
  },
);

const debug: NonNullable<Window["orizonDebug"]> = { frameCount: 0 };
window.orizonDebug = debug;

backend.on("snapshot", (message) => {
  debug.snapshot = message;
  store.applySnapshot(message);
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

const traceWidth = (): number => document.documentElement.clientWidth;
backend.hello(traceWidth(), window.devicePixelRatio);
window.addEventListener("resize", () => {
  backend.setViewport(traceWidth(), window.devicePixelRatio);
});
