// A mono track: the engine draws L alone, and the page offers only L.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { ChannelView } from "../../../protocol/src/enums.ts";
import {
  beforeApp,
  byId,
  choice,
  inTimeBase,
  press,
  settle,
  showViews,
  startApp,
  type Started,
  useReadout,
} from "./harness.ts";

describe("a mono track", () => {
  beforeApp(async () => {
    await startApp({ inputChannels: 1 });
  });

  test("offers L alone among the views", async () => {
    const toggles = [...byId("views").querySelectorAll("button")];
    expect(toggles.map((button) => button.textContent)).toEqual([
      "L",
      "R",
      "M",
      "S",
    ]);
    await expectOnlyLeftEnabled(toggles);
  });

  test("labels the readouts Left, whatever the trigger source", async () => {
    await press(choice("time-base", "Trig"));
    await expect.poll(() => byId("readout-tag").textContent).toBe("Left");
    // The Views field lights L alone, as the frame draws it.
    await expect
      .poll(() =>
        [...byId("views-summary").querySelectorAll(".on")].map(
          (letter) => letter.textContent,
        ),
      )
      .toEqual(["L"]);
    expect(byId("views-details").getAttribute("aria-label")).toBe("Views: L");
  });

  test("draws one lane when stacked, with the trigger marker in it", async () => {
    await press(choice("time-base", "Trig"));
    await press(choice("layout", "Stack"));
    await settle();
    expect([...(window.orizonDebug?.lastFrame?.views.keys() ?? [])]).toEqual([
      ChannelView.left,
    ]);
    // One lane: the marker sits within the whole screen's height, at the trigger level 0, the
    // middle.
    const screen = byId("screen").getBoundingClientRect();
    const marker = byId("trigger-marker").getBoundingClientRect();
    const middle = marker.top + marker.height / 2 - screen.top;
    expect(middle / screen.height).toBeCloseTo(0.5, 1);
  });
});

async function expectOnlyLeftEnabled(
  toggles: HTMLButtonElement[],
): Promise<void> {
  const [left, ...others] = toggles;
  await expect.element(left as HTMLButtonElement).toBeEnabled();
  for (const button of others) {
    await expect.element(button).toBeDisabled();
  }
}

// The host changes the track's channels with the editor open: the frames change at once, and the
// snapshot that reports it follows later. The Views field follows the frames.
describe("a track that changes channels", () => {
  let app: Started;
  beforeApp(async () => {
    app = await startApp({ inputChannels: 2 });
  });

  const lit = () =>
    [...byId("views-summary").querySelectorAll(".on")].map(
      (letter) => letter.textContent,
    );

  test("names the views the frames draw before any snapshot says", async () => {
    await press(choice("display-mode", "Single"));
    await press(choice("time-base", "Trig"));
    await expect.poll(lit).toEqual(["L", "R"]);
    app.transport.changeChannels(1);
    await expect.poll(lit).toEqual(["L"]);
    app.transport.changeChannels(2);
    await expect.poll(lit).toEqual(["L", "R"]);
  });

  // The new count relabels the Channel button and the view toggles by itself, with no control
  // touched: a mono track draws and measures L alone, so its popover lights L and offers nothing
  // else; stereo again, the channel chosen comes back.
  test("relabels the readouts and the toggles when the snapshot says", async () => {
    const tag = () => byId("readout-tag").textContent;
    const toggles = [...byId("views").querySelectorAll("button")];
    await press(choice("display-mode", "Single"));
    await inTimeBase("Trig");
    await press(choice("source", "R"));
    await showViews(["L", "S"]);
    await useReadout("S");
    await expect.poll(tag).toBe("Side");
    await userEvent.click(byId("readout-details"));
    await expect.element(byId("readout-popover")).toBeVisible();
    app.transport.changeChannels(1, true);
    await expect.poll(tag).toBe("Left");
    await expect
      .element(byId("readout-details"))
      .toHaveAttribute("aria-disabled", "false");
    await expect
      .element(choice("readout-source", "L"))
      .toHaveAttribute("aria-pressed", "true");
    for (const other of ["R", "M", "S"]) {
      await expect.element(choice("readout-source", other)).toBeDisabled();
    }
    await userEvent.click(byId("readout-details"));
    await expectOnlyLeftEnabled(toggles);
    app.transport.changeChannels(2, true);
    await expect.poll(tag).toBe("Side");
    await expect
      .element(byId("readout-details"))
      .toHaveAttribute("aria-disabled", "false");
    for (const button of toggles) {
      await expect.element(button).toBeEnabled();
    }
    await showViews(["L", "R"]);
    await press(choice("source", "L"));
  });
});
