import { describe, expect, test } from "vitest";

import {
  BeatDivision,
  GridDensity,
  beatSpanLengths,
} from "../../protocol/src/enums.ts";
import { gridFineBeatLineGap } from "../../protocol/src/limits.ts";
import type {
  BeatDivisionName,
  GridDensityName,
} from "../src/view/grid-settings.ts";
import {
  beatLadder,
  beatSpanLabels,
  beatSteps,
  musicalGrid,
  musicalPosition,
  parseBeatSpan,
  type BeatGridOptions,
} from "../src/view/musical-grid.ts";

// The amount a span label names, in bars: "1/4 bar" is 0.25, "16 bars" 16.
function barsOf(label: string): number {
  const [amount = ""] = label.split(" ");
  const [numerator = "", denominator = "1"] = amount.split("/");
  return Number(numerator) / Number(denominator);
}

// The beat span field shows a label per span the engine sweeps, in the order the protocol numbers
// them: a label naming another length would show one span and sweep another.
describe("beat span labels", () => {
  test("name each span's length in bars, one label per span", () => {
    expect(beatSpanLabels).toHaveLength(beatSpanLengths.length);
    beatSpanLengths.forEach((length, index) => {
      expect(barsOf(beatSpanLabels[index] ?? "")).toBeCloseTo(
        length.amount,
        12,
      );
    });
  });

  test("say bar for one bar or less, bars above", () => {
    for (const label of beatSpanLabels) {
      expect(label.endsWith(barsOf(label) > 1 ? " bars" : " bar")).toBe(true);
    }
  });
});

// Typing in the span field: a label, or its number alone ("16" for 16 bars, "1/4" for 1/4 bar).
describe("a typed beat span", () => {
  test.each(beatSpanLabels.map((label, index) => [label, index] as const))(
    "%s is read from its label, its number alone, and in other case or spacing",
    (label, index) => {
      const amount = label.split(" ")[0] ?? "";
      for (const text of [
        label,
        amount,
        ` ${amount.replace("/", " / ")} `,
        label.toUpperCase(),
        `${label} `,
      ]) {
        expect(parseBeatSpan(text), text).toBe(index);
      }
    },
  );

  test.each(["5", "1/16", "", "bar", "1/2 beat", "two bars"])(
    "%j names no span",
    (text) => {
      expect(parseBeatSpan(text)).toBeUndefined();
    },
  );
});

describe("musical grid", () => {
  test("draws nothing without a span or a bar to measure it by", () => {
    // As before the host has given a tempo or a time signature.
    for (const [span, bar] of [
      [0, 4],
      [8, 0],
      [-1, 4],
    ] as const) {
      expect(musicalGrid(0, span, bar)).toEqual({ lines: [], labels: [] });
    }
  });

  // The hover keeps one shape as the pointer moves: the sixteenth on whole beats too.
  test("keeps the sixteenth on whole beats when asked", () => {
    expect(musicalPosition(5, 4, true)).toBe("2.2.1");
    expect(musicalPosition(5.25, 4, true)).toBe("2.2.2");
    expect(musicalPosition(5, 4)).toBe("2.2");
  });

  test("counts bar.beat.sixteenth like a DAW", () => {
    expect(musicalPosition(0, 4)).toBe("1.1");
    expect(musicalPosition(5, 4)).toBe("2.2");
    expect(musicalPosition(5.25, 4)).toBe("2.2.2");
    expect(musicalPosition(3.5, 3.5)).toBe("2.1"); // 7/8
  });

  test("draws beat lines with stronger bar lines for multi-bar spans", () => {
    const { lines, labels } = musicalGrid(8, 8, 4); // Two bars from bar 3.
    expect(lines).toHaveLength(7);
    expect(
      lines.filter((line) => line.strong).map((line) => line.position),
    ).toEqual([0.5]);
    expect(labels.map((label) => label.text)).toEqual([
      "3.1",
      "3.3",
      "4.1",
      "4.3",
    ]);
  });

  test("draws bar lines with stronger four-bar groups over sixteen bars", () => {
    const { lines, labels } = musicalGrid(0, 64, 4);
    expect(lines).toHaveLength(15);
    expect(lines.filter((line) => line.strong)).toHaveLength(3);
    expect(lines[0]).toEqual({ position: 1 / 16, strong: false });
    expect(lines[3]).toEqual({ position: 4 / 16, strong: true });
    expect(labels.map((label) => label.text)).toEqual([
      "1.1",
      "5.1",
      "9.1",
      "13.1",
    ]);
  });

  test("draws sixteenth lines with stronger beats within a bar", () => {
    const { lines, labels } = musicalGrid(4, 2, 4); // Two beats from bar 2.
    expect(lines).toHaveLength(7);
    expect(
      lines.filter((line) => line.strong).map((line) => line.position),
    ).toEqual([0.5]);
    expect(labels.map((label) => label.text)).toEqual([
      "2.1",
      "2.1.3",
      "2.2",
      "2.2.3",
    ]);
  });

  test("refines the grid inside a zoomed view", () => {
    // One bar from bar 2, zoomed to its second beat: sixteenth lines, beat-relative labels.
    const { lines, labels } = musicalGrid(4, 4, 4, { start: 0.25, end: 0.5 });
    expect(lines.map((line) => line.position)).toEqual([0.25, 0.5, 0.75]);
    expect(labels[0]?.text).toBe("2.2");
  });

  test("labels sub-beat spans as fractions of a beat", () => {
    const { lines, labels } = musicalGrid(4, 0.25, 4);
    expect(lines).toHaveLength(3);
    expect(labels.map((label) => label.text)).toEqual([
      "2.1",
      "+1/16",
      "+1/8",
      "+3/16",
    ]);
  });

  test("label the project time under each label when timed", () => {
    // Two bars from bar 3 at 120 BPM: bar 3 starts 4 s into the project.
    const { labels } = musicalGrid(8, 8, 4, undefined, {
      cycleSeconds: 4,
      secondsPerQuarter: 0.5,
    });
    expect(labels[0]).toMatchObject({ text: "3.1", detail: "0:04.000" });
    expect(labels.at(-1)?.detail).toBe("0:07.000");
  });
});

const divisions = Object.keys(BeatDivision) as BeatDivisionName[];
const densities = Object.keys(GridDensity) as GridDensityName[];
const wide = 1e6;

const grid = (
  time: GridDensityName,
  division: BeatDivisionName = "regular",
  widthCss = wide,
): BeatGridOptions => ({ time, division, widthCss });

// Visible spans in each of Medium's span classes, in 4/4 and 3/4: sixteen bars, two bars, one bar,
// half a bar, a beat and a sixteenth.
const spans = [64, 8, 4, 3, 2, 1, 0.25, 48, 6];

describe("Beats' Time steps and Beat division", () => {
  test.each(divisions)(
    "the %s ladder runs longest first, from 16 bars to below 1/1024, each level once",
    (division) => {
      for (const bar of [4, 3, 3.5, 1, 0.75]) {
        const ladder = beatLadder(bar, division);
        expect(ladder[0]).toBe(16 * bar);
        expect(ladder.at(-1)).toBeLessThanOrEqual(1 / 1024);
        expect(ladder.at(-2)).toBeGreaterThan(1 / 1024);
        for (let index = 1; index < ladder.length; index++) {
          expect(ladder[index]).toBeLessThan(ladder[index - 1] ?? 0);
        }
        expect(ladder).toContain(bar);
        expect(ladder).toContain(1);
        expect(ladder).toContain(division === "triplets" ? 1 / 3 : 1 / 4);
      }
    },
  );

  test.each(
    divisions.flatMap((division) =>
      spans.flatMap((span) =>
        [4, 3].map((bar) => [division, span, bar] as const),
      ),
    ),
  )(
    "in %s, a %s quarter-note view of %s-beat bars: Coarse and Fine step one ladder level from Medium",
    (division, span, bar) => {
      const ladder = beatLadder(bar, division);
      const medium = beatSteps(span, bar, grid("medium", division)).step;
      const coarse = beatSteps(span, bar, grid("coarse", division));
      const fine = beatSteps(span, bar, grid("fine", division));
      // The levels around Medium's step, which may sit between two (a Regular half beat).
      const longer = ladder.filter((level) => level > medium * (1 + 1e-9));
      const shorter = ladder.filter((level) => level < medium * (1 - 1e-9));
      expect(coarse.step).toBeCloseTo(longer.at(-1) ?? 4 * medium, 9);
      expect(fine.step).toBeCloseTo(shorter[0] ?? medium, 9);
      // Strong lines one level coarser than the step drawn, and on its multiples.
      for (const steps of [coarse, fine]) {
        expect(steps.strongEvery).toBeGreaterThan(steps.step);
        expect(steps.strongEvery / steps.step).toBeCloseTo(
          Math.round(steps.strongEvery / steps.step),
          9,
        );
      }
    },
  );

  test("Regular keeps today's grid at Medium; Coarse draws bars where it drew beats", () => {
    // Two bars of 4/4: beats with strong bars; Coarse bars with strong 4-bar groups; Fine
    // sixteenths with strong beats.
    expect(beatSteps(8, 4, grid("medium"))).toEqual({
      step: 1,
      strongEvery: 4,
    });
    expect(beatSteps(8, 4, grid("coarse"))).toEqual({
      step: 4,
      strongEvery: 16,
    });
    expect(beatSteps(8, 4, grid("fine"))).toEqual({
      step: 0.25,
      strongEvery: 1,
    });
    // A bar of 3/4 halves the beat at Medium, as before; Coarse goes to the beat, Fine to 1/4.
    expect(beatSteps(3, 3, grid("medium"))).toEqual({
      step: 0.5,
      strongEvery: 3,
    });
    expect(beatSteps(3, 3, grid("coarse")).step).toBe(1);
    expect(beatSteps(3, 3, grid("fine")).step).toBe(0.25);
  });

  test("Fine draws Medium's step where its finer lines would come closer than its gap", () => {
    for (const division of divisions) {
      for (const span of spans) {
        const medium = beatSteps(span, 4, grid("medium", division));
        const finer = beatSteps(span, 4, grid("fine", division)).step;
        if (finer === medium.step) {
          continue;
        }
        // The width at which the finer step's lines stand exactly the gap apart.
        const edge = (gridFineBeatLineGap * span) / finer;
        expect(
          beatSteps(span, 4, grid("fine", division, edge * 1.01)).step,
        ).toBe(finer);
        expect(beatSteps(span, 4, grid("fine", division, edge * 0.99))).toEqual(
          medium,
        );
      }
    }
  });

  test("Triplets draws thirds of the beat, strong on the next coarser triplet level", () => {
    // A quarter of a 4/4 bar: lines every 1/6, strong on the thirds.
    expect(beatSteps(1, 4, grid("medium", "triplets"))).toEqual({
      step: 1 / 6,
      strongEvery: 1 / 3,
    });
    // Half a bar: thirds, strong on the beats.
    expect(beatSteps(2, 4, grid("medium", "triplets"))).toEqual({
      step: 1 / 3,
      strongEvery: 1,
    });
    const { lines, labels } = musicalGrid(4, 1, 4, undefined, undefined, {
      time: "medium",
      division: "triplets",
      widthCss: wide,
    });
    expect(lines.map((line) => line.position)).toEqual(
      [1, 2, 3, 4, 5].map((sixth) => expect.closeTo(sixth / 6, 9) as number),
    );
    expect(lines.filter((line) => line.strong)).toHaveLength(2);
    expect(labels.map((label) => label.text)).toEqual(["2.1", "+1/3", "+2/3"]);
  });

  test("Triplets labels name sixths and twelfths exactly, and sixteenths as positions", () => {
    const texts = (start: number, span: number) =>
      musicalGrid(
        start,
        span,
        4,
        undefined,
        undefined,
        grid("fine", "triplets"),
      ).labels.map((label) => label.text);
    // A third of a beat at Fine: lines and labels every twelfth, a quarter beat (a sixteenth) read
    // as a position.
    expect(texts(4, 1 / 3)).toEqual(["2.1", "+1/12", "+1/6", "2.1.2"]);
    // Half a beat around the middle of one: sixths, the half beat a position.
    expect(texts(4.25, 0.5)).toEqual(["+1/3", "2.1.3", "+2/3"]);
  });

  test("Triplets labels never step to two thirds of a beat", () => {
    for (const span of spans) {
      for (const time of densities) {
        const { labels } = musicalGrid(
          0,
          span,
          4,
          undefined,
          undefined,
          grid(time, "triplets"),
        );
        expect(labels.length).toBeLessThanOrEqual(5);
        expect(labels.some((label) => label.text === "+2/3")).toBe(
          labels.some((label) => label.text === "+1/3"),
        );
      }
    }
  });

  test("Triplets labels step from a third of a beat to the beat, so they stand on the beats", () => {
    // Half a bar of 4/4 and a bar of 3/4 draw thirds of the beat, too many to label each.
    const texts = (span: number, bar: number) =>
      musicalGrid(
        0,
        span,
        bar,
        undefined,
        undefined,
        grid("medium", "triplets"),
      ).labels.map((label) => label.text);
    expect(beatSteps(2, 4, grid("medium", "triplets")).step).toBeCloseTo(
      1 / 3,
      9,
    );
    expect(texts(2, 4)).toEqual(["1.1", "1.2"]);
    expect(texts(3, 3)).toEqual(["1.1", "1.2", "1.3"]);
  });

  test("Coarse at one bar in 4/4 draws no line inside a bar-aligned sweep", () => {
    // The issue's rule: where Medium draws beats, Coarse draws bars, which a one-bar sweep holds
    // only at its edges.
    expect(
      musicalGrid(0, 4, 4, undefined, undefined, grid("coarse")).lines,
    ).toEqual([]);
  });
});
