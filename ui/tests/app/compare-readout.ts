// What the Compare view says about the track named `name`, beside its checkbox.
import { byId } from "./harness.ts";

export function laneReadout(name: string): string {
  const check = [
    ...byId("lane-labels").querySelectorAll(".compare-check"),
  ].find((button) => button.textContent === name);
  return (
    check?.parentElement?.querySelector(".lane-readout")?.textContent ?? ""
  );
}
