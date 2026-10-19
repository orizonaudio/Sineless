// The UI's only way to the engine: typed calls out, typed events in. Components use
// this interface and never touch the transport.
import {
  decodeFrame,
  type Frame,
} from "../../../protocol/src/frame-decoder.ts";
import type {
  ErrorMessage,
  LanesMessage,
  SettingValue,
  SnapshotMessage,
  StateChangedMessage,
} from "../../../protocol/src/messages.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import { protocolVersion } from "../../../protocol/src/version.ts";
import type { ControlMessage, Transport } from "./transport.ts";

export interface BackendEvents {
  snapshot(message: SnapshotMessage): void;
  stateChanged(message: StateChangedMessage): void;
  error(message: ErrorMessage): void;
  lanes(message: LanesMessage): void;
  frame(frame: Frame): void;
}

export class Backend {
  private readonly transport: Transport;
  private readonly handlers: {
    [Event in keyof BackendEvents]: BackendEvents[Event][];
  } = {
    snapshot: [],
    stateChanged: [],
    error: [],
    lanes: [],
    frame: [],
  };

  constructor(transport: Transport) {
    this.transport = transport;
    transport.onMessage((message) => {
      this.dispatch(message);
    });
    transport.onFrame((buffer) => {
      const frame = decodeFrame(buffer);
      for (const handler of this.handlers.frame) {
        handler(frame);
      }
    });
  }

  // Adds a handler; handlers run in the order they were added.
  on<Event extends keyof BackendEvents>(
    event: Event,
    handler: BackendEvents[Event],
  ): void {
    this.handlers[event].push(handler);
  }

  hello(viewWidth: number, pixelRatio: number): void {
    this.transport.send({
      type: "hello",
      protocolVersion,
      viewWidth,
      pixelRatio,
    });
  }

  setSetting(name: SettingName, value: SettingValue): void {
    this.transport.send({ type: "setSetting", name, value });
  }

  setViewport(viewWidth: number, pixelRatio: number): void {
    this.transport.send({ type: "viewport", viewWidth, pixelRatio });
  }

  freeze(frozen: boolean): void {
    this.transport.send({ type: "freeze", frozen });
  }

  pan(seconds: number): void {
    this.transport.send({ type: "pan", seconds });
  }

  // The part of the span on screen.
  setView(start: number, end: number): void {
    this.transport.send({ type: "view", start, end });
  }

  // Compare view: the other tracks to compare with this one, by the ids of their lanes entries.
  compare(ids: readonly number[]): void {
    this.transport.send({ type: "compare", ids });
  }

  previewFix(on: boolean): void {
    this.transport.send({ type: "previewFix", on });
  }

  arm(): void {
    this.transport.send({ type: "arm" });
  }

  // The size the user dragged the resize grip to, for the whole editor in CSS pixels.
  resize(width: number, height: number): void {
    this.transport.send({ type: "resize", width, height });
  }

  // Asks for the next display frame: once at the start, then after each frame drawn.
  requestFrame(): void {
    this.transport.requestFrame();
  }

  private dispatch(message: ControlMessage): void {
    switch (message.type) {
      case "snapshot":
        for (const handler of this.handlers.snapshot) {
          handler(message as unknown as SnapshotMessage);
        }
        break;
      case "stateChanged":
        for (const handler of this.handlers.stateChanged) {
          handler(message as unknown as StateChangedMessage);
        }
        break;
      case "error":
        for (const handler of this.handlers.error) {
          handler(message as unknown as ErrorMessage);
        }
        break;
      case "lanes":
        for (const handler of this.handlers.lanes) {
          handler(message as unknown as LanesMessage);
        }
        break;
      default:
        break;
    }
  }
}
