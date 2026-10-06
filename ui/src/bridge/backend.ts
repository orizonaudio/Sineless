// The UI's only way to the engine (DESIGN 5.1): typed calls out, typed events in. Components use
// this interface and never touch the transport.
import {
  decodeFrame,
  type Frame,
} from "../../../protocol/src/frame-decoder.ts";
import type {
  ErrorMessage,
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
  frame(frame: Frame): void;
}

export class Backend {
  private readonly transport: Transport;
  private readonly events: Partial<BackendEvents> = {};

  constructor(transport: Transport) {
    this.transport = transport;
    transport.onMessage((message) => {
      this.dispatch(message);
    });
    transport.onFrame((buffer) => {
      this.events.frame?.(decodeFrame(buffer));
    });
  }

  on<Event extends keyof BackendEvents>(
    event: Event,
    handler: BackendEvents[Event],
  ): void {
    this.events[event] = handler;
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

  arm(): void {
    this.transport.send({ type: "arm" });
  }

  credit(): void {
    this.transport.send({ type: "credit" });
  }

  private dispatch(message: ControlMessage): void {
    switch (message.type) {
      case "snapshot":
        this.events.snapshot?.(message as unknown as SnapshotMessage);
        break;
      case "stateChanged":
        this.events.stateChanged?.(message as unknown as StateChangedMessage);
        break;
      case "error":
        this.events.error?.(message as unknown as ErrorMessage);
        break;
      default:
        break;
    }
  }
}
