// Two instances on one host track, one before an effect and one after: the host gives both the
// track's name and colour, so each lane shows its number after the name, wherever a lane is
// named, and says what the number is.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  beforeApp,
  byId,
  choice,
  press,
  setCompare,
  settle,
  startApp,
} from "./harness.ts";

beforeApp(async () => {
  // Kick (this track), Bass, the unnamed hi-hats, Kick 2, and a second instance on Bass's track.
  await startApp({ bassAfter: true });
});

// Each lane's name as shown (the name, and the number after it if any) and as a screen reader
// says it, from its label.
function names(): { shown: string; said: string }[] {
  return [...byId("lane-labels").querySelectorAll(".lane-label > span")].map(
    (chip) => {
      const named = chip.querySelector(".compare-check") ?? chip;
      const number = named.querySelector(".instance");
      const hidden = named.querySelector(".visually-hidden");
      const plain = [...named.childNodes]
        .concat([...(named.querySelector(".lane-name")?.childNodes ?? [])])
        .filter((node) => node.nodeType === Node.TEXT_NODE)
        .map((node) => node.textContent)
        .join("");
      return {
        shown: `${plain}${number === null ? "" : `[${number.textContent}]`}`,
        said: `${plain}${hidden?.textContent ?? ""}`,
      };
    },
  );
}

async function showMulti(): Promise<void> {
  await press(choice("time-base", "Trig"));
  await press(choice("display-mode", "Multi"));
  await press(choice("group", "A"));
  await press(choice("layout", "Stack"));
  await settle();
}

describe("two instances on one track", () => {
  test("are numbered after the track's name, and the other tracks keep plain names", async () => {
    await showMulti();
    await setCompare(false);
    await expect.poll(names).toEqual([
      { shown: "Kick", said: "Kick" },
      { shown: "Bass[1]", said: "Bass, instance 1" },
      { shown: "Track 3", said: "Track 3" },
      { shown: "Kick 2", said: "Kick 2" },
      { shown: "Bass[2]", said: "Bass, instance 2" },
    ]);
    // The number is part of the picture only: a screen reader hears the words instead.
    for (const number of byId("lane-labels").querySelectorAll(".instance")) {
      expect(number.getAttribute("aria-hidden")).toBe("true");
    }
  });

  test("keep their numbers on Compare's check boxes", async () => {
    await showMulti();
    await setCompare(true);
    await settle();
    await expect
      .poll(() => names().map((name) => name.said))
      .toEqual([
        "Kick",
        "Bass, instance 1",
        "Track 3",
        "Kick 2",
        "Bass, instance 2",
        "Sum",
      ]);
    await setCompare(false);
  });

  test("take two colours, the second a shade of the track's", async () => {
    await showMulti();
    const swatches = () =>
      [
        ...byId("lane-labels").querySelectorAll<HTMLElement>(
          ".lane-label > span",
        ),
      ].map((chip) => chip.style.getPropertyValue("--swatch"));
    await expect.poll(() => swatches()[1]).toBe("#4f8fe6");
    expect(swatches()[4]).not.toBe("#4f8fe6");
    expect(swatches()[4]).toMatch(/^#[0-9a-f]{6}$/u);
  });

  test("are numbered in the hover readout too", async () => {
    await showMulti();
    await userEvent.click(byId("hover-toggle"));
    await settle();
    const trace = byId("trace").getBoundingClientRect();
    // The last of five stacked lanes: the second instance on Bass's track.
    byId("trace").dispatchEvent(
      new PointerEvent("pointermove", {
        bubbles: true,
        buttons: 0,
        clientX: trace.left + trace.width / 2,
        clientY: trace.top + trace.height * 0.9,
      }),
    );
    const title = byId("hover-readout").querySelector("strong");
    expect(title?.querySelector(".instance")?.textContent).toBe("2");
    expect(title?.querySelector(".visually-hidden")?.textContent).toBe(
      ", instance 2",
    );
    await userEvent.click(byId("hover-toggle"));
  });
});
