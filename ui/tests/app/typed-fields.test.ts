// Typing a value: in the room the value took, so editing moves and widens nothing; the beat span
// read from its number alone; the vertical scale in whole dB only, as its steps.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import type { SettingName } from "../../../protocol/src/settings.ts";
import { numberFields } from "../../src/view/fields.ts";
import {
  beforeApp,
  byId,
  inTimeBase,
  settle,
  startApp,
  typeInto,
  type Started,
} from "./harness.ts";

// The time base each field shows in: Trig's span and the trigger's fields in Trig, Free's span in
// Free, the beat span in Beats; the others in any.
const timeBaseOf: Readonly<Record<string, string>> = {
  "span-field": "Trig",
  "free-span-field": "Free",
  "beat-span-field": "Beats",
  "level-field": "Trig",
  "hysteresis-field": "Trig",
  "hold-off-field": "Trig",
  "position-field": "Trig",
};

// Every typed field: the number fields and the beat span.
const hosts = [
  ...Object.values(numberFields).map((field) => field.host),
  "free-span-field",
  "beat-span-field",
].filter((host, index, all) => all.indexOf(host) === index);

// Shows the field in `host`: its time base, and the popover it sits in opened.
async function show(host: string): Promise<HTMLElement> {
  await inTimeBase(timeBaseOf[host] ?? "Trig");
  await userEvent.click(byId("wordmark"));
  const popover = byId(host).closest<HTMLElement>(".popover");
  if (popover?.hidden === true) {
    await userEvent.click(byId(popover.id.replace(/-popover$/u, "-details")));
  }
  const field = byId(host).querySelector<HTMLElement>(".field");
  expect(field, host).not.toBeNull();
  return field as HTMLElement;
}

describe("a typed value", () => {
  let app: Started;
  beforeApp(async () => {
    app = await startApp();
    await settle();
  });

  const asked = (setting: SettingName, from: number) =>
    app.sent
      .slice(from)
      .filter(
        (message) => message.type === "setSetting" && message.name === setting,
      )
      .map((message) => (message as unknown as { value: unknown }).value);

  test.each(hosts)(
    "%s keeps its place and width while it is typed in",
    async (host) => {
      const field = await show(host);
      const pill = field.closest<HTMLElement>(".icon-field") ?? field;
      const before = pill.getBoundingClientRect();
      await userEvent.dblClick(field);
      const input = field.querySelector("input");
      expect(input).not.toBeNull();
      const during = pill.getBoundingClientRect();
      expect(during.left).toBeCloseTo(before.left, 0);
      expect(during.width).toBeCloseTo(before.width, 0);
      await userEvent.keyboard("{Escape}");
    },
  );

  test.each([
    ["16", "16 bars", "sixteenBars"],
    ["1/4", "1/4 bar", "quarterBar"],
    ["6 bars", "6 bars", "sixBars"],
  ])("the beat span reads %j as %s", async (typed, shown, name) => {
    const field = await show("beat-span-field");
    const from = app.sent.length;
    await typeInto("beat-span-field", typed);
    expect(asked("beatSpan", from)).toEqual([name]);
    await expect.poll(() => field.textContent).toBe(shown);
  });

  test("the vertical scale takes whole dB only, as its steps", async () => {
    const field = await show("scale-field");
    const before = field.textContent;
    const from = app.sent.length;
    await typeInto("scale-field", "1.5");
    expect(asked("verticalScale", from)).toEqual([]);
    expect(field.textContent).toBe(before);
    await typeInto("scale-field", "-12");
    expect(asked("verticalScale", from)).toEqual([-12]);
    await expect.poll(() => field.textContent).toBe("−12 dB");
  });

  // Whole percent, sent as that fraction exactly (57% as 0.57, not 0.5700000000000001), so what is
  // saved is what is shown.
  test("the trigger position takes whole percent only, as its steps", async () => {
    const field = await show("position-field");
    const from = app.sent.length;
    await typeInto("position-field", "12.5%");
    expect(asked("triggerPosition", from)).toEqual([]);
    await typeInto("position-field", "57");
    await typeInto("position-field", "7%");
    expect(asked("triggerPosition", from)).toEqual([0.57, 0.07]);
    await expect.poll(() => field.textContent).toBe("7%");
  });
});
