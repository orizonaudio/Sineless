// The About popover beside Help: the version the editor reports and, in a build with the brand, a
// way to the vendor's contact page, which the editor opens (DESIGN FR-13.1).
import { afterAll, describe, expect, test, vi } from "vitest";
import { page, userEvent } from "vitest/browser";

import { brand } from "@brand/brand.ts";

import {
  defaultEditorWidth,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import { helpTexts } from "../../src/view/help.ts";
import { byId, startApp } from "./harness.ts";

afterAll(async () => {
  await page.viewport(1200, 720);
});

async function openAbout(): Promise<void> {
  if (byId("about-popover").hidden) {
    await userEvent.click(byId("about-details"));
  }
  expect(byId("about-popover").hidden).toBe(false);
}

// Rests the pointer on an element, as the help tooltips wait for.
function restOn(target: HTMLElement): void {
  const box = target.getBoundingClientRect();
  target.dispatchEvent(
    new PointerEvent("pointermove", {
      pointerId: 1,
      bubbles: true,
      buttons: 0,
      clientX: box.left + box.width / 2,
      clientY: box.top + box.height / 2,
    }),
  );
}

describe("about", () => {
  test("shows the version the editor reports, and no version before it does", async () => {
    await startApp({ about: false });
    await openAbout();
    expect(byId("about-version").textContent).toBe("—");
    await startApp({ about: { version: "12.3.4", contact: true } });
    await openAbout();
    expect(byId("about-version").textContent).toBe("12.3.4");
  });

  test("names the product in the popover and the button's label", async () => {
    await startApp();
    expect(byId("about-name").textContent).toBe(brand.name);
    expect(byId("about-details").getAttribute("aria-label")).toBe(
      `About ${brand.name}`,
    );
  });

  // A build without the brand has no contact page: the editor says so, and the page offers none.
  test("offers About Us only once the editor says there is a contact page", async () => {
    await startApp({ about: false });
    await openAbout();
    expect(byId("contact-row").hidden).toBe(true);
    const app = await startApp({ about: { version: "1.0.0", contact: false } });
    await openAbout();
    expect(byId("contact-row").hidden).toBe(true);
    app.deliver({ type: "about", version: "1.0.0", contact: true });
    expect(byId("contact-row").hidden).toBe(false);
  });

  // As the popover's other rows: its name on the left, the control on the right, here a button
  // named after the vendor, whose page it opens.
  test("lays About Us out as a row, its button named after the vendor", async () => {
    await startApp();
    await openAbout();
    const row = byId("contact-row");
    expect(row.classList.contains("popover-row")).toBe(true);
    expect(row.firstElementChild?.textContent).toBe("About Us");
    expect(row.lastElementChild).toBe(byId("contact"));
    expect(byId("contact").textContent).toBe("Orizon Audio");
    expect(byId("contact").getBoundingClientRect().left).toBeGreaterThan(
      (row.firstElementChild as HTMLElement).getBoundingClientRect().right,
    );
  });

  test("About Us asks the editor to open the contact page, and closes the popover", async () => {
    const app = await startApp();
    await openAbout();
    await userEvent.click(byId("contact"));
    expect(app.sent.filter((message) => message.type === "contact")).toEqual([
      { type: "contact" },
    ]);
    expect(byId("about-popover").hidden).toBe(true);
    expect(byId("about-details").getAttribute("aria-expanded")).toBe("false");
  });

  // The same rules as the toolbar's popovers: one at a time, and a press elsewhere closes it.
  test("opens and closes like the toolbar's popovers", async () => {
    await startApp();
    const about = byId("about-popover");
    await userEvent.click(byId("about-details"));
    expect(about.hidden).toBe(false);
    expect(byId("about-details").getAttribute("aria-expanded")).toBe("true");
    await userEvent.click(byId("about-details"));
    expect(about.hidden).toBe(true);

    await userEvent.click(byId("about-details"));
    await userEvent.click(byId("views-details"));
    expect(about.hidden).toBe(true);
    expect(byId("views-popover").hidden).toBe(false);
    await userEvent.click(byId("about-details"));
    expect(byId("views-popover").hidden).toBe(true);
    expect(about.hidden).toBe(false);

    await userEvent.click(byId("screen"));
    expect(about.hidden).toBe(true);
  });

  // FR-13: with Help on, everything in the popover explains itself.
  test("with help on, explains About Us and the rest of the popover", async () => {
    await startApp();
    await userEvent.click(byId("help"));
    await openAbout();
    const tip = byId("help-tip");
    for (const [id, topic] of [
      ["contact", "contact"],
      ["about-version", "about"],
    ] as const) {
      restOn(byId(id));
      // Timers run late in a busy browser: the wait outlasts the delay several times over.
      await vi.waitFor(
        () => {
          expect(tip.hidden).toBe(false);
          expect(tip.textContent).toContain(helpTexts[topic].title);
        },
        { timeout: 5000 },
      );
    }
  });

  test.each([
    { width: minimumEditorWidth, height: 420 },
    { width: defaultEditorWidth, height: 580 },
  ])(
    "lies inside the window at $width × $height",
    async ({ width, height }) => {
      await page.viewport(width, height);
      await startApp();
      await openAbout();
      const box = byId("about-popover").getBoundingClientRect();
      expect(box.left).toBeGreaterThanOrEqual(0);
      expect(box.top).toBeGreaterThanOrEqual(0);
      expect(box.right).toBeLessThanOrEqual(width);
      expect(box.bottom).toBeLessThanOrEqual(
        byId("about-details").getBoundingClientRect().top,
      );
    },
  );
});
