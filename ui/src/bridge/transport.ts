// The UI side of the engine/UI transport: JSON control messages out, JSON control
// messages and binary frames in. Components never touch the native channel directly.
import type { MessageType } from "../../../protocol/src/messages.ts";

export interface ControlMessage {
  readonly type: MessageType;
  readonly [field: string]: unknown;
}

export interface Transport {
  send(message: ControlMessage): void;
  onMessage(handler: (message: ControlMessage) => void): void;
  onFrame(handler: (frame: ArrayBuffer) => void): void;
}

// What the native side provides: a channel to post to, and an object on which it calls
// receive(text) and receiveBinary(base64).
export interface NativeHost {
  readonly channel: { postMessage(text: string): void } | undefined;
  readonly receiver: { orizonBridge?: NativeReceiver };
}

export interface NativeReceiver {
  receive(text: string): void;
  receiveBinary(base64: string): void;
}

export function base64ToArrayBuffer(base64: string): ArrayBuffer {
  const text = atob(base64);
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) {
    bytes[i] = text.charCodeAt(i);
  }
  return bytes.buffer;
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
      receiveBinary: (base64) => {
        this.frameHandler?.(base64ToArrayBuffer(base64));
      },
    };
  }

  send(message: ControlMessage): void {
    this.host.channel?.postMessage(JSON.stringify(message));
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
