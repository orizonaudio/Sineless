// What the lane labels say about the track named `name`: its chip on the screen, and, with Compare
// on, the match readout after its name.
import { byId } from "./harness.ts";

// The label of the lane whose track is named `name` (its whole name, though a long one is cut).
export function laneChip(name: string): HTMLElement | undefined {
  return [
    ...byId("lane-labels").querySelectorAll<HTMLElement>(".lane-label > span"),
  ].find((chip) => {
    const cut = chip.querySelector<HTMLElement>(".cut-name");
    return (
      (cut?.dataset["fullName"] ??
        cut?.querySelector(".cut-shown")?.textContent) === name
    );
  });
}

export function laneReadout(name: string): string {
  return laneChip(name)?.querySelector(".lane-readout")?.textContent ?? "";
}
