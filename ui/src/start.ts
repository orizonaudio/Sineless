// Starts the UI on a transport: the native channel inside the plugin, the mock in a browser
// and in the app tests.
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import type {
  ErrorMessage,
  SnapshotMessage,
  StateChangedMessage,
} from "../../protocol/src/messages.ts";
import { OscilloscopeApp, type DebugState } from "./app.ts";
import { Backend } from "./bridge/backend.ts";
import type { NativeReceiver, Transport } from "./bridge/transport.ts";
import { perf, type Perf } from "./perf.ts";
import { SettingsStore } from "./store/settings-store.ts";

declare global {
  interface Window {
    // The native channel: WKWebView on macOS, WebView2 on Windows.
    webkit?: {
      messageHandlers: Record<
        string,
        { postMessage(text: string): void } | undefined
      >;
    };
    chrome?: {
      webview?: {
        postMessage(text: string): void;
        addEventListener(
          type: "message",
          listener: (event: { data: unknown }) => void,
        ): void;
      };
    };
    orizonBridge?: NativeReceiver;
    // Session state exposed for the editor's automated tests.
    orizonDebug?: DebugState & {
      snapshot?: SnapshotMessage;
      error?: ErrorMessage;
      lastState?: StateChangedMessage;
      lastFrame?: Frame;
      perf?: Perf;
      requestFrame?: () => void;
    };
  }
}

// The page holds one app: it finds its elements by id.
export function startSineless(transport: Transport): void {
  const backend = new Backend(transport);
  const store = new SettingsStore();

  const debug: NonNullable<Window["orizonDebug"]> = {
    frameCount: 0,
    perf,
    // For the editor's tests, where WebKit pauses the draw loop that normally asks.
    requestFrame: () => {
      backend.requestFrame();
    },
  };
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
}
