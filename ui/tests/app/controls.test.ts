// Which controls the page enables and shows, decided from the settings the engine reports.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { DisplayMode } from "../../../protocol/src/enums.ts";

import {
  byId,
  choice,
  chips,
  settle,
  startApp,
  typeInto,
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

  // Each overlay the Display group toggles, and everything it puts over the screen. Turning one
  // off must leave nothing of it showing, wherever it was placed: an element left unplaced would
  // sit at the top of the screen. A new overlay belongs in this table.
  const overlays = [
    {
      name: "time cursors",
      toggle: "time-cursors",
      parts: () => [byId("cursor-a"), byId("cursor-b"), byId("cursor-readout")],
    },
    {
      name: "level cursors",
      toggle: "level-cursors",
      parts: () => [
        ...(byId("level-cursor-lines")
          .children as HTMLCollectionOf<HTMLElement>),
        byId("cursor-readout"),
      ],
    },
    {
      name: "hover readout",
      toggle: "hover-toggle",
      parts: () => [
        byId("hover-line"),
        byId("hover-readout"),
        byId("hover-cycle"),
      ],
      // It shows where the pointer is over the screen.
      show: () =>
        userEvent.hover(byId("screen"), { position: { x: 300, y: 80 } }),
    },
  ];
  const showing = (parts: HTMLElement[]) =>
    parts.filter((part) => part.isConnected && !part.hidden).length;

  test.each(
    // Every display mode, from the protocol's enum, by its button's label ("single" → "Single").
    Object.keys(DisplayMode)
      .map((key) => key.charAt(0).toUpperCase() + key.slice(1))
      .flatMap((mode) =>
        overlays.map((overlay) => [mode, overlay.name, overlay] as const),
      ),
  )(
    "in %s, turning the %s off leaves nothing of them on the screen",
    async (mode, _, overlay) => {
      await userEvent.click(choice("display-mode", mode));
      await settle();
      const toggle = byId(overlay.toggle);
      if (toggle.getAttribute("aria-pressed") === "true") {
        await userEvent.click(toggle);
      }
      await userEvent.click(toggle);
      await overlay.show?.();
      await expect.poll(() => showing(overlay.parts())).toBeGreaterThan(0);
      await userEvent.click(toggle);
      await overlay.show?.();
      await settle();
      expect(showing(overlay.parts())).toBe(0);
      await userEvent.click(choice("display-mode", "Single"));
    },
  );

  test("an engine error shows its message over the screen", async () => {
    app.deliver({ type: "error", code: "engineUnavailable", message: "" });
    await expect.element(byId("message")).toBeVisible();
    await expect
      .element(byId("message-title"))
      .toHaveTextContent("Display unavailable");
  });

  // The Trigger group's summary shows the level as its field does: a finer step than the
  // summary's own rounding would read the same as the coarse value next to it.
  test("sums up the trigger level as its field shows it", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    if (byId("trigger-popover").hidden) {
      await userEvent.click(byId("trigger-details"));
    }
    await typeInto("level-field", "0.253");
    await expect
      .poll(() => byId("trigger-summary").textContent)
      .toContain("+0.253");
  });
});
