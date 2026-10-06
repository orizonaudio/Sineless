import { describe, expect, test } from "vitest";

import {
  NativeTransport,
  type ControlMessage,
  type NativeHost,
} from "../src/bridge/transport.ts";

function makeHost(): { host: NativeHost; posted: string[] } {
  const posted: string[] = [];
  const host: NativeHost = {
    channel: { postMessage: (text) => posted.push(text) },
    receiver: {},
  };
  return { host, posted };
}

describe("native transport", () => {
  test("sends control messages as JSON text", () => {
    const { host, posted } = makeHost();
    new NativeTransport(host).send({ type: "hello", viewWidth: 800 });
    expect(posted.map((text) => JSON.parse(text) as unknown)).toEqual([
      { type: "hello", viewWidth: 800 },
    ]);
  });

  test("delivers messages with a type and drops anything else", () => {
    const { host } = makeHost();
    const transport = new NativeTransport(host);
    const received: ControlMessage[] = [];
    transport.onMessage((message) => received.push(message));
    const bridge = host.receiver.orizonBridge;
    bridge?.receive('{"type": "snapshot", "sampleRate": 48000}');
    bridge?.receive("not json");
    bridge?.receive('{"sampleRate": 48000}');
    bridge?.receive("[1, 2]");
    expect(received).toEqual([{ type: "snapshot", sampleRate: 48000 }]);
  });

  test("decodes binary frames from base64", () => {
    const { host } = makeHost();
    const transport = new NativeTransport(host);
    const frames: ArrayBuffer[] = [];
    transport.onFrame((frame) => frames.push(frame));
    host.receiver.orizonBridge?.receiveBinary("AAEC/w==");
    expect(frames.map((frame) => Array.from(new Uint8Array(frame)))).toEqual([
      [0, 1, 2, 255],
    ]);
  });

  test("does nothing when the page runs without a native channel", () => {
    const host: NativeHost = { channel: undefined, receiver: {} };
    expect(() => {
      new NativeTransport(host).send({ type: "credit" });
    }).not.toThrow();
  });
});
