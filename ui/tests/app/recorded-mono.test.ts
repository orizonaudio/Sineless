// A mono track on the real engine's frame: the snapshot says one input channel and the frame
// draws L alone, so the page offers and measures L, whatever views were asked for (L and R).
import { describe, expect, test } from "vitest";

import { ChannelView } from "../../../protocol/src/enums.ts";
import { formatDbfs } from "../../src/view/format.ts";
import {
  beforeApp,
  channelRead,
  drawnLetters,
  settle,
  startRecorded,
} from "./harness.ts";

describe("a recorded mono track, a full-scale 400 Hz sine", () => {
  beforeApp(async () => {
    await startRecorded("MonoSine400_48k");
  });

  test("reads Left, though the trigger source is Mid", async () => {
    await settle();
    await expect.poll(channelRead).toBe("Left");
  });

  test("lights L alone in the Views field, as the frame draws it", async () => {
    await expect.poll(drawnLetters).toEqual(["L"]);
    expect([...(window.orizonDebug?.lastFrame?.views.keys() ?? [])]).toEqual([
      ChannelView.left,
    ]);
  });

  // What the Meter shows, which this recording keeps off: the frame's levels of the one channel,
  // not 6 dB lower as half of a stereo pair would read.
  test("measures the sine's levels on its one channel", async () => {
    await expect
      .poll(() => formatDbfs(window.orizonDebug?.lastFrame?.peak ?? Number.NaN))
      .toBe(formatDbfs(1));
    expect(formatDbfs(window.orizonDebug?.lastFrame?.rms ?? Number.NaN)).toBe(
      formatDbfs(Math.SQRT1_2),
    );
  });
});
