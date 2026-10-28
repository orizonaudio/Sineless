// Which controls the page enables and shows, decided from the settings the engine reports.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { brand } from "@brand/brand.ts";

import { DisplayMode } from "../../../protocol/src/enums.ts";

import {
  press,
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

  // The product's name and mark come from the brand folder the build takes (cmake/Branding.cmake).
  test("names the product beside its mark and in the title", () => {
    expect(byId("wordmark").textContent).toBe(brand.name);
    expect(document.title).toBe(brand.name);
    expect(byId("brand-mark").querySelector("svg")).not.toBeNull();
  });

  // One popover at a time: opening one closes the other, and a popover whose field the host hides
  // (a state change) closes with it and doesn't come back with the field.
  test("shows one popover at a time, and none for a hidden field", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.click(choice("display-mode", "Multi"));
    await userEvent.click(byId("group-details"));
    await expect.element(byId("group-popover")).toBeVisible();
    await userEvent.click(byId("trigger-details"));
    await expect.element(byId("trigger-popover")).toBeVisible();
    await expect.element(byId("group-popover")).not.toBeVisible();
    expect(byId("group-details").getAttribute("aria-expanded")).toBe("false");
    // From the keyboard too, where no press lands outside the open popover.
    byId("group-details").focus();
    await userEvent.keyboard("{Enter}");
    await expect.element(byId("group-popover")).toBeVisible();
    await expect.element(byId("trigger-popover")).not.toBeVisible();
    expect(byId("trigger-details").getAttribute("aria-expanded")).toBe("false");
    await userEvent.keyboard("{Enter}");
    await expect.element(byId("group-popover")).not.toBeVisible();

    await userEvent.click(byId("group-details"));
    await expect.element(byId("group-popover")).toBeVisible();
    app.deliver({ type: "stateChanged", name: "displayMode", value: "single" });
    await expect.element(byId("group-details")).not.toBeVisible();
    app.deliver({ type: "stateChanged", name: "displayMode", value: "multi" });
    await expect.element(byId("group-details")).toBeVisible();
    await expect.element(byId("group-popover")).not.toBeVisible();

    // Views has no field in Multi: its popover closes the same way.
    app.deliver({ type: "stateChanged", name: "displayMode", value: "single" });
    await expect.element(byId("views-details")).toBeVisible();
    await userEvent.click(byId("views-details"));
    await expect.element(byId("views-popover")).toBeVisible();
    app.deliver({ type: "stateChanged", name: "displayMode", value: "multi" });
    await expect.element(byId("views-details")).not.toBeVisible();
    app.deliver({ type: "stateChanged", name: "displayMode", value: "single" });
    await expect.element(byId("views-details")).toBeVisible();
    await expect.element(byId("views-popover")).not.toBeVisible();
    expect(byId("views-details").getAttribute("aria-expanded")).toBe("false");
    await userEvent.click(choice("display-mode", "Single"));
  });

  // The Group field names the group shown, whether the user picks it or the host restores it.
  test("names the current group in its field", async () => {
    const field = () => [
      byId("group-summary").textContent,
      byId("group-details").getAttribute("aria-label"),
    ];
    await userEvent.click(choice("display-mode", "Multi"));
    await press(choice("group", "B"));
    await expect.poll(field).toEqual(["B", "Group B"]);
    app.deliver({ type: "stateChanged", name: "group", value: "c" });
    await expect.poll(field).toEqual(["C", "Group C"]);
    // Another setting leaves it be.
    app.deliver({ type: "stateChanged", name: "triggerMode", value: "free" });
    await expect.poll(() => byId("trigger-summary").textContent).toBe("Free");
    expect(field()).toEqual(["C", "Group C"]);
    app.deliver({ type: "stateChanged", name: "triggerMode", value: "auto" });
    await press(choice("group", "A"));
    await expect.poll(field).toEqual(["A", "Group A"]);
    await userEvent.click(choice("display-mode", "Single"));
  });

  // The mode chip and the Trigger field name each trigger mode alike.
  test("names the trigger mode alike in its field and its chip", async () => {
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Sec"));
    for (const label of ["Normal", "Free", "Single", "Auto"]) {
      await press(choice("mode", label));
      await expect.poll(() => byId("trigger-summary").textContent).toBe(label);
      // The field's name for screen readers says the mode.
      expect(byId("trigger-details").getAttribute("aria-label")).toContain(
        label,
      );
      await expect.poll(chips).toContain(label);
    }
  });

  // Only the controls that apply are shown: a tempo-synced sweep has no trigger.
  test("Beats mode shows the beat span and hides the trigger", async () => {
    // In the Single trigger mode, so Re-arm would show in Seconds.
    await userEvent.click(choice("time-base", "Sec"));
    await press(choice("mode", "Single"));
    await expect.element(byId("arm")).toBeVisible();
    await userEvent.click(choice("time-base", "Beats"));
    await expect.element(byId("beat-span-field")).toBeVisible();
    await expect.element(byId("span-field")).not.toBeVisible();
    await expect.element(byId("trigger-group")).not.toBeVisible();
    await expect.element(byId("arm")).not.toBeVisible();

    await userEvent.click(choice("time-base", "Sec"));
    await expect.element(byId("span-field")).toBeVisible();
    await expect.element(byId("beat-span-field")).not.toBeVisible();
    await expect.element(byId("trigger-details")).toBeVisible();
    await expect.element(byId("arm")).toBeVisible();
    await press(choice("mode", "Auto"));
  });

  // The tag says which channel Peak, RMS and Freq measure (DESIGN.md 1.2.1): the trigger source in
  // Single and Seconds, Mid in Beats, this track's Mid in Multi and Compare.
  test("labels the readouts with the channel they measure", async () => {
    const tag = () => byId("readout-tag").textContent;
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Sec"));
    // Away from the default source, then back: the tag follows it.
    await press(choice("source", "S"));
    await expect.poll(tag).toBe("Side");
    await userEvent.click(choice("time-base", "Beats"));
    await expect.poll(tag).toBe("Mid");
    await userEvent.click(choice("display-mode", "Multi"));
    await expect.poll(tag).toBe("This track");
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Sec"));
    await press(choice("source", "M"));
    await expect.poll(tag).toBe("Mid");
  });

  // Peak, RMS and Freq on screen come from the last frame: until a frame measures the new time
  // base, the tag keeps naming the channel the one on screen measured.
  test("labels the readouts with the channel the frame on screen measured", async () => {
    const tag = () => byId("readout-tag").textContent;
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Sec"));
    await press(choice("source", "S"));
    await expect.poll(tag).toBe("Side");
    app.holdFrames(true);
    try {
      await new Promise((resolve) => setTimeout(resolve, 300));
      await userEvent.click(choice("time-base", "Beats"));
      await new Promise((resolve) => setTimeout(resolve, 300));
      expect(tag()).toBe("Side");
    } finally {
      app.holdFrames(false);
    }
    await expect.poll(tag).toBe("Mid");
    await userEvent.click(choice("time-base", "Sec"));
    await press(choice("source", "M"));
  });

  // The same while the display mode changes between Single, which measures the readout channel,
  // and each mode that measures this track's Mid, either way.
  test.each(
    Object.keys(DisplayMode)
      .filter((key) => key !== "single")
      .map((key) => key.charAt(0).toUpperCase() + key.slice(1))
      .flatMap((mode) => [
        ["Single", mode, "Side", "This track"],
        [mode, "Single", "This track", "Side"],
      ]),
  )(
    "labels the readouts measured in %s until a frame measures them in %s",
    async (from, to, before, after) => {
      const tag = () => byId("readout-tag").textContent;
      await userEvent.click(choice("display-mode", "Single"));
      await userEvent.click(choice("time-base", "Sec"));
      await press(choice("source", "S"));
      await userEvent.click(choice("display-mode", from));
      await expect.poll(tag).toBe(before);
      app.holdFrames(true);
      try {
        await new Promise((resolve) => setTimeout(resolve, 300));
        await userEvent.click(choice("display-mode", to));
        await new Promise((resolve) => setTimeout(resolve, 300));
        expect(tag()).toBe(before);
      } finally {
        app.holdFrames(false);
      }
      await expect.poll(tag).toBe(after);
      await userEvent.click(choice("display-mode", "Single"));
      await press(choice("source", "M"));
    },
  );

  test("Re-arm shows in the Single trigger mode only", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await press(choice("mode", "Auto"));
    await expect.element(byId("arm")).not.toBeVisible();
    await press(choice("mode", "Single"));
    await expect.element(byId("arm")).toBeVisible();
    await press(choice("mode", "Auto"));
    await expect.element(byId("arm")).not.toBeVisible();
  });

  test("Re-arm asks the engine to arm the trigger again", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await press(choice("mode", "Single"));
    const before = app.sent.length;
    await userEvent.click(byId("arm"));
    expect(app.sent.slice(before)).toContainEqual({ type: "arm" });
    await press(choice("mode", "Auto"));
  });

  test("the view toggles keep at least one view shown", async () => {
    await userEvent.click(choice("display-mode", "Single"));
    const pressed = (label: string) =>
      viewToggle(label).getAttribute("aria-pressed") === "true";
    // L alone.
    if (!pressed("L")) {
      await press(viewToggle("L"));
    }
    for (const label of ["R", "M", "S"]) {
      if (pressed(label)) {
        await press(viewToggle(label));
      }
      await expect.poll(() => pressed(label)).toBe(false);
    }
    await expect.poll(() => pressed("L")).toBe(true);

    // Hiding the last view shown asks nothing and changes nothing.
    const before = app.sent.length;
    await press(viewToggle("L"));
    await settle();
    expect(pressed("L")).toBe(true);
    expect(
      app.sent
        .slice(before)
        .filter((message) => message["name"] === "channelViews"),
    ).toEqual([]);

    await press(viewToggle("M"));
    await expect.poll(() => pressed("M")).toBe(true);
    await settle();
    expect(window.orizonDebug?.lastFrame?.views.size).toBe(2);
  });

  // Multi and Compare show each track's Mid: the channel views and the trigger source don't
  // apply, so they are hidden; the group applies only there.
  test("the multi-track views hide the channel views and trigger source", async () => {
    for (const mode of ["Multi", "Compare"]) {
      await userEvent.click(choice("display-mode", mode));
      await expect.element(byId("views-group")).not.toBeVisible();
      expect(byId("source-row").hidden).toBe(true);
      await expect.element(byId("group-group")).toBeVisible();
    }
    await userEvent.click(choice("display-mode", "Single"));
    await expect.element(byId("views-group")).toBeVisible();
    expect(byId("source-row").hidden).toBe(false);
    await expect.element(byId("group-group")).not.toBeVisible();
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
    await press(choice("mode", "Normal"));
    await settle();
    app.holdFrames(true);
    await expect.poll(chips, { timeout: 3000 }).toContain("Waiting");
    app.holdFrames(false);
    await expect.poll(chips, { timeout: 3000 }).not.toContain("Waiting");
    await press(choice("mode", "Auto"));
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

  // The Trigger field names the mode; its popover holds the rest.
  test("names the trigger mode in its field", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await press(choice("mode", "Normal"));
    await expect.poll(() => byId("trigger-summary").textContent).toBe("Normal");
    await press(choice("mode", "Auto"));
    await expect.poll(() => byId("trigger-summary").textContent).toBe("Auto");
  });
});
