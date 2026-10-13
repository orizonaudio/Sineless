// The golden lanes frame with its values as numbers: "NaN" in the file stands for no value, which
// JSON cannot hold.
import goldenLanesFile from "./data/golden-lanes-frame.json" with { type: "json" };

export function valueOf(value: number | string): number {
  return value === "NaN" ? Number.NaN : Number(value);
}

export const goldenLanes = {
  ...goldenLanesFile,
  header: {
    ...goldenLanesFile.header,
    frequency: valueOf(goldenLanesFile.header.frequency),
  },
  matches: goldenLanesFile.matches.map((match) => ({
    ...match,
    correlation: valueOf(match.correlation),
    offset: valueOf(match.offset),
  })),
};
