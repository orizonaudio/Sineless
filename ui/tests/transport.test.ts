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

  test("fetches one binary frame per request", async () => {
    let fetches = 0;
    const host: NativeHost = {
      channel: undefined,
      receiver: {},
      fetchFrame: () => {
        fetches++;
        return Promise.resolve(new Uint8Array([0, 1, 2, 255]).buffer);
      },
    };
    const transport = new NativeTransport(host);
    const frames: ArrayBuffer[] = [];
    transport.onFrame((frame) => frames.push(frame));
    transport.requestFrame();
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(fetches).toBe(1);
    expect(frames.map((frame) => Array.from(new Uint8Array(frame)))).toEqual([
      [0, 1, 2, 255],
    ]);
  });

  test("drops an empty answer to a superseded request", async () => {
    const host: NativeHost = {
      channel: undefined,
      receiver: {},
      fetchFrame: () => Promise.resolve(new ArrayBuffer(0)),
    };
    const transport = new NativeTransport(host);
    const frames: ArrayBuffer[] = [];
    transport.onFrame((frame) => frames.push(frame));
    transport.requestFrame();
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(frames).toEqual([]);
  });

  test("does nothing when the page runs without a native channel", () => {
    const host: NativeHost = { channel: undefined, receiver: {} };
    expect(() => {
      new NativeTransport(host).send({ type: "arm" });
    }).not.toThrow();
  });
});
