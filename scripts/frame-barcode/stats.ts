// Counts, per refresh of a recording, what each barcode strip shows: new pictures (on time or
// presented late), repeats (nothing drawn for that refresh, or a picture drawn for it and shown
// late), pictures drawn and never shown, and what cannot be judged. The method is the
// frame-timing skill's (.claude/skills/frame-timing/SKILL.md); the rules are the plan's.
//
// Times: `displayTime` is the recorder's, in milliseconds; a strip's drawn-for value is the
// producer's time in barcodeTicksPerMillisecond, modulo the row's range. The two clocks may differ
// by any constant: lateness is judged against a baseline taken from the same recording.
import {
  barcodeDataBits,
  barcodeTicksPerMillisecond,
} from "../../protocol/src/limits.ts";
import type { StripRead } from "../../ui/src/render/frame-barcode.ts";

const range = 2 ** barcodeDataBits;

export interface Sample {
  // When the recorder says the window server showed this refresh, in milliseconds.
  readonly displayTime: number;
  // The recorder had no new image: the screen did not change, so every strip repeats.
  readonly idle: boolean;
  // One read per strip, in a fixed order; ignored when idle.
  readonly strips: readonly StripRead[];
}

export interface StripCounts {
  // A new picture whose lateness is within half a period of the baseline, later, or unjudged.
  newOnTime: number;
  newLate: number;
  newUnknown: number;
  // A refresh showing the last picture again: no picture was due for it (making, or nothing new),
  // one was and came late (presentation), or it cannot be told.
  repeatNothingDrawn: number;
  repeatPresentedLate: number;
  repeatUnknown: number;
  // Pictures drawn and never shown (presentation).
  lost: number;
  // Refreshes the recorder saw no change in (counted among the repeats too).
  idle: number;
  unreadable: number;
  // Refreshes across a gap in the recorder's own samples: what the screen did there is unknown.
  unknownAcrossGaps: number;
  // The picture count started again (the editor reopened, the switch read again).
  restarts: number;
}

export interface Counts {
  readonly periodMs: number;
  readonly strips: readonly StripCounts[];
  // Refreshes where every strip repeated: the same stall reaching all of them.
  readonly repeatedTogether: number;
}

// A recorder step longer than this many periods missed a refresh: jitter stays below it.
export const gapPeriods = 1.5;
// The baseline lateness: this percentile of the lateness of the pictures within
// baselineWindowMs either side, from at least baselinePictures of them. Delays only add, so a low
// percentile finds the pipeline's depth even when most pictures are late.
export const baselinePercentile = 0.1;
export const baselineWindowMs = 1000;
export const baselinePictures = 30;
// Pictures beyond twice the refreshes elapsed that a jump may still be: redraws between refreshes.
export const restartSlack = 2;

function median(values: readonly number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 1
    ? (sorted[middle] ?? 0)
    : ((sorted[middle - 1] ?? 0) + (sorted[middle] ?? 0)) / 2;
}

function emptyCounts(): StripCounts {
  return {
    newOnTime: 0,
    newLate: 0,
    newUnknown: 0,
    repeatNothingDrawn: 0,
    repeatPresentedLate: 0,
    repeatUnknown: 0,
    lost: 0,
    idle: 0,
    unreadable: 0,
    unknownAcrossGaps: 0,
    restarts: 0,
  };
}

// The nearest representative of `value` (modulo the range) to `reference`.
function nearest(value: number, reference: number): number {
  const difference = (((value - reference) % range) + range) % range;
  return reference + (difference > range / 2 ? difference - range : difference);
}

interface NewPicture {
  readonly sample: number;
  readonly displayTime: number;
  // Display time minus drawn-for, in ticks, modulo the range.
  readonly lateness: number;
  // Above the baseline, in ticks; undefined when no baseline can be had.
  excess?: number;
}

interface Repeat {
  readonly sample: number;
  readonly displayTime: number;
  // The next new picture in the same unbroken stretch, if any.
  next?: NewPicture;
  // Pictures were drawn between the last shown and the next and never shown: one was drawn for
  // this refresh and lost.
  lostAfter?: boolean;
}

function countStrip(
  samples: readonly Sample[],
  strip: number,
  periodMs: number,
  gapBefore: readonly number[],
  repeatedAt: Set<number>[],
): StripCounts {
  const counts = emptyCounts();
  const pictures: NewPicture[] = [];
  const repeats: Repeat[] = [];
  let pending: Repeat[] = [];
  let last: number | undefined;
  let lastShown = 0;
  // The stretch since the last judged picture is broken (a recorder gap, unreadable rows): a jump
  // across it is not counted lost, and repeats before it have no next picture to be judged by.
  let broken = true;
  const breakStretch = () => {
    broken = true;
    pending = [];
  };
  samples.forEach((sample, index) => {
    const missed = gapBefore[index] ?? 0;
    if (missed > 0) {
      counts.unknownAcrossGaps += missed;
      breakStretch();
    }
    if (sample.idle) {
      counts.idle++;
      if (last !== undefined && index > 0) {
        const repeat: Repeat = {
          sample: index,
          displayTime: sample.displayTime,
        };
        repeats.push(repeat);
        pending.push(repeat);
        repeatedAt[index]?.add(strip);
      }
      return;
    }
    const read = sample.strips[strip];
    if (read === undefined || "unreadable" in read) {
      counts.unreadable++;
      breakStretch();
      return;
    }
    const lateness =
      (((sample.displayTime * barcodeTicksPerMillisecond - read.drawnFor) %
        range) +
        range) %
      range;
    if (last === undefined) {
      last = read.picture;
      lastShown = sample.displayTime;
      pictures.push({
        sample: index,
        displayTime: sample.displayTime,
        lateness,
      });
      broken = false;
      return;
    }
    const step = (((read.picture - last) % range) + range) % range;
    if (step === 0) {
      const repeat: Repeat = { sample: index, displayTime: sample.displayTime };
      repeats.push(repeat);
      pending.push(repeat);
      repeatedAt[index]?.add(strip);
      return;
    }
    const picture: NewPicture = {
      sample: index,
      displayTime: sample.displayTime,
      lateness,
    };
    // At most one picture per refresh, and a redraw or two between: a longer jump than the time
    // allows is the count starting again (the editor reopened), however it lands modulo the range.
    const elapsed = Math.round((sample.displayTime - lastShown) / periodMs);
    if (step > 2 * elapsed + restartSlack) {
      counts.restarts++;
      pending = [];
    } else if (!broken) {
      counts.lost += step - 1;
      for (const repeat of pending) {
        repeat.next = picture;
        repeat.lostAfter = step > 1;
      }
    }
    pending = [];
    broken = false;
    last = read.picture;
    lastShown = sample.displayTime;
    pictures.push(picture);
  });

  const periodTicks = periodMs * barcodeTicksPerMillisecond;
  for (const picture of pictures) {
    const near = pictures.filter(
      (other) =>
        Math.abs(other.displayTime - picture.displayTime) <= baselineWindowMs,
    );
    if (near.length < baselinePictures) {
      continue;
    }
    const values = near
      .map((other) => nearest(other.lateness, picture.lateness))
      .sort((a, b) => a - b);
    const baseline =
      values[Math.floor(baselinePercentile * (values.length - 1))] ?? 0;
    picture.excess = picture.lateness - baseline;
  }
  // The first picture of a recording is shown, not judged: nothing before it says when it was due.
  for (const picture of pictures.slice(1)) {
    if (picture.excess === undefined) {
      counts.newUnknown++;
    } else if (picture.excess > periodTicks / 2) {
      counts.newLate++;
    } else {
      counts.newOnTime++;
    }
  }
  for (const repeat of repeats) {
    const next = repeat.next;
    if (repeat.lostAfter === true) {
      counts.repeatPresentedLate++;
      continue;
    }
    if (next?.excess === undefined) {
      counts.repeatUnknown++;
      continue;
    }
    // When the next picture would have been shown at the baseline lateness.
    const due = next.displayTime - next.excess / barcodeTicksPerMillisecond;
    if (due <= repeat.displayTime + periodMs / 2) {
      counts.repeatPresentedLate++;
    } else {
      counts.repeatNothingDrawn++;
    }
  }
  return counts;
}

export function countRefreshes(samples: readonly Sample[]): Counts {
  const steps = samples
    .slice(1)
    .map(
      (sample, index) =>
        sample.displayTime - (samples[index]?.displayTime ?? 0),
    );
  const periodMs = steps.length > 0 ? median(steps) : 0;
  const gapBefore = samples.map((sample, index) => {
    const step =
      index > 0
        ? sample.displayTime - (samples[index - 1]?.displayTime ?? 0)
        : 0;
    return periodMs > 0 && step > gapPeriods * periodMs
      ? Math.round(step / periodMs) - 1
      : 0;
  });
  const stripCount = Math.max(
    0,
    ...samples.map((sample) => sample.strips.length),
  );
  const repeatedAt = samples.map(() => new Set<number>());
  const strips = Array.from({ length: stripCount }, (_, strip) =>
    countStrip(samples, strip, periodMs, gapBefore, repeatedAt),
  );
  const repeatedTogether =
    stripCount > 1
      ? repeatedAt.filter((set) => set.size === stripCount).length
      : 0;
  return { periodMs, strips, repeatedTogether };
}
