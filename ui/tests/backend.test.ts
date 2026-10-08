import { describe, expect, test } from "vitest";

import golden from "../../protocol/tests/data/golden-frame.json" with { type: "json" };
import { Backend } from "../src/bridge/backend.ts";
import type { ControlMessage, Transport } from "../src/bridge/transport.ts";

class FakeTransport implements Transport {
  sent: ControlMessage[] = [];
  messageHandler: ((message: ControlMessage) => void) | undefined;
  frameHandler: ((frame: ArrayBuffer) => void) | undefined;
  send(message: ControlMessage): void {
    this.sent.push(message);
  }
  onMessage(handler: (message: ControlMessage) => void): void {
    this.messageHandler = handler;
  }
  onFrame(handler: (frame: ArrayBuffer) => void): void {
    this.frameHandler = handler;
  }
}

describe("backend", () => {
  test("sends typed messages", () => {
    const transport = new FakeTransport();
    const backend = new Backend(transport);
    backend.hello(640, 2);
    backend.setSetting("triggerMode", "single");
    backend.freeze(true);
    backend.pan(0.5);
    backend.arm();
    backend.credit();
    backend.setViewport(500, 1);
    backend.partner(3);
    backend.previewFix(true);
    expect(transport.sent.map((message) => message.type)).toEqual([
      "hello",
      "setSetting",
      "freeze",
      "pan",
      "arm",
      "credit",
      "viewport",
      "partner",
      "previewFix",
    ]);
    expect(transport.sent.at(-2)).toMatchObject({ id: 3 });
    expect(transport.sent.at(-1)).toMatchObject({ on: true });
    expect(transport.sent[0]).toMatchObject({
      protocolVersion: 1,
      viewWidth: 640,
      pixelRatio: 2,
    });
  });

  test("routes engine messages and decodes frames", () => {
    const transport = new FakeTransport();
    const backend = new Backend(transport);
    const events: string[] = [];
    backend.on("stateChanged", (message) =>
      events.push(`state:${message.name}`),
    );
    backend.on("error", (message) => events.push(`error:${message.code}`));
    backend.on("lanes", (message) =>
      events.push(`lanes:${String(message.laneSet)}`),
    );
    backend.on("frame", (frame) =>
      events.push(`frame:${String(frame.sequence)}`),
    );
    transport.messageHandler?.({
      type: "stateChanged",
      name: "timeSpan",
      value: 0.5,
    });
    transport.messageHandler?.({
      type: "error",
      code: "protocolMismatch",
      message: "",
    });
    transport.messageHandler?.({
      type: "lanes",
      laneSet: 3,
      lanes: [{ id: 1, name: "Kick", colour: null, own: true }],
      hidden: 0,
      candidates: [],
    });
    const bytes = new Uint8Array(golden.hex.length / 2);
    for (let i = 0; i < bytes.length; i++) {
      bytes[i] = parseInt(golden.hex.slice(i * 2, i * 2 + 2), 16);
    }
    transport.frameHandler?.(bytes.buffer);
    expect(events).toEqual([
      "state:timeSpan",
      "error:protocolMismatch",
      "lanes:3",
      "frame:7",
    ]);
  });
});
