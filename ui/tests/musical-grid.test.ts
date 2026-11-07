import { describe, expect, test } from "vitest";

import { beatSpanLengths } from "../../protocol/src/enums.ts";
import {
  beatSpanLabels,
  musicalGrid,
  musicalPosition,
  parseBeatSpan,
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
