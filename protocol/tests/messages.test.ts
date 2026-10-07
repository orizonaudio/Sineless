import { expect, test } from "vitest";

import {
  MessageType,
  type ArmMessage,
  type CreditMessage,
  type FreezeMessage,
  type HelloMessage,
  type PanMessage,
  type SetSettingMessage,
  type ViewMessage,
  type ViewportMessage,
} from "../src/messages.ts";

import examples from "./data/messages.json" with { type: "json" };

// JSON imports widen `type` to string; every other field must match its payload type exactly.
type Shape<Message extends { type: string }> = Omit<Message, "type"> & {
  readonly type: string;
};

// The C++ session tests parse these same examples, so field names cannot drift.
const typed = {
  hello: examples.hello satisfies Shape<HelloMessage>,
  setSetting: examples.setSetting satisfies Shape<
    Omit<SetSettingMessage, "name">
  >,
  setSettingFlags: examples.setSettingFlags satisfies Shape<
    Omit<SetSettingMessage, "name">
  >,
  setSettingNumber: examples.setSettingNumber satisfies Shape<
    Omit<SetSettingMessage, "name">
  >,
  credit: examples.credit satisfies Shape<CreditMessage>,
  viewport: examples.viewport satisfies Shape<ViewportMessage>,
  freeze: examples.freeze satisfies Shape<FreezeMessage>,
  pan: examples.pan satisfies Shape<PanMessage>,
  arm: examples.arm satisfies Shape<ArmMessage>,
  view: examples.view satisfies Shape<ViewMessage>,
};

test("every example names a known message type", () => {
  for (const message of Object.values(typed)) {
    expect(Object.values(MessageType)).toContain(message.type);
  }
});
