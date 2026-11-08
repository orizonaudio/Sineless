// Which controls the page enables and shows, decided from the settings the engine reports.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { brand } from "@brand/brand.ts";

import { valueNames } from "../../../protocol/src/setting-values.ts";
import { settings } from "../../../protocol/src/settings.ts";
import { choiceLabels } from "../../src/view/choice-labels.ts";

import {
  beforeApp,
  byId,
  chips,
  choice,
  displays,
  press,
  settle,
  showDisplay,
  showViews,
  startApp,
  type Started,
  useReadout,
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
  beforeApp(async () => {
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
    await press(choice("time-base", "Trig"));
    await press(choice("display-mode", "Multi"));
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
    await press(choice("display-mode", "Single"));
  });

  // Multi's Views field names the group shown, whether the user picks it or the host restores it.
  test("names the current group in its field", async () => {
    const field = () => [
      byId("group-summary").textContent,
      byId("group-details").getAttribute("aria-label"),
    ];
    await press(choice("display-mode", "Multi"));
    await press(choice("group", "B"));
    await expect.poll(field).toEqual(["B·Σ", "Views: group B"]);
    app.deliver({ type: "stateChanged", name: "group", value: "c" });
    await expect.poll(field).toEqual(["C·Σ", "Views: group C"]);
    // Another setting leaves it be.
    app.deliver({ type: "stateChanged", name: "triggerMode", value: "single" });
    await expect.poll(() => shownValue("trigger").join("")).toBe("Single");
    expect(field()).toEqual(["C·Σ", "Views: group C"]);
    app.deliver({ type: "stateChanged", name: "triggerMode", value: "auto" });
    await press(choice("group", "A"));
    await expect.poll(field).toEqual(["A·Σ", "Views: group A"]);
    await press(choice("display-mode", "Single"));
  });

  // Compare is an On | Off choice in Multi's Views popover, not a display mode; the field shows Σ,
  // dimmed while Compare is off, since the choice is out of sight while the popover is closed.
  test("offers Compare as an On | Off choice in Multi's Views popover", async () => {
    const modes = [...byId("display-mode").querySelectorAll("button")].map(
      (button) => button.firstChild?.textContent,
    );
    expect(modes).toEqual(["Single", "Multi"]);
    const sumLit = () =>
      byId("group-summary").querySelector("span")?.classList.contains("on");
    const field = () => [
      byId("group-summary").textContent,
      sumLit(),
      byId("group-details").getAttribute("aria-label"),
    ];
    await press(choice("display-mode", "Multi"));
    await press(choice("group", "A"));
    await expect
      .element(choice("compare", "Off"))
      .toHaveAttribute("aria-pressed", "true");
    await expect.poll(field).toEqual(["A·Σ", false, "Views: group A"]);
    await userEvent.click(choice("compare", "On"));
    await expect
      .element(choice("compare", "On"))
      .toHaveAttribute("aria-pressed", "true");
    await expect
      .poll(field)
      .toEqual(["A·Σ", true, "Views: group A, Compare on"]);
    await press(choice("group", "B"));
    await expect
      .poll(field)
      .toEqual(["B·Σ", true, "Views: group B, Compare on"]);
    // The host restores it off.
    app.deliver({ type: "stateChanged", name: "compare", value: "off" });
    await expect
      .element(choice("compare", "Off"))
      .toHaveAttribute("aria-pressed", "true");
    await expect.poll(field).toEqual(["B·Σ", false, "Views: group B"]);
    await press(choice("group", "A"));
    await press(choice("display-mode", "Single"));
  });

  // The mode chip and the Trigger field name each trigger mode alike.
  test("names the trigger mode alike in its field and its chip", async () => {
    await press(choice("display-mode", "Single"));
    await press(choice("time-base", "Trig"));
    for (const label of ["Normal", "Single", "Auto"]) {
      await press(choice("mode", label));
      await expect.poll(() => shownValue("trigger").join("")).toBe(label);
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
    await press(choice("time-base", "Trig"));
    await press(choice("mode", "Single"));
    await expect.element(byId("arm")).toBeVisible();
    await press(choice("time-base", "Beats"));
    await expect.element(byId("beat-span-field")).toBeVisible();
    await expect.element(byId("span-field")).not.toBeVisible();
    await expect.element(byId("trigger-group")).not.toBeVisible();
    await expect.element(byId("arm")).not.toBeVisible();

    await press(choice("time-base", "Trig"));
    await expect.element(byId("span-field")).toBeVisible();
    await expect.element(byId("beat-span-field")).not.toBeVisible();
    await expect.element(byId("trigger-details")).toBeVisible();
    await expect.element(byId("arm")).toBeVisible();
    await press(choice("mode", "Auto"));
  });

  // The tag says which channel Peak and RMS measure (DESIGN.md 1.2.1): in Single the readout
  // source when drawn, else the first view drawn (Left with the default views, L and R), whatever
  // the trigger source; this track's Mid in Multi.
  test("labels the readouts with the channel they measure", async () => {
    const tag = () => byId("readout-tag").textContent;
    await press(choice("display-mode", "Single"));
    await press(choice("time-base", "Trig"));
    // Away from the default source: the tag stays.
    await press(choice("source", "R"));
    await expect.poll(tag).toBe("Left");
    await press(choice("time-base", "Beats"));
    await expect.poll(tag).toBe("Left");
    await press(choice("display-mode", "Multi"));
    await expect.poll(tag).toBe("This");
    await press(choice("display-mode", "Single"));
    await press(choice("time-base", "Trig"));
    await press(choice("source", "L"));
    await useReadout("R");
    await expect.poll(tag).toBe("Right");
    // Back to the default, Mid, which the default views do not draw: Left again.
    await showViews(["L", "R", "M"]);
    await useReadout("M");
    await showViews(["L", "R"]);
    await expect.poll(tag).toBe("Left");
  });

  // In Single the tag is a button that opens the readouts' popover above it, in the status bar,
  // kept inside the window; a press elsewhere closes it. In Multi, Compare off or on, which measures
  // this track's Mid, it is a label: it opens nothing, and switching there closes the popover.
  test.each(displays.filter((display) => display !== "Single"))(
    "chooses the readout channel in Single, not in %s",
    async (mode) => {
      const popover = byId("readout-popover");
      await press(choice("display-mode", "Single"));
      await settle();
      await expect
        .element(byId("readout-details"))
        .toHaveAttribute("aria-disabled", "false");
      await userEvent.click(byId("readout-details"));
      await expect.element(popover).toBeVisible();
      const button = byId("readout-details").getBoundingClientRect();
      const box = popover.getBoundingClientRect();
      expect(box.bottom).toBeLessThanOrEqual(button.top);
      // Aligned with the tag's left edge (DESIGN.md 1.5), not placed by script.
      expect(Math.abs(box.left - button.left)).toBeLessThan(1);
      expect(box.left).toBeGreaterThanOrEqual(8);
      expect(box.right).toBeLessThanOrEqual(window.innerWidth - 8);
      await userEvent.click(byId("screen"));
      await expect.element(popover).not.toBeVisible();

      await userEvent.click(byId("readout-details"));
      await expect.element(popover).toBeVisible();
      await showDisplay(mode);
      await expect.element(popover).not.toBeVisible();
      await settle();
      await expect
        .element(byId("readout-details"))
        .toHaveAttribute("aria-disabled", "true");
      // A user's click, which the test runner itself refuses on a disabled control.
      byId("readout-details").click();
      expect(popover.hidden).toBe(true);
      await press(choice("display-mode", "Single"));
    },
  );

  // Peak and RMS on screen come from the last frame: until a frame measures the new display
  // mode, between Single, which measures the readout channel, and each mode that measures this
  // track's Mid, either way, the tag keeps naming what the one on screen measured.
  test.each(
    displays
      .filter((display) => display !== "Single")
      .flatMap((mode) => [
        ["Single", mode, "Left", "This"] as const,
        [mode, "Single", "This", "Left"] as const,
      ]),
  )(
    "labels the readouts measured in %s until a frame measures them in %s",
    async (from, to, before, after) => {
      const tag = () => byId("readout-tag").textContent;
      await press(choice("display-mode", "Single"));
      await press(choice("time-base", "Trig"));
      await showDisplay(from);
      await expect.poll(tag).toBe(before);
      app.holdFrames(true);
      try {
        await new Promise((resolve) => setTimeout(resolve, 300));
        await showDisplay(to);
        await new Promise((resolve) => setTimeout(resolve, 300));
        expect(tag()).toBe(before);
      } finally {
        app.holdFrames(false);
      }
      await expect.poll(tag).toBe(after);
      await press(choice("display-mode", "Single"));
    },
  );

  test("Re-arm shows in the Single trigger mode only", async () => {
    await press(choice("time-base", "Trig"));
    await press(choice("mode", "Auto"));
    await expect.element(byId("arm")).not.toBeVisible();
    await press(choice("mode", "Single"));
    await expect.element(byId("arm")).toBeVisible();
    await press(choice("mode", "Auto"));
    await expect.element(byId("arm")).not.toBeVisible();
  });

  test("Re-arm asks the engine to arm the trigger again", async () => {
    await press(choice("time-base", "Trig"));
    await press(choice("mode", "Single"));
    const before = app.sent.length;
    await userEvent.click(byId("arm"));
    expect(app.sent.slice(before)).toContainEqual({ type: "arm" });
    await press(choice("mode", "Auto"));
  });

  test("the view toggles keep at least one view shown", async () => {
    await press(choice("display-mode", "Single"));
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

  // Multi, with Compare off and on, shows each track's Mid: the channel views and the trigger
  // source don't apply, so they are hidden; the group applies only there.
  test("the multi-track views hide the channel views and trigger source", async () => {
    for (const mode of ["Multi", "Compare"] as const) {
      await showDisplay(mode);
      await expect.element(byId("views-group")).not.toBeVisible();
      expect(byId("source-row").hidden).toBe(true);
      await expect.element(byId("group-group")).toBeVisible();
    }
    await press(choice("display-mode", "Single"));
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
    await press(choice("time-base", "Trig"));
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
    {
      name: "Meter",
      toggle: "meter-toggle",
      parts: () => [byId("cursor-readout")],
    },
  ];
  const showing = (parts: HTMLElement[]) =>
    parts.filter((part) => part.isConnected && !part.hidden).length;

  test.each(
    // Every display, from the protocol's enums (Multi with Compare off and on).
    displays.flatMap((mode) =>
      overlays.map((overlay) => [mode, overlay.name, overlay] as const),
    ),
  )(
    "in %s, turning the %s off leaves nothing of them on the screen",
    async (mode, _, overlay) => {
      await showDisplay(mode);
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
      await press(choice("display-mode", "Single"));
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
    await press(choice("time-base", "Trig"));
    await press(choice("mode", "Normal"));
    await expect.poll(() => shownValue("trigger").join("")).toBe("Normal");
    await press(choice("mode", "Auto"));
    await expect.poll(() => shownValue("trigger").join("")).toBe("Auto");
  });
});

// Display mode, time base and layout are fields like Group, Views and Trigger: a button naming
// the value, opening a popover with the choices (#359).
const choiceFields = [
  { setting: "displayMode", word: "Mode", id: "mode", host: "display-mode" },
  { setting: "timeBase", word: "Time", id: "time-base", host: "time-base" },
  { setting: "viewLayout", word: "Layout", id: "layout", host: "layout" },
] as const;

function labelOf(setting: keyof typeof choiceLabels, value: string): string {
  const labels: Readonly<Record<string, string>> = choiceLabels[setting];
  return labels[value] ?? value;
}

// The value a choice field's button shows: the one of its stacked values left visible.
function shownValue(id: string): string[] {
  return [...byId(`${id}-summary`).children]
    .filter((value) => getComputedStyle(value).visibility !== "hidden")
    .map((value) => value.textContent);
}

describe("choice fields", () => {
  beforeApp(async () => {
    await startApp();
  });

  test.each(
    choiceFields.flatMap((field) =>
      valueNames(settings[field.setting].values).map((value) => ({
        ...field,
        value,
        label: labelOf(field.setting, value),
      })),
    ),
  )(
    "$word: the button names $label, and its popover chooses it and stays open",
    async ({ setting, word, id, host, label }) => {
      const button = byId(`${id}-details`);
      const popover = byId(`${id}-popover`);
      // Another popover open: opening this one closes it.
      await userEvent.click(byId("about-details"));
      await expect.element(byId("about-popover")).toBeVisible();
      await userEvent.click(button);
      await expect.element(popover).toBeVisible();
      expect(button.getAttribute("aria-expanded")).toBe("true");
      await expect.element(byId("about-popover")).not.toBeVisible();

      await userEvent.click(choice(host, label));
      await expect
        .element(choice(host, label))
        .toHaveAttribute("aria-pressed", "true");
      await expect.poll(() => shownValue(id)).toEqual([label]);
      expect(button.querySelector("svg.icon")).not.toBeNull();
      expect(button.getAttribute("aria-label")).toBe(`${word}: ${label}`);
      // Choosing keeps it open, as Group's does; a press elsewhere closes it.
      await expect.element(popover).toBeVisible();
      await userEvent.click(byId("wordmark"));
      await expect.element(popover).not.toBeVisible();
      expect(button.getAttribute("aria-expanded")).toBe("false");

      // Button and popover share the setting's help.
      const group = byId(`${id}-group`);
      expect(group.dataset["help"]).toBe(helpTopicOf[setting]);
      expect(group.contains(popover)).toBe(true);

      // Back to the default, for the next case.
      const { values, defaultValue } = settings[setting];
      const restore = labelOf(setting, valueNames(values)[defaultValue] ?? "");
      await press(choice(host, restore));
      await userEvent.click(byId("wordmark"));
    },
  );
});

// Each choice field's popover labels its row, as the other popovers do; Compare offers Off before
// On, as the other switches read.
describe("choice field popovers", () => {
  beforeApp(async () => {
    await startApp();
  });

  // A row with a label, and in it the setting's choices, one per value, as the other popovers.
  test.each(choiceFields)(
    "$word's popover labels the row that offers every value",
    ({ setting, id, host }) => {
      const row = byId(`${id}-popover`).querySelector(".popover-row");
      expect(row?.querySelector("span")?.textContent.trim()).not.toBe("");
      expect(row?.contains(byId(host))).toBe(true);
      // Every value is offered, in whatever order the field lists them.
      expect(
        [...byId(host).querySelectorAll("button")]
          .map((button) => button.textContent)
          .sort(),
      ).toEqual(
        valueNames(settings[setting].values)
          .map((value) => labelOf(setting, value))
          .sort(),
      );
    },
  );

  test("Compare offers Off, then On", () => {
    expect(
      [...byId("compare").querySelectorAll("button")].map(
        (button) => button.textContent,
      ),
    ).toEqual(["Off", "On"]);
  });
});

// The help topic of each choice field.
const helpTopicOf = {
  displayMode: "displayMode",
  timeBase: "timeBase",
  viewLayout: "layout",
} as const;
