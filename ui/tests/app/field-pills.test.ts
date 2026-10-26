// A value in a bar sits in a pill with its icon or label (Span, Scale, A4), which reads as one
// control: a drag, the wheel or a double-click anywhere on the pill, its icon included, does
// what it does on the value.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import type { SettingName } from "../../../protocol/src/settings.ts";
import { numberFields } from "../../src/view/fields.ts";
import { byId, choice, startApp, type Started } from "./harness.ts";

// Each pill's value: its host, the setting it asks for and the time base it shows in. The last
// test checks the list holds every number field (ui/src/view/fields.ts) that sits in a pill.
const pills: readonly {
  readonly name: string;
  readonly host: string;
  readonly setting: SettingName;
  readonly timeBase: "Sec" | "Beats";
}[] = [
  { name: "Span", host: "span-field", setting: "timeSpan", timeBase: "Sec" },
  {
    name: "Beat span",
    host: "beat-span-field",
    setting: "beatSpan",
    timeBase: "Beats",
  },
  {
    name: "Scale",
    host: "scale-field",
    setting: "verticalScale",
    timeBase: "Sec",
  },
  {
    name: "A4",
    host: "concert-pitch-field",
    setting: "concertPitch",
    timeBase: "Sec",
  },
];

function pillOf(host: string): HTMLElement {
  const pill = byId(host).closest<HTMLElement>(".icon-field");
  expect(pill, `${host}'s pill`).not.toBeNull();
  return pill as HTMLElement;
}

// The pill's icon or label, which is outside the value.
function iconOf(host: string): HTMLElement {
  const icon = pillOf(host).querySelector<HTMLElement>(".icon, i");
  expect(icon, `${host}'s icon`).not.toBeNull();
  expect(byId(host).contains(icon)).toBe(false);
  return icon as HTMLElement;
}

function fieldOf(host: string): HTMLElement {
  return byId(host).querySelector(".field") as HTMLElement;
}

function pointer(type: string, target: Element, y: number): void {
  const box = target.getBoundingClientRect();
  target.dispatchEvent(
    new PointerEvent(type, {
      pointerId: 1,
      bubbles: true,
      button: 0,
      buttons: type === "pointerup" ? 0 : 1,
      clientX: box.left + box.width / 2,
      clientY: y,
    }),
  );
}

describe("a value's pill", () => {
  let app: Started;
  beforeAll(async () => {
    app = await startApp();
  });

  // What the page asked for of `setting` since message `from`.
  const asked = (setting: SettingName, from: number) =>
    app.sent
      .slice(from)
      .filter(
        (message) => message.type === "setSetting" && message.name === setting,
      );

  test.each(pills)(
    "$name: dragging from the icon steps the value",
    async ({ host, setting, timeBase }) => {
      await userEvent.click(choice("time-base", timeBase));
      const icon = iconOf(host);
      const from = app.sent.length;
      const y = icon.getBoundingClientRect().top;
      // The drag is captured by the value, as one started on it.
      pointer("pointerdown", icon, y);
      pointer("pointermove", fieldOf(host), y - 30);
      pointer("pointerup", fieldOf(host), y - 30);
      expect(asked(setting, from).length).toBeGreaterThan(0);
    },
  );

  // A real click: the icon's mousedown must not take focus from the value.
  test.each(pills)(
    "$name: pressing the icon focuses the value",
    async ({ host, timeBase }) => {
      await userEvent.click(choice("time-base", timeBase));
      await userEvent.click(iconOf(host));
      expect(document.activeElement).toBe(fieldOf(host));
    },
  );

  test.each(pills)(
    "$name: the wheel over the icon steps the value",
    async ({ host, setting, timeBase }) => {
      await userEvent.click(choice("time-base", timeBase));
      const from = app.sent.length;
      iconOf(host).dispatchEvent(
        new WheelEvent("wheel", {
          bubbles: true,
          cancelable: true,
          deltaY: -1,
          deltaMode: WheelEvent.DOM_DELTA_LINE,
        }),
      );
      expect(asked(setting, from)).toHaveLength(1);
    },
  );

  test.each(pills)(
    "$name: double-clicking the icon opens the value for typing",
    async ({ host, timeBase }) => {
      await userEvent.click(choice("time-base", timeBase));
      await userEvent.dblClick(iconOf(host));
      const input = fieldOf(host).querySelector("input");
      expect(input).not.toBeNull();
      expect(document.activeElement).toBe(input);
      await userEvent.keyboard("{Escape}");
    },
  );

  // The pill adds nothing on the value itself: a trackpad's stream of wheel events steps it as
  // far there as on the icon, not twice as far.
  test("a wheel stream steps the value as far on it as on the icon", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    const stream = (target: Element) => {
      for (let event = 0; event < 6; event++) {
        target.dispatchEvent(
          new WheelEvent("wheel", {
            bubbles: true,
            cancelable: true,
            deltaY: 60,
          }),
        );
      }
    };
    let from = app.sent.length;
    stream(iconOf("scale-field"));
    const onIcon = asked("verticalScale", from).length;
    await new Promise((resolve) => setTimeout(resolve, 200)); // A new stream.
    from = app.sent.length;
    stream(fieldOf("scale-field"));
    expect(onIcon).toBeGreaterThan(1);
    expect(asked("verticalScale", from)).toHaveLength(onIcon);
  });

  // While a value is being typed, a press on its pill ends the typing as one on the value does,
  // and drags nothing: what was typed is what is asked for.
  test("a press on the pill while typing keeps what was typed", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.dblClick(fieldOf("scale-field"));
    const input = fieldOf("scale-field").querySelector("input");
    expect(input).not.toBeNull();
    await userEvent.fill(input as HTMLInputElement, "-12 dB");
    const from = app.sent.length;
    const icon = iconOf("scale-field");
    const y = icon.getBoundingClientRect().top;
    pointer("pointerdown", icon, y);
    pointer("pointermove", fieldOf("scale-field"), y + 30);
    pointer("pointerup", fieldOf("scale-field"), y + 30);
    expect(asked("verticalScale", from)).toEqual([]);
    await userEvent.click(icon);
    expect(
      asked("verticalScale", from).map((message) => message.value),
    ).toEqual([-12]);
  });

  // The time pill holds the span and the beat span, one shown at a time: only the one shown
  // answers.
  test("in Seconds, the time pill steps the span and not the beat span", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    const from = app.sent.length;
    iconOf("span-field").dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        deltaY: -1,
        deltaMode: WheelEvent.DOM_DELTA_LINE,
      }),
    );
    expect(asked("timeSpan", from)).toHaveLength(1);
    expect(asked("beatSpan", from)).toHaveLength(0);
  });

  // A number field added to a pill is tested above too.
  test("covers every number field in a pill", () => {
    const inPills = Object.values(numberFields)
      .map((spec) => spec.host)
      .filter((host) => byId(host).closest(".icon-field") !== null);
    expect(inPills.length).toBeGreaterThan(0);
    expect(pills.map((pill) => pill.host)).toEqual(
      expect.arrayContaining(inPills),
    );
  });
});
