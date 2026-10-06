// Entry point: picks the transport and starts the UI.
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import type {
  ErrorMessage,
  SnapshotMessage,
  StateChangedMessage,
} from "../../protocol/src/messages.ts";
import { OscilloscopeApp, type DebugState } from "./app.ts";
import { Backend } from "./bridge/backend.ts";
import { MockTransport } from "./bridge/mock-transport.ts";
import {
  NativeTransport,
  type NativeReceiver,
  type Transport,
} from "./bridge/transport.ts";
import { SettingsStore } from "./store/settings-store.ts";
import "./styles.css";

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
    orizonDebug?: DebugState & {
      snapshot?: SnapshotMessage;
      error?: ErrorMessage;
      lastState?: StateChangedMessage;
      lastFrame?: Frame;
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

const debug: NonNullable<Window["orizonDebug"]> = { frameCount: 0 };
window.orizonDebug = debug;

// The store hears engine values first; the app's own handlers run after these.
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
});
new OscilloscopeApp(backend, store, debug).start();
