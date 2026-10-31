// Where the editor can hold the pointer (window.orizonPointerHold), a value dragged with the mouse
// hides it from the press, as Live's own devices do, and goes on from the travel the editor reports,
// past the screen's edge; the pointer comes back where it was pressed. The page must neither lose nor repeat
// a step when it switches from the pointer's travel to the editor's, and must drag as before
// wherever the editor does not answer.
import { afterEach, beforeAll, describe, expect, test } from "vitest";

import type { SettingName } from "../../../protocol/src/settings.ts";
import type { PointerHold } from "../../src/components/pointer-hold.ts";
import { pixelsPerStep as step } from "../../src/components/drag-field.ts";
import { numberFields } from "../../src/view/fields.ts";
import { byId, inTimeBase, startApp, type Started } from "./harness.ts";

// The editor's hold as the page sees it, answering start() only when told to.
class FakeHold implements PointerHold {
  starts = 0;
  ends = 0;
  onTravel: ((travel: number, fine: boolean) => void) | undefined;

  start(): void {
    this.starts++;
  }

  end(): void {
    this.ends++;
  }

  // The editor reports travel, as it does once it holds the pointer.
  travel(travel: number, fine = false): void {
    this.onTravel?.(travel, fine);
  }
}

// Every number field, dragged on its value, and each pill's value, dragged on its icon.
const fields = (
  Object.entries(numberFields) as [SettingName, { host: string }][]
).map(([setting, { host }]) => ({ host, setting, name: host, onIcon: false }));
// Each pill's value, dragged on its icon, in the time base that shows it.
const pills = [
  { host: "span-field", setting: "timeSpan", timeBase: "Sec" },
  { host: "beat-span-field", setting: "beatSpan", timeBase: "Beats" },
  { host: "scale-field", setting: "verticalScale", timeBase: "Sec" },
  { host: "concert-pitch-field", setting: "concertPitch", timeBase: "Sec" },
] as const;
const drags = [
  ...fields.map((field) => ({ ...field, timeBase: "Sec" })),
  ...pills.map((pill) => ({
    ...pill,
    name: `${pill.host} (icon)`,
    onIcon: true,
  })),
];

function fieldOf(host: string): HTMLElement {
  return byId(host).querySelector(".field") as HTMLElement;
}

function pressTarget(host: string, onIcon: boolean): HTMLElement {
  if (!onIcon) {
    return fieldOf(host);
  }
  const icon = byId(host)
    .closest(".icon-field")
    ?.querySelector<HTMLElement>(".icon, i");
  expect(icon, `${host}'s icon`).toBeTruthy();
  return icon as HTMLElement;
}

interface PointerOptions {
  readonly pointerType?: string;
  readonly button?: number;
}

function pointer(
  type: string,
  target: Element,
  y: number,
  { pointerType = "mouse", button = 0 }: PointerOptions = {},
): void {
  target.dispatchEvent(
    new PointerEvent(type, {
      pointerId: 1,
      pointerType,
      bubbles: true,
      button,
      buttons: type === "pointerup" ? 0 : button === 2 ? 2 : 1,
      clientY: y,
    }),
  );
}

describe("a value dragged where the editor holds the pointer", () => {
  let app: Started;
  let hold: FakeHold;
  beforeAll(async () => {
    app = await startApp();
  });
  afterEach(() => {
    delete window.orizonPointerHold;
  });

  function install(): FakeHold {
    hold = new FakeHold();
    window.orizonPointerHold = hold;
    return hold;
  }

  const asked = (setting: SettingName, from: number) =>
    app.sent
      .slice(from)
      .flatMap((message) =>
        message.type === "setSetting" && message.name === setting
          ? [message.value as number]
          : [],
      );

  // Starts a mouse drag on the value and returns the press's y and where the drag is captured.
  function press(host: string, onIcon: boolean, options?: PointerOptions) {
    const y = 300;
    pointer("pointerdown", pressTarget(host, onIcon), y, options);
    return { y, field: fieldOf(host) };
  }

  test.each(drags)(
    "$name: holds the pointer from the press, once",
    async ({ host, onIcon, timeBase }) => {
      await inTimeBase(timeBase);
      install();
      const { y, field } = press(host, onIcon);
      expect(hold.starts).toBe(1);
      pointer("pointermove", field, y - step);
      expect(hold.starts).toBe(1);
      pointer("pointermove", field, y - 4 * step);
      expect(hold.starts).toBe(1);
      pointer("pointerup", field, y - 4 * step);
      expect(hold.ends).toBe(1);
    },
  );

  test.each(drags)(
    "$name: switches to the editor's travel without losing or repeating a step",
    async ({ host, onIcon, setting, timeBase }) => {
      await inTimeBase(timeBase);
      install();
      const from = app.sent.length;
      const { y, field } = press(host, onIcon);
      pointer("pointermove", field, y - step);
      expect(asked(setting, from)).toHaveLength(1);
      hold.travel(-step); // The travel the page already stepped for.
      hold.travel(-(2 * step - 1));
      expect(asked(setting, from)).toHaveLength(1);
      hold.travel(-2 * step);
      expect(asked(setting, from)).toHaveLength(2);
      // From now on the frozen pointer's position steps nothing.
      pointer("pointermove", field, y + 12 * step);
      expect(asked(setting, from)).toHaveLength(2);
      pointer("pointerup", field, y + 12 * step);
    },
  );

  test.each(drags)(
    "$name: steps both ways from the press",
    async ({ host, onIcon, setting, timeBase }) => {
      await inTimeBase(timeBase);
      install();
      const from = app.sent.length;
      const { y, field } = press(host, onIcon);
      pointer("pointermove", field, y + step);
      expect(asked(setting, from)).toHaveLength(1);
      hold.travel(step);
      hold.travel(-(step - 1)); // Back past the press, but not a step beyond it.
      expect(asked(setting, from)).toHaveLength(2);
      hold.travel(-step);
      expect(asked(setting, from)).toHaveLength(3);
      pointer("pointerup", field, y + step);
    },
  );

  test.each(drags)(
    "$name: a double-click holds and shows the pointer twice, then types",
    async ({ host, onIcon, timeBase }) => {
      await inTimeBase(timeBase);
      install();
      const target = pressTarget(host, onIcon);
      for (let click = 0; click < 2; click++) {
        const { y, field } = press(host, onIcon);
        pointer("pointerup", field, y);
      }
      target.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
      expect([hold.starts, hold.ends]).toEqual([2, 2]);
      const input = fieldOf(host).querySelector("input");
      expect(input).not.toBeNull();
      // A press while the value is typed only ends the typing: no hold.
      press(host, false);
      expect(hold.starts).toBe(2);
      input?.blur();
    },
  );

  test("a first report before any move steps nothing and replaces the pointer's travel", () => {
    install();
    const from = app.sent.length;
    const { y, field } = press("scale-field", false);
    hold.travel(0);
    pointer("pointermove", field, y - 4 * step);
    expect(asked("verticalScale", from)).toHaveLength(0);
    hold.travel(-step);
    expect(asked("verticalScale", from)).toHaveLength(1);
    pointer("pointerup", field, y - 4 * step);
  });

  test("Shift gives fine steps from the editor's travel", () => {
    install();
    const from = app.sent.length;
    const { y, field } = press("scale-field", false);
    pointer("pointermove", field, y + step);
    hold.travel(step);
    hold.travel(2 * step, true);
    const [coarse, fine] = asked("verticalScale", from);
    const scale = numberFields.verticalScale;
    expect(fine).toBe(scale?.step(coarse ?? 0, -1, true));
    pointer("pointerup", field, y + step);
  });

  test.each(["pointerup", "pointercancel", "lostpointercapture"])(
    "ends the hold on %s, and the next drag holds again",
    (ending) => {
      install();
      const from = app.sent.length;
      let { y, field } = press("scale-field", false);
      pointer("pointermove", field, y - step);
      const first = hold.onTravel;
      pointer(ending, field, y - step);
      expect(hold.ends).toBe(1);
      expect(hold.onTravel).toBeUndefined();
      // A report the first drag's editor sent late changes nothing.
      const afterFirst = asked("verticalScale", from).length;
      first?.(-5 * step, false);
      expect(asked("verticalScale", from)).toHaveLength(afterFirst);
      ({ y, field } = press("scale-field", false));
      pointer("pointermove", field, y - step);
      expect(hold.starts).toBe(2);
      expect(asked("verticalScale", from)).toHaveLength(afterFirst + 1);
      pointer("pointerup", field, y - step);
    },
  );

  // Where the editor cannot hold the pointer it never reports travel: the drag goes on as before.
  test("drags from the pointer until the editor reports travel", () => {
    install();
    const from = app.sent.length;
    const { y, field } = press("scale-field", false);
    pointer("pointermove", field, y - step);
    pointer("pointermove", field, y - 2 * step);
    expect(hold.starts).toBe(1);
    expect(asked("verticalScale", from)).toHaveLength(2);
    pointer("pointerup", field, y - 2 * step);
    expect(hold.ends).toBe(1);
  });

  test.each([
    { name: "touch", options: { pointerType: "touch" } },
    { name: "pen", options: { pointerType: "pen" } },
    { name: "the right button", options: { button: 2 } },
  ])(
    "holds nothing for a drag with $name, and does for the mouse",
    ({ options }) => {
      install();
      let { y, field } = press("scale-field", false, options);
      pointer("pointermove", field, y - 4 * step, options);
      pointer("pointerup", field, y - 4 * step, options);
      expect(hold.starts).toBe(0);
      ({ y, field } = press("scale-field", false));
      pointer("pointermove", field, y - 4 * step);
      pointer("pointerup", field, y - 4 * step);
      expect(hold.starts).toBe(1);
    },
  );

  test("drags from the pointer where the editor has no hold", () => {
    const from = app.sent.length;
    const { y, field } = press("scale-field", false);
    pointer("pointermove", field, y - 2 * step);
    pointer("pointerup", field, y - 2 * step);
    expect(asked("verticalScale", from)).toHaveLength(1);
  });
});
