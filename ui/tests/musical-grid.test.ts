import { describe, expect, test } from "vitest";

import { musicalGrid, musicalPosition } from "../src/view/musical-grid.ts";

describe("musical grid", () => {
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
