// A mono track: the engine draws L alone, and the page offers only L.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { ChannelView } from "../../../protocol/src/enums.ts";
import {
  beforeApp,
  byId,
  channelRead,
  choice,
  drawnLetters,
  inTimeBase,
  press,
  settle,
  showViews,
  startApp,
  type Started,
  trackRows,
  useReadout,
} from "./harness.ts";

describe("a mono track", () => {
  beforeApp(async () => {
    await startApp({ inputChannels: 1 });
  });

  test("offers L alone among the views", async () => {
    expect(trackRows("channels").map((row) => row.name)).toEqual([
      "Left",
      "Right",
      "Mid",
      "Side",
    ]);
    expectOnlyLeftTicked();
    // None and All change nothing: there is one channel.
    const before = byId("channels").querySelector(".check-count")?.textContent;
    expect(before).toBe("1 of 4 selected");
    for (const button of byId("channels").querySelectorAll("button")) {
      button.click();
    }
    await settle();
    expectOnlyLeftTicked();
  });

  test("reads Left, whatever the trigger source", async () => {
    await press(choice("time-base", "Trig"));
    await expect.poll(channelRead).toBe("Left");
    // The Views field lights L alone, as the frame draws it.
    await expect.poll(drawnLetters).toEqual(["L"]);
    expect(byId("views-details").getAttribute("aria-label")).toBe(
      "Views: 1 of 4 channels, measures Left",
    );
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

// Single's Channels list on a mono track: Left alone ticked, and no row to change, each saying
// why.
function expectOnlyLeftTicked(): void {
  const rows = trackRows("channels");
  expect(rows.map((row) => row.box.checked)).toEqual([
    true,
    false,
    false,
    false,
  ]);
  for (const row of rows) {
    expect(row.box.getAttribute("aria-disabled"), row.name).toBe("true");
  }
  expect(rows[1]?.box.getAttribute("aria-description")).toBe(
    "A mono track has one channel",
  );
}

// The host changes the track's channels with the editor open: the frames change at once, and the
// snapshot that reports it follows later. The Views field follows the frames.
describe("a track that changes channels", () => {
  let app: Started;
  beforeApp(async () => {
    app = await startApp({ inputChannels: 2 });
  });

  const lit = drawnLetters;

  test("names the views the frames draw before any snapshot says", async () => {
    await press(choice("display-mode", "Single"));
    await press(choice("time-base", "Trig"));
    await expect.poll(lit).toEqual(["L", "R"]);
    app.transport.changeChannels(1);
    await expect.poll(lit).toEqual(["L"]);
    app.transport.changeChannels(2);
    await expect.poll(lit).toEqual(["L", "R"]);
  });

  // The new count relabels the Views field, the Measure row and the Channels list by itself, with no
  // control touched: a mono track draws and measures L alone, so the Measure row lights L and offers
  // nothing else; stereo again, the channel chosen comes back.
  test("relabels the readouts and the Channels list when the snapshot says", async () => {
    await press(choice("display-mode", "Single"));
    await inTimeBase("Trig");
    await press(choice("source", "R"));
    await showViews(["L", "S"]);
    await useReadout("S");
    await expect.poll(channelRead).toBe("Side");
    await userEvent.click(byId("views-details"));
    await expect.element(byId("views-popover")).toBeVisible();
    app.transport.changeChannels(1, true);
    await expect.poll(channelRead).toBe("Left");
    await expect
      .element(choice("readout-source", "L"))
      .toHaveAttribute("aria-pressed", "true");
    await expect.element(choice("readout-source", "L")).toBeEnabled();
    for (const other of ["R", "M", "S"]) {
      await expect.element(choice("readout-source", other)).toBeDisabled();
    }
    await expect.poll(() => trackRows("channels")[3]?.box.checked).toBe(false);
    expectOnlyLeftTicked();
    app.transport.changeChannels(2, true);
    await expect.poll(channelRead).toBe("Side");
    for (const letter of ["L", "S"]) {
      await expect.element(choice("readout-source", letter)).toBeEnabled();
    }
    // The views asked for before it turned mono are ticked again.
    await expect
      .poll(() =>
        trackRows("channels")
          .filter((row) => row.box.checked)
          .map((row) => row.name),
      )
      .toEqual(["Left", "Side"]);
    expect(
      trackRows("channels")[1]?.box.getAttribute("aria-disabled"),
    ).toBeNull();
    await userEvent.click(byId("views-details"));
    await showViews(["L", "R"]);
    await press(choice("source", "L"));
  });
});
