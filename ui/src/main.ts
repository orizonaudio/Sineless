// Minimal session client until the UI proper arrives (M3): says hello, receives frames and
// returns a credit after each animation frame (AD-12).
import { decodeFrame, type Frame } from "../../protocol/src/frame-decoder.ts";
import type {
  ErrorMessage,
  SnapshotMessage,
  StateChangedMessage,
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
    orizonDebug?: {
      snapshot?: SnapshotMessage;
      error?: ErrorMessage;
      lastState?: StateChangedMessage;
      lastFrame?: Frame;
      frameCount: number;
    };
  }
}

const status = document.createElement("p");
document.body.append(status);
status.textContent = "Connecting...";

const debug: NonNullable<Window["orizonDebug"]> = { frameCount: 0 };
window.orizonDebug = debug;

const transport = new NativeTransport({
  channel: window.webkit?.messageHandlers["orizon"],
  receiver: window,
});

transport.onMessage((message) => {
  if (message.type === "snapshot") {
    debug.snapshot = message as unknown as SnapshotMessage;
  } else if (message.type === "stateChanged") {
    debug.lastState = message as unknown as StateChangedMessage;
  } else if (message.type === "error") {
    debug.error = message as unknown as ErrorMessage;
    status.textContent = debug.error.message;
  }
});

transport.onFrame((buffer) => {
  const frame = decodeFrame(buffer);
  debug.lastFrame = frame;
  debug.frameCount++;
  requestAnimationFrame(() => {
    status.textContent = `Frame ${String(frame.sequence)}: ${String(frame.pointCount)} points`;
    transport.send({ type: "credit" });
  });
});

transport.send({
  type: "hello",
  protocolVersion,
  viewWidth: document.documentElement.clientWidth,
  pixelRatio: window.devicePixelRatio,
});
