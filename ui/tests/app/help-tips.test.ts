// A help tooltip waits for the pointer to rest. A drag is no rest: a tooltip shown mid-drag would
// cover the value being dragged, or float under nothing while a value drag hides the pointer.
import { afterAll, describe, expect, test, vi } from "vitest";
import { page, userEvent } from "vitest/browser";

import {
  defaultEditorWidth,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import { edge, HelpTips, offset } from "../../src/components/help-tips.ts";
import { helpTexts } from "../../src/view/help.ts";
import { beforeApp, byId, startApp } from "./harness.ts";

afterAll(async () => {
  await page.viewport(1200, 720);
});

// Moves the pointer over an element, `dx` px right of its centre.
function moveOver(id: string, buttons = 0, dx = 0): void {
  const target = byId(id);
  const box = target.getBoundingClientRect();
  target.dispatchEvent(
    new PointerEvent("pointermove", {
      pointerId: 1,
      bubbles: true,
      buttons,
      clientX: box.left + box.width / 2 + dx,
      clientY: box.top + box.height / 2,
    }),
  );
}

// The help switch explains itself even while help is off.
function moveOverHelpSwitch(buttons: number): void {
  moveOver("help", buttons);
}

// Longer than the tooltip's delay: one that would show has shown by then.
const pastDelay = () => new Promise((resolve) => setTimeout(resolve, 1500));

const tip = () => byId("help-tip");
const explainsHelpOn = () =>
  !tip().hidden && tip().textContent.includes(helpTexts.helpOn.lead);

describe("help tooltips", () => {
  beforeApp(async () => {
    await startApp();
  });

  test("appear once the pointer rests with no button held", async () => {
    const tip = byId("help-tip");
    moveOverHelpSwitch(0);
    // Timers run late in a busy browser: the wait outlasts the delay several times over.
    await vi.waitFor(
      () => {
        expect(tip.hidden).toBe(false);
      },
      { timeout: 5000 },
    );
  });

  test("do not appear while a button is held, as during a drag", async () => {
    const tip = byId("help-tip");
    moveOverHelpSwitch(1);
    expect(tip.hidden).toBe(true);
    await pastDelay();
    expect(tip.hidden).toBe(true);
  });
});

// FR-13: a click on ? that turns help on says so at once, by the switch, so a user who clicks it
// learns what it does without waiting on it; it goes the moment the user moves on to a control.
describe("turning help on", () => {
  // Help is off whenever the editor opens: each test starts one.
  async function turnOn(): Promise<void> {
    await startApp();
    await userEvent.click(byId("help"));
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "true");
  }

  test.each([
    { width: minimumEditorWidth, height: 420 },
    { width: defaultEditorWidth, height: 580 },
  ])(
    "explains it at once, above the switch and inside the window, at $width × $height",
    async ({ width, height }) => {
      await page.viewport(width, height);
      await turnOn();
      expect(explainsHelpOn()).toBe(true);
      const box = tip().getBoundingClientRect();
      const button = byId("help").getBoundingClientRect();
      expect(box.bottom).toBeLessThanOrEqual(button.top);
      // Just above it, right edges aligned.
      expect(button.top - box.bottom).toBeLessThan(button.height);
      expect(box.right).toBeCloseTo(button.right, -0.5);
      expect(box.left).toBeGreaterThanOrEqual(0);
      expect(box.right).toBeLessThanOrEqual(width);
      expect(box.top).toBeGreaterThanOrEqual(0);
    },
  );

  // A hand moves a little after a click: the explanation neither goes nor gives way to the
  // switch's own tip, which would drop "Click ? again".
  test("keeps the explanation through a small move over the switch", async () => {
    await turnOn();
    moveOver("help", 0, 3);
    await pastDelay();
    expect(explainsHelpOn()).toBe(true);
  });

  test("closes it when ? is clicked again, which turns help off", async () => {
    await turnOn();
    await userEvent.click(byId("help"));
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "false");
    expect(tip().hidden).toBe(true);
  });

  test("closes it on a click elsewhere", async () => {
    await turnOn();
    await userEvent.click(byId("screen"));
    expect(tip().hidden).toBe(true);
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "true");
  });

  // The expected path: the user moves to a control, rests, and sees help working.
  test("closes it when the pointer leaves the switch, and the next rest explains a control", async () => {
    await turnOn();
    byId("help").dispatchEvent(new PointerEvent("pointerleave"));
    expect(tip().hidden).toBe(true);
    moveOver("freeze");
    await vi.waitFor(
      () => {
        expect(tip().textContent).toContain(helpTexts.freeze.title);
      },
      { timeout: 5000 },
    );
  });

  // Turned on from the keyboard, the pointer is elsewhere: its first move takes the
  // explanation away and shows the control under it.
  test("from the keyboard, closes it at the first move over another control", async () => {
    await startApp();
    byId("help").focus();
    await userEvent.keyboard("{Enter}");
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "true");
    expect(explainsHelpOn()).toBe(true);
    moveOver("freeze");
    expect(explainsHelpOn()).toBe(false);
    await vi.waitFor(
      () => {
        expect(tip().textContent).toContain(helpTexts.freeze.title);
      },
      { timeout: 5000 },
    );
  });

  // The status bar's own controls each explain themselves, not the bar around them.
  test.each([
    ["meter-toggle", "meter"],
    ["grid-details", "grid"],
  ] as const)("explains %s with its own help", async (id, topic) => {
    await turnOn();
    moveOver(id);
    await vi.waitFor(
      () => {
        expect(tip().textContent).toContain(helpTexts[topic].lead);
      },
      { timeout: 5000 },
    );
  });

  // The Reads row explains the channel read, not the Views it sits in.
  test("explains the Reads row with the readout source's help", async () => {
    await turnOn();
    if (byId("views-popover").hidden) {
      await userEvent.click(byId("views-details"));
    }
    moveOver("readout-source");
    await vi.waitFor(
      () => {
        expect(tip().textContent).toContain(helpTexts.readoutSource.lead);
      },
      { timeout: 5000 },
    );
    await userEvent.click(byId("views-details"));
  });

  test("shows nothing when help is turned off", async () => {
    await turnOn();
    await userEvent.click(byId("help"));
    expect(tip().hidden).toBe(true);
    await pastDelay();
    expect(explainsHelpOn()).toBe(false);
  });
});

// A tip of its own, shown by its own HelpTips, so the app's tip and its help switch stay out of the
// way: `showAt` rests the pointer on `topic` at (x, y) and gives the tip once shown.
function ownTips(): {
  tip: HTMLElement;
  tips: HelpTips;
  showAt: (topic: string, x: number, y?: number) => Promise<HTMLElement>;
} {
  const tip = document.createElement("div");
  tip.className = "help-tip";
  tip.hidden = true;
  document.body.append(tip);
  const tips = new HelpTips(tip);
  tips.setEnabled(true);
  const target = document.createElement("div");
  document.body.append(target);
  return {
    tip,
    tips,
    showAt: async (topic, x, y = 20) => {
      document.dispatchEvent(new PointerEvent("pointerdown"));
      target.dataset["help"] = topic;
      target.dispatchEvent(
        new PointerEvent("pointermove", {
          bubbles: true,
          clientX: x,
          clientY: y,
        }),
      );
      await vi.waitFor(
        () => {
          expect(tip.hidden).toBe(false);
        },
        { timeout: 5000 },
      );
      return tip;
    },
  };
}

// A test that shows every topic waits out the hover delay for each: a second per topic, so a topic
// added does not bring it to the default timeout.
const everyTopicTimeout = Object.keys(helpTexts).length * 1000;

// The parts of a tip, in the order it shows them, by the class each part's element has.
const partOrder = ["help-lead", "help-choices", "help-actions", "help-note"];
const partsOf = (tip: HTMLElement) =>
  [...tip.children]
    .map((child) => partOrder.find((part) => child.classList.contains(part)))
    .filter((part) => part !== undefined);

// One reading order in every tip: what it is, its choices, what the hand does, then the limits.
describe("a help tip's parts", () => {
  beforeApp(async () => {
    await page.viewport(defaultEditorWidth, 580);
    await startApp();
    await userEvent.click(byId("help"));
  });

  async function restOn(element: Element): Promise<void> {
    document.dispatchEvent(new PointerEvent("pointerdown"));
    const box = element.getBoundingClientRect();
    element.dispatchEvent(
      new PointerEvent("pointermove", {
        bubbles: true,
        clientX: box.left + 2,
        clientY: box.top + 2,
      }),
    );
    await vi.waitFor(
      () => {
        expect(tip().hidden).toBe(false);
      },
      { timeout: 5000 },
    );
  }

  test(
    "come in order: lead, choices, actions, notes, each note as written",
    async () => {
      const { showAt } = ownTips();
      for (const [topic, text] of Object.entries(helpTexts)) {
        const shown = await showAt(topic, 10);
        const parts = partsOf(shown);
        expect(parts[0], topic).toBe("help-lead");
        expect(parts, topic).toEqual(
          [...parts].sort(
            (a, b) => partOrder.indexOf(a) - partOrder.indexOf(b),
          ),
        );
        // A topic's limits reach the tip as written; one without any shows none.
        const notes = [...shown.querySelectorAll(".help-note")].map(
          (note) => note.textContent,
        );
        expect(notes, topic).toEqual("notes" in text ? text.notes : []);
      }
    },
    everyTopicTimeout,
  );

  test("show the screen's gestures beside what each does", async () => {
    await restOn(byId("screen"));
    expect(partsOf(tip())).toContain("help-actions");
    const gestures = [...tip().querySelectorAll(".help-actions dt")];
    const effects = [...tip().querySelectorAll(".help-actions dd")];
    expect(gestures.length).toBeGreaterThan(1);
    expect(effects.length).toBe(gestures.length);
  });

  // The page runs in WKWebView on macOS and WebView2 on Windows; its help names the keys of the
  // platform it runs on.
  test("name the keys of the platform the page runs on", async () => {
    await restOn(byId("screen"));
    const gestures = [...tip().querySelectorAll(".help-actions dt")]
      .map((gesture) => gesture.textContent)
      .join(" ");
    expect(gestures).toContain(/Mac/u.test(navigator.platform) ? "⌘" : "Ctrl");
  });

  // Each field's tip names its choices as the field's popover does, in its order.
  test.each([
    ["mode-group", "display-mode"],
    ["time-base-group", "time-base"],
    ["layout-group", "layout"],
    ["trigger-group", "mode"],
  ])("of %s name the choices its popover shows", async (field, choices) => {
    await restOn(byId(field));
    const shown = [...tip().querySelectorAll(".help-choices dt")].map(
      (name) => name.textContent,
    );
    const offered = [...byId(choices).querySelectorAll("button")].map(
      (button) => button.textContent,
    );
    expect(shown).toEqual(offered);
  });

  // A popover row's tip names the choices its own buttons show.
  test.each(["slope", "grid-type", "grid-time", "grid-level", "beat-division"])(
    "of the %s row name the choices the row shows",
    async (choices) => {
      const row = byId(choices).closest("[data-help]");
      expect(row).not.toBeNull();
      await restOn(row ?? byId(choices));
      const shown = [...tip().querySelectorAll(".help-choices dt")].map(
        (name) => name.textContent,
      );
      const offered = [...byId(choices).querySelectorAll("button")].map(
        (button) => button.textContent,
      );
      expect(offered.length).toBeGreaterThan(1);
      expect(shown).toEqual(offered);
    },
  );

  // A trigger mode's button explains that mode, not another.
  test("of each trigger mode button explain that mode", async () => {
    const buttons = [...byId("mode").querySelectorAll("button")];
    expect(buttons.length).toBeGreaterThan(1);
    for (const button of buttons) {
      await restOn(button);
      expect(tip().querySelector(".help-title")?.textContent).toBe(
        button.textContent,
      );
    }
  });
});

// A tip larger than the editor at its smallest would be cut off by the window.
describe("a help tip's size", () => {
  // The editor's minimum height, `kMinimumHeight` in editor/include/editor/EditorCore.h (not a
  // shared limit). The "Help is on" tests above use 420 px for their own case.
  const minimumEditorHeight = 400;

  beforeApp(async () => {
    await page.viewport(minimumEditorWidth, minimumEditorHeight);
    await startApp();
  });

  // Every topic's own width, shown where it has the whole window to its right, narrowest first;
  // measured once, as each tip waits out the hover delay.
  let widths: [string, number][] | undefined;
  const widthsOf = async (
    showAt: ReturnType<typeof ownTips>["showAt"],
  ): Promise<[string, number][]> => {
    if (widths === undefined) {
      const measured: [string, number][] = [];
      for (const topic of Object.keys(helpTexts)) {
        measured.push([
          topic,
          (await showAt(topic, 10)).getBoundingClientRect().width,
        ]);
      }
      widths = measured.sort(([, a], [, b]) => a - b);
    }
    return widths;
  };

  test.each(Object.keys(helpTexts))(
    "keeps %s inside the editor at its minimum size wherever the pointer rests",
    async (topic) => {
      const { tip, showAt } = ownTips();
      const { innerWidth: width, innerHeight: height } = window;
      const places = [
        [1, 1],
        [width - 1, 1],
        [1, height - 1],
        [width - 1, height - 1],
        [width / 2, height / 2],
      ];
      for (const [x = 0, y = 0] of places) {
        const box = (await showAt(topic, x, y)).getBoundingClientRect();
        expect(
          box.left,
          `${topic} at ${String(x)}, ${String(y)}`,
        ).toBeGreaterThanOrEqual(0);
        expect(box.top).toBeGreaterThanOrEqual(0);
        expect(box.right).toBeLessThanOrEqual(width);
        expect(box.bottom).toBeLessThanOrEqual(height);
        expect(box.width).toBeLessThanOrEqual(300);
      }
      // A gesture reads on one line, beside what it does.
      const text = helpTexts[topic as keyof typeof helpTexts];
      const gestures = [...tip.querySelectorAll(".help-actions dt")];
      expect(gestures.length).toBe("actions" in text ? text.actions.length : 0);
      for (const gesture of gestures) {
        const line = parseFloat(getComputedStyle(gesture).lineHeight);
        expect(gesture.getBoundingClientRect().height).toBeLessThan(line * 1.5);
      }
      tip.remove();
    },
  );

  // A tip is placed by its size, measured before it moves: measured squeezed against where the
  // last tip stood, a long tip would be placed beside the pointer and stay squeezed there,
  // narrower and taller than it is anywhere else.
  test(
    "does not depend on where the tip before it stood",
    async () => {
      const { showAt } = ownTips();
      const byWidth = await widthsOf(showAt);
      const [narrow = "", narrowWidth = 0] = byWidth[0] ?? [];
      const [wide = "", wideWidth = 0] = byWidth.at(-1) ?? [];
      // The narrow tip, flipped left of a pointer at the right edge, leaves this much room after it.
      const room = 1 + offset + narrowWidth;
      expect(room).toBeLessThan(wideWidth);
      await showAt(narrow, window.innerWidth - 1);
      // A pointer where the wide tip fits to its right only if measured in that room.
      const x = window.innerWidth - edge - offset - room;
      const shown = await showAt(wide, x);
      expect(shown.getBoundingClientRect().width).toBeCloseTo(wideWidth, 0);
    },
    everyTopicTimeout,
  );

  // Turning help on from the switch pins "Help is on" above it, sized the same way.
  test("pins Help is on at its own width wherever the tip before it stood", async () => {
    const { tip, tips, showAt } = ownTips();
    const button = document.createElement("button");
    button.style.cssText = `position: fixed; right: ${String(edge)}px; bottom: ${String(edge)}px`;
    button.textContent = "?";
    document.body.append(button);
    tips.setEnabled(true, button);
    const own = tip.getBoundingClientRect().width;
    // The narrowest tip, flipped left of a pointer at the right edge, leaves less room than that.
    const [narrow = "", narrowWidth = 0] = (await widthsOf(showAt))[0] ?? [];
    expect(1 + offset + narrowWidth).toBeLessThan(own);
    await showAt(narrow, window.innerWidth - 1);
    tips.setEnabled(true, button);
    expect(tip.getBoundingClientRect().width).toBeCloseTo(own, 0);
    button.remove();
  });
});
