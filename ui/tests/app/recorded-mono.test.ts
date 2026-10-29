// A mono track on the real engine's frame: the snapshot says one input channel and the frame
// draws L alone, so the page offers and measures L, whatever views were asked for (L and R).
import { beforeAll, describe, expect, test } from "vitest";

import { ChannelView } from "../../../protocol/src/enums.ts";
import { formatDbfs, formatFrequency } from "../../src/view/format.ts";
import { byId, settle, startRecorded } from "./harness.ts";

describe("a recorded mono track, a full-scale 400 Hz sine", () => {
  beforeAll(async () => {
    await startRecorded("MonoSine400_48k");
  });

  test("labels the readouts Left, though the trigger source is Mid", async () => {
    await settle();
    await expect.poll(() => byId("readout-tag").textContent).toBe("Left");
  });

  test("lights L alone in the Views field, as the frame draws it", async () => {
    await expect
      .poll(() =>
        [...byId("views-summary").querySelectorAll(".on")].map(
          (letter) => letter.textContent,
        ),
      )
      .toEqual(["L"]);
    expect([...(window.orizonDebug?.lastFrame?.views.keys() ?? [])]).toEqual([
      ChannelView.left,
    ]);
  });

  test("reads the sine's levels and pitch", async () => {
    await expect.element(byId("peak")).toHaveTextContent(formatDbfs(1));
    await expect
      .element(byId("rms"))
      .toHaveTextContent(formatDbfs(Math.SQRT1_2));
    await expect
      .element(byId("frequency"))
      .toHaveTextContent(formatFrequency(400));
  });
});
