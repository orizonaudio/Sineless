// Which controls the page enables and shows, decided from the settings the engine reports.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  byId,
  choice,
  chips,
  settle,
  startApp,
  type Started,
} from "./harness.ts";

function viewToggle(label: string): HTMLButtonElement {
  const button = [...byId("views").querySelectorAll("button")].find(
    (candidate) => candidate.textContent === label,
  );
  expect(button, `view ${label}`).toBeDefined();
  return button as HTMLButtonElement;
}

describe("controls", () => {
  let app: Started;
  beforeAll(async () => {
    app = await startApp();
  });

  test("Beats mode shows the beat span and disables the trigger", async () => {
    await userEvent.click(choice("time-base", "Beats"));
    await expect.element(byId("beat-span-field")).toBeVisible();
    await expect.element(byId("span-field")).not.toBeVisible();
    for (const button of byId("trigger-group").querySelectorAll("button")) {
      expect(button.disabled, button.textContent).toBe(true);
    }

    await userEvent.click(choice("time-base", "Sec"));
    await expect.element(byId("span-field")).toBeVisible();
    await expect.element(byId("beat-span-field")).not.toBeVisible();
    await expect.element(byId("trigger-details")).toBeEnabled();
  });

  test("Re-arm applies to the Single trigger mode only", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.click(choice("mode", "Auto"));
    await expect.element(byId("arm")).toBeDisabled();
    await userEvent.click(choice("mode", "Single"));
    await expect.element(byId("arm")).toBeEnabled();
    await userEvent.click(choice("mode", "Auto"));
    await expect.element(byId("arm")).toBeDisabled();
  });

  test("the view toggles keep at least one view shown", async () => {
    await userEvent.click(choice("display-mode", "Single"));
    const pressed = (label: string) =>
      viewToggle(label).getAttribute("aria-pressed") === "true";
    // L alone.
    if (!pressed("L")) {
      await userEvent.click(viewToggle("L"));
    }
    for (const label of ["R", "M", "S"]) {
      if (pressed(label)) {
        await userEvent.click(viewToggle(label));
      }
      await expect.poll(() => pressed(label)).toBe(false);
    }
    await expect.poll(() => pressed("L")).toBe(true);

    // Hiding the last view shown asks nothing and changes nothing.
    const before = app.sent.length;
    await userEvent.click(viewToggle("L"));
    await settle();
    expect(pressed("L")).toBe(true);
    expect(
      app.sent
        .slice(before)
        .filter((message) => message["name"] === "channelViews"),
    ).toEqual([]);

    await userEvent.click(viewToggle("M"));
    await expect.poll(() => pressed("M")).toBe(true);
    await settle();
    expect(window.orizonDebug?.lastFrame?.views.size).toBe(2);
  });

  test("the multi-track views disable the view toggles and trigger source", async () => {
    for (const mode of ["Multi", "Compare"]) {
      await userEvent.click(choice("display-mode", mode));
      for (const label of ["L", "R", "M", "S"]) {
        await expect.element(viewToggle(label)).toBeDisabled();
      }
      for (const button of byId("source").querySelectorAll("button")) {
        expect(button.disabled).toBe(true);
      }
    }
    await userEvent.click(choice("display-mode", "Single"));
    for (const label of ["L", "R", "M", "S"]) {
      await expect.element(viewToggle(label)).toBeEnabled();
    }
  });

  test("Freeze holds the view and says so", async () => {
    await userEvent.click(byId("freeze"));
    await expect
      .element(byId("freeze"))
      .toHaveAttribute("aria-pressed", "true");
    await expect.poll(chips).toContain("Frozen");
    await userEvent.click(byId("freeze"));
    await expect
      .element(byId("freeze"))
      .toHaveAttribute("aria-pressed", "false");
    await expect.poll(chips).not.toContain("Frozen");
  });

  test("Normal mode says it is waiting while no trace triggers", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.click(choice("mode", "Normal"));
    await settle();
    app.holdFrames(true);
    await expect.poll(chips, { timeout: 3000 }).toContain("Waiting");
    app.holdFrames(false);
    await expect.poll(chips, { timeout: 3000 }).not.toContain("Waiting");
    await userEvent.click(choice("mode", "Auto"));
  });

  test("an engine error shows its message over the screen", async () => {
    app.deliver({ type: "error", code: "engineUnavailable", message: "" });
    await expect.element(byId("message")).toBeVisible();
    await expect
      .element(byId("message-title"))
      .toHaveTextContent("Display unavailable");
  });
});
