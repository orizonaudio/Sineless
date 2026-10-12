// Property-based tests of the page's message parsing against malformed input: whatever text the
// native side delivers, the transport never throws, and passes on exactly the JSON objects with a
// string `type` (protocol/src/messages.ts), as they were sent.
import fc from "fast-check";
import { describe, expect, test } from "vitest";

import { MessageType } from "../../protocol/src/messages.ts";
import { settingNames } from "../../protocol/src/setting-values.ts";
import {
  NativeTransport,
  type ControlMessage,
} from "../src/bridge/transport.ts";

function receiveAll(texts: readonly string[]): ControlMessage[] {
  const receiver: { orizonBridge?: { receive(text: string): void } } = {};
  const transport = new NativeTransport({ channel: undefined, receiver });
  const received: ControlMessage[] = [];
  transport.onMessage((message) => received.push(message));
  for (const text of texts) {
    receiver.orizonBridge?.receive(text);
  }
  return received;
}

// A JSON object (not an array, a string or null) whose `type` is a string.
function isControlMessage(value: unknown): boolean {
  return (
    Object.prototype.toString.call(value) === "[object Object]" &&
    typeof (value as Record<string, unknown>)["type"] === "string"
  );
}

function parsed(text: string): unknown {
  try {
    return JSON.parse(text) as unknown;
  } catch {
    return undefined;
  }
}

// Messages shaped like the protocol's: a known or unknown type, protocol field and setting names,
// and any JSON values in them.
const fieldName = fc.oneof(
  fc.constantFrom(
    "type",
    "name",
    "value",
    "settings",
    "protocolVersion",
    "sampleRate",
    "inputChannels",
    "laneSet",
    "lanes",
    "hidden",
    "code",
    "message",
    ...settingNames,
  ),
  fc.string(),
);
const messageLike = fc
  .tuple(
    fc.oneof(fc.constantFrom(...Object.values(MessageType)), fc.jsonValue()),
    fc.dictionary(fieldName, fc.jsonValue(), { maxKeys: 8 }),
  )
  .map(([type, fields]) => ({ ...fields, type }));

// Text that is JSON, almost JSON, or anything at all.
const text = fc.oneof(
  fc.string(),
  fc.json(),
  messageLike.map((message) => JSON.stringify(message)),
  messageLike
    .map((message) => JSON.stringify(message))
    .chain((json) =>
      fc
        .integer({ min: 0, max: json.length })
        .map((length) => json.slice(0, length)),
    ),
);

describe("native transport against malformed input", () => {
  test("delivers exactly the objects with a string type, unchanged", () => {
    fc.assert(
      fc.property(fc.array(text, { maxLength: 8 }), (texts) => {
        const expected = texts.map(parsed).filter(isControlMessage);
        expect(receiveAll(texts)).toEqual(expected);
      }),
    );
  });

  test("messages shaped like the protocol's arrive as sent", () => {
    fc.assert(
      fc.property(messageLike, (message) => {
        const received = receiveAll([JSON.stringify(message)]);
        expect(received).toEqual(
          typeof message.type === "string"
            ? [parsed(JSON.stringify(message))]
            : [],
        );
      }),
    );
  });
});
