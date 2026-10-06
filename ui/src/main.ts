// Minimal session client until the UI proper arrives (M3): says hello and shows the snapshot.
import type {
  ErrorMessage,
  SnapshotMessage,
} from "../../protocol/src/messages.ts";
import { protocolVersion } from "../../protocol/src/version.ts";
import { NativeTransport, type NativeReceiver } from "./bridge/transport.ts";

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
    orizonDebug?: { snapshot?: SnapshotMessage; error?: ErrorMessage };
  }
}

const status = document.createElement("p");
document.body.append(status);
status.textContent = "Connecting...";

const debug: NonNullable<Window["orizonDebug"]> = {};
window.orizonDebug = debug;

const transport = new NativeTransport({
  channel: window.webkit?.messageHandlers["orizon"],
  receiver: window,
});

transport.onMessage((message) => {
  if (message.type === "snapshot") {
    const snapshot = message as unknown as SnapshotMessage;
    debug.snapshot = snapshot;
    status.textContent = `Connected: ${String(snapshot.sampleRate)} Hz, ${String(snapshot.inputChannels)} channel(s)`;
  } else if (message.type === "error") {
    const error = message as unknown as ErrorMessage;
    debug.error = error;
    status.textContent = error.message;
  }
});

transport.send({
  type: "hello",
  protocolVersion,
  viewWidth: document.documentElement.clientWidth,
  pixelRatio: window.devicePixelRatio,
});
