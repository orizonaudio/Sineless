// The UI side of the engine/UI transport: JSON control messages out, JSON control
// messages and binary frames in. Components never touch the native channel directly.
import type { MessageType } from "../../../protocol/src/messages.ts";
import { perf } from "../perf.ts";

export interface ControlMessage {
  readonly type: MessageType;
  readonly [field: string]: unknown;
}

export interface Transport {
  send(message: ControlMessage): void;
  // Asks for the next display frame; it arrives through onFrame. One request is one frame, so
  // the UI asks again only once it has drawn the last.
  requestFrame(): void;
  onMessage(handler: (message: ControlMessage) => void): void;
  onFrame(handler: (frame: ArrayBuffer) => void): void;
}

// What the native side provides: a channel to post to, an object on which it calls
// receive(text), and how to fetch a frame (the page's own fetch of the frame URL).
export interface NativeHost {
  readonly channel: { postMessage(text: string): void } | undefined;
  readonly receiver: { orizonBridge?: NativeReceiver };
  readonly fetchFrame?: () => Promise<ArrayBuffer>;
}

export interface NativeReceiver {
  receive(text: string): void;
}

// The native side answers this request with the next frame's bytes once one is ready.
export const frameUrl = "orizon://ui/frame";

function fetchFrame(): Promise<ArrayBuffer> {
  return fetch(frameUrl, { cache: "no-store" }).then((response) =>
    response.arrayBuffer(),
  );
}

function isControlMessage(value: unknown): value is ControlMessage {
  return (
    typeof value === "object" &&
    value !== null &&
    typeof (value as Record<string, unknown>)["type"] === "string"
  );
}

export class NativeTransport implements Transport {
  private messageHandler: ((message: ControlMessage) => void) | undefined;
  private frameHandler: ((frame: ArrayBuffer) => void) | undefined;

  private readonly host: NativeHost;

  constructor(host: NativeHost) {
    this.host = host;
    host.receiver.orizonBridge = {
      receive: (text) => {
        this.receive(text);
      },
    };
  }

  send(message: ControlMessage): void {
    this.host.channel?.postMessage(JSON.stringify(message));
  }

  requestFrame(): void {
    // A failed request (the page going away) just ends the stream.
    (this.host.fetchFrame ?? fetchFrame)()
      .then((frame) => {
        if (frame.byteLength === 0) {
          return; // Superseded by a newer request.
        }
        const started = performance.now();
        try {
          this.frameHandler?.(frame);
        } catch (error) {
          // The frame is dropped. Nothing else asks for the next one: without this, the
          // display would stop until the editor is opened again.
          console.error(error);
          this.requestFrame();
          return;
        }
        perf.framesReceived++;
        perf.bytesReceived += frame.byteLength;
        perf.receiveMs += performance.now() - started;
      })
      .catch(() => undefined);
  }

  onMessage(handler: (message: ControlMessage) => void): void {
    this.messageHandler = handler;
  }

  onFrame(handler: (frame: ArrayBuffer) => void): void {
    this.frameHandler = handler;
  }

  private receive(text: string): void {
    let parsed: unknown;
    try {
      parsed = JSON.parse(text);
    } catch {
      return; // Malformed messages are dropped, as on the native side.
    }
    if (isControlMessage(parsed)) {
      this.messageHandler?.(parsed);
    }
  }
}
