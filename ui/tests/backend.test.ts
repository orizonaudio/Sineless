import { describe, expect, test } from "vitest";

import golden from "../../protocol/tests/data/golden-frame.json" with { type: "json" };
import { protocolVersion } from "../../protocol/src/version.ts";
import { Backend } from "../src/bridge/backend.ts";
import type { ControlMessage, Transport } from "../src/bridge/transport.ts";

class FakeTransport implements Transport {
  sent: ControlMessage[] = [];
  frameRequests = 0;
  messageHandler: ((message: ControlMessage) => void) | undefined;
  frameHandler: ((frame: ArrayBuffer) => void) | undefined;
  send(message: ControlMessage): void {
    this.sent.push(message);
  }
  requestFrame(): void {
    this.frameRequests++;
  }
  onMessage(handler: (message: ControlMessage) => void): void {
    this.messageHandler = handler;
  }
  onFrame(handler: (frame: ArrayBuffer) => void): void {
    this.frameHandler = handler;
  }
}

describe("backend", () => {
  test("asks the transport for frames", () => {
    const transport = new FakeTransport();
    new Backend(transport).requestFrame();
    expect(transport.frameRequests).toBe(1);
  });

  test("sends typed messages", () => {
    const transport = new FakeTransport();
    const backend = new Backend(transport);
    backend.hello(640, 2);
    backend.setSetting("triggerMode", "single");
    backend.freeze(true);
    backend.pan(0.5);
    backend.arm();
    backend.setViewport(500, 1);
    backend.compare([3, 5]);
    backend.previewFix(true);
    expect(transport.sent.map((message) => message.type)).toEqual([
      "hello",
      "setSetting",
      "freeze",
      "pan",
      "arm",
      "viewport",
      "compare",
      "previewFix",
    ]);
    expect(transport.sent.at(-2)).toMatchObject({ ids: [3, 5] });
    expect(transport.sent.at(-1)).toMatchObject({ on: true });
    expect(transport.sent[0]).toMatchObject({
      protocolVersion,
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
    backend.on("frame", (frame) => events.push(`frame:${String(frame.peak)}`));
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
      lanes: [
        { id: 1, name: "Kick", colour: null, own: true, compared: false },
      ],
      hidden: 0,
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
      `frame:${String(golden.header.peak)}`,
    ]);
  });
});
