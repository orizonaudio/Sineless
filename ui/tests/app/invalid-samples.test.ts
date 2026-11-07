// A track whose audio holds samples that are not finite (NaN or infinite), as from a broken plugin
// upstream: the page warns of them in every display mode, and names the lanes that hold them.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { FrameFlag, frameHeader } from "../../../protocol/src/frame.ts";
import type { ControlMessage } from "../../src/bridge/transport.ts";
import { MockTransport } from "../../src/bridge/mock-transport.ts";
import {
  beforeApp,
  byId,
  chips,
  choice,
  press,
  settle,
  showDisplay,
  startApp,
  swipe,
  typeInto,
} from "./harness.ts";

// A crowded state the mock does not make by itself, as the engine sends it: a group of 30 tracks,
// more than the lanes show (the lanes message says how many are hidden), and, frozen, held audio
// that does not cover the span (the frame's partial flag).
class Crowded extends MockTransport {
  override onMessage(handler: (message: ControlMessage) => void): void {
    super.onMessage((message) => {
      handler(message.type === "lanes" ? { ...message, hidden: 14 } : message);
    });
  }
  override onFrame(handler: (frame: ArrayBuffer) => void): void {
    super.onFrame((frame) => {
      const flags = new DataView(frame).getUint8(flagsOffset);
      if ((flags & FrameFlag.frozen) !== 0) {
        new DataView(frame).setUint8(flagsOffset, flags | FrameFlag.partial);
      }
      handler(frame);
    });
  }
}
const flagsOffset =
  frameHeader.fields.find((field) => field.name === "flags")?.offset ?? 1;

const chipTexts = () =>
  [...byId("chips").children].map((chip) => chip.textContent);

// The lane names marked as holding samples that are not finite.
const markedLanes = () =>
  [...byId("lane-labels").querySelectorAll(".lane-label > span")]
    .filter((name) => name.classList.contains("invalid"))
    .map((name) => name.textContent);

// The colour each marked lane's name is drawn in: in Compare, a track's name is its check box's.
const markedColours = () =>
  [...byId("lane-labels").querySelectorAll(".lane-label > span.invalid")].map(
    (name) =>
      getComputedStyle(name.querySelector(".compare-check") ?? name).color,
  );

// The warning colour, as the page computes it.
const warnColour = () => {
  const probe = document.createElement("span");
  probe.style.color = "var(--sl-warn)";
  document.body.append(probe);
  const colour = getComputedStyle(probe).color;
  probe.remove();
  return colour;
};

describe("a track sending samples that are not finite", () => {
  beforeApp(async () => {
    await startApp({ brokenSamples: 3 });
  });

  test.each(["Single", "Multi", "Compare"] as const)(
    "warns of them in %s",
    async (mode) => {
      await showDisplay(mode);
      await settle();
      expect(chipTexts()).toContain("Invalid samples");
    },
  );

  test("names this track's lane in the warning colour, and in Compare the sum's", async () => {
    await showDisplay("Multi");
    await press(choice("layout", "Stack"));
    await settle();
    expect(markedLanes()).toEqual(["Kick"]);
    expect(markedColours()).toEqual([warnColour()]);

    await showDisplay("Compare");
    await settle();
    const marked = markedLanes();
    expect(marked).toHaveLength(2);
    expect(marked[0]).toBe("Kick");
    expect(marked[1]).toMatch(/^Sum/u);
    expect(markedColours()).toEqual([warnColour(), warnColour()]);
  });

  // More states than the status bar's fixed area holds, which takes every state frozen can show,
  // with long values: invalid samples in Multi with 14 tracks hidden, frozen with a span longer than
  // the capture kept and not all of it held, panned back and zoomed past ten times. The first, the
  // problem, stays shown in yellow; the rest fold into one "+n" chip that still lists them, and
  // nothing runs outside the area, so nothing beside it moves.
  test("folds the states that don't fit into one chip that lists them", async () => {
    await startApp({}, new Crowded({ brokenSamples: 3, keptSeconds: 1 }));
    await press(choice("display-mode", "Multi"));
    await press(choice("time-base", "Free"));
    await typeInto("free-span-field", "4 s");
    if (byId("freeze").getAttribute("aria-pressed") !== "true") {
      await userEvent.click(byId("freeze"));
    }
    // Panned back once frozen, which adds the offset's chip.
    await expect.poll(chips).toContain("Frozen");
    swipe(-0.1);
    await settle();
    // Zoomed in to ten times or more, which the chip says in two digits.
    const trace = byId("trace").getBoundingClientRect();
    for (let step = 0; step < 10; step += 1) {
      byId("trace").dispatchEvent(
        new WheelEvent("wheel", {
          deltaY: -400,
          metaKey: true,
          clientX: trace.left + trace.width / 2,
          clientY: trace.top + trace.height / 2,
          bubbles: true,
          cancelable: true,
        }),
      );
    }
    await settle();
    await expect.poll(() => chips().length).toBe(7);
    await expect.poll(chips).toContain("Invalid samples");
    const states = chips();
    expect(states[0]).toBe("Invalid samples");

    const host = byId("chips");
    const more = host.querySelector<HTMLElement>(".chip-more");
    expect(more, "the +n chip").not.toBeNull();
    const folded = [...(more as HTMLElement).querySelectorAll(".chip")].map(
      (chip) => chip.textContent,
    );
    expect(folded.length).toBeGreaterThan(0);
    expect((more as HTMLElement).firstChild?.textContent).toBe(
      `+${String(folded.length)}`,
    );
    // Every state is shown or listed once, in order, the problem among the shown.
    const shown = [...host.children].filter((child) => child !== more);
    expect([...shown.map((child) => child.textContent), ...folded]).toEqual(
      states,
    );
    expect(shown[0]?.classList.contains("problem")).toBe(true);
    // For screen readers, "+n" says how many are folded and names each, and none of those shown.
    const label = more?.getAttribute("aria-label") ?? "";
    expect(label).toContain(String(folded.length));
    for (const state of folded) {
      expect(label).toContain(state);
    }
    for (const child of shown) {
      expect(label).not.toContain(child.textContent);
    }
    const area = host.getBoundingClientRect();
    for (const child of host.children) {
      const box = child.getBoundingClientRect();
      expect(box.left).toBeGreaterThanOrEqual(area.left - 0.5);
      expect(box.right).toBeLessThanOrEqual(area.right + 0.5);
    }
    // Resting the pointer on "+n" shows the folded states, whole, inside the window.
    const list = (more as HTMLElement).querySelector<HTMLElement>(".chip-list");
    expect(list, "the folded list").not.toBeNull();
    await expect.element(list as HTMLElement).not.toBeVisible();
    await userEvent.hover(more as HTMLElement);
    await expect.element(list as HTMLElement).toBeVisible();
    const box = (list as HTMLElement).getBoundingClientRect();
    expect(box.left).toBeGreaterThanOrEqual(0);
    expect(box.top).toBeGreaterThanOrEqual(0);
    expect(box.right).toBeLessThanOrEqual(window.innerWidth);
    expect(box.bottom).toBeLessThanOrEqual(window.innerHeight);
    for (const chip of (list as HTMLElement).querySelectorAll(".chip")) {
      await expect.element(chip as HTMLElement).toBeVisible();
    }
    await userEvent.unhover(more as HTMLElement);
    await userEvent.click(byId("freeze"));
  });
});
