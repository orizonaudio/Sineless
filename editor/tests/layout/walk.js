// The layout probe's walk through the page (LayoutProbe.mm): the same user actions in the same
// order in every probe, the page's boxes measured after each. Run by callAsyncJavaScript, whose
// promise resolves to the text the probe prints: "step <name>" lines, each followed by one line per
// element that shows, "<path> <x> <y> <width> <height>". A wait that cannot end resolves to
// "stalled <what> <whether animation frames ran>" instead.

/* global document, window, performance, getComputedStyle, requestAnimationFrame, setTimeout,
   MouseEvent, KeyboardEvent -- the page's, where the probe runs this */

const timeout = 5000;

class Stall extends Error {}

const sleep = (ms) =>
  new Promise((resolve) => {
    setTimeout(resolve, ms);
  });

// Whether the page's animation frames run: the frames from the mock arrive only on them.
async function framesRan() {
  let count = 0;
  let running = true;
  const tick = () => {
    count += 1;
    if (running) {
      requestAnimationFrame(tick);
    }
  };
  requestAnimationFrame(tick);
  await sleep(500);
  running = false;
  return count > 0;
}

async function until(condition, what) {
  const deadline = performance.now() + timeout;
  while (!condition()) {
    if (performance.now() > deadline) {
      throw new Stall(`${what} ${(await framesRan()) ? "ran" : "stalled"}`);
    }
    await sleep(5);
  }
}

const frameCount = () => window.orizonDebug?.frameCount ?? 0;

// A frame counted after now: one the page received after the change it waits on was seen.
async function nextFrame(what) {
  const from = frameCount();
  await until(() => frameCount() > from, `frame after ${what}`);
}

const shows = (element) =>
  element.getClientRects().length > 0 &&
  getComputedStyle(element).visibility !== "hidden";

const byId = (id) => {
  const element = document.getElementById(id);
  if (element === null) {
    throw new Error(`no element #${id}`);
  }
  return element;
};

// An element's path from the nearest ancestor with an id.
const keyOf = (element) => {
  if (element.id !== "") {
    return `#${element.id}`;
  }
  const parent = element.parentElement;
  const index = parent === null ? 0 : [...parent.children].indexOf(element);
  return `${parent === null ? "" : keyOf(parent)}>${element.tagName.toLowerCase()}:${String(index)}`;
};

// Every element that shows, keyed by its path.
function measure() {
  const lines = [];
  for (const element of document.body.querySelectorAll("*")) {
    if (
      element.tagName === "SCRIPT" ||
      element.tagName === "STYLE" ||
      !shows(element)
    ) {
      continue;
    }
    const box = element.getBoundingClientRect();
    lines.push(
      `${keyOf(element)} ${String(box.x)} ${String(box.y)} ${String(box.width)} ${String(box.height)}`,
    );
  }
  return lines;
}

// The areas sized by what they show, which changes with where the mock's sweep is when a step is
// measured: the status chips ("Filling", "Partial") and the readout in the screen's corner (the
// Meter's values). One "varies" line each: the keys its boxes go by (its own and those of the
// elements with an id inside it) and the text it shows. The test compares an area's boxes only
// where both probes show the same text.
const contentSized = () => [
  byId("chips"),
  byId("cursor-readout").parentElement,
];

function varies(area) {
  const keys = [area, ...area.querySelectorAll("[id]")].map(keyOf).join(",");
  const text = [...area.querySelectorAll("*")]
    .filter((element) => element.children.length === 0 && shows(element))
    .map((element) => element.textContent.trim())
    .join("|");
  return `varies ${keys} ${text}`;
}

const output = [];
function step(name) {
  output.push(`step ${name}`, ...contentSized().map(varies), ...measure());
}

function detailsButtons() {
  return [...document.querySelectorAll("button[aria-expanded]")].filter(
    (button) =>
      shows(button) && button.getAttribute("aria-disabled") !== "true",
  );
}

async function openPopover(details) {
  if (details.getAttribute("aria-expanded") !== "true") {
    details.click();
    await until(
      () => details.getAttribute("aria-expanded") === "true",
      `open ${details.id}`,
    );
  }
}

async function closePopover(details) {
  if (details.getAttribute("aria-expanded") === "true") {
    details.click();
    await until(
      () => details.getAttribute("aria-expanded") === "false",
      `close ${details.id}`,
    );
  }
}

// The details button whose popover holds `container`.
function detailsFor(container) {
  const popover = container.closest(".popover");
  const details = popover?.parentElement?.querySelector(
    ":scope > button[aria-expanded]",
  );
  if (details === null || details === undefined) {
    throw new Error(`no popover button for #${container.id}`);
  }
  return details;
}

const optionsOf = (container) => [
  ...container.querySelectorAll(":scope > button"),
];
const chosen = (container) =>
  optionsOf(container).find(
    (button) => button.getAttribute("aria-pressed") === "true",
  );

// Chooses an option of a segmented control through its popover, as a user does, and waits for a
// frame carrying it: the store changes on the mock's reply, sent before any frame computed with
// the new value, so the frame counted after the change shows it.
async function choose(container, option) {
  const details = detailsFor(container);
  await openPopover(details);
  if (option.getAttribute("aria-pressed") !== "true") {
    option.click();
    await until(
      () => option.getAttribute("aria-pressed") === "true",
      `choose ${option.textContent}`,
    );
    await nextFrame(`choosing ${option.textContent}`);
  }
  await closePopover(details);
}

// The bars' own switches, not the options in popovers, which carry aria-pressed too.
function switches() {
  return [
    ...document.querySelectorAll(
      ".toolbar [aria-pressed], .statusbar [aria-pressed]",
    ),
  ].filter((element) => element.closest(".popover") === null && shows(element));
}

// Turns a switch to `on` and waits for the page to show it. Freeze's click changes nothing in the
// page: it toggles from the last frame, so the wait is for a frame that is frozen or not.
async function turn(element, on) {
  if (element.id === "freeze") {
    if ((window.orizonDebug?.lastFrame?.frozen === true) !== on) {
      element.click();
      await until(
        () => (window.orizonDebug?.lastFrame?.frozen === true) === on,
        `freeze ${String(on)}`,
      );
    }
    return;
  }
  if ((element.getAttribute("aria-pressed") === "true") !== on) {
    element.click();
    await until(
      () => (element.getAttribute("aria-pressed") === "true") === on,
      `${element.id} ${String(on)}`,
    );
    await nextFrame(element.id);
  }
}

const label = (option) => option.textContent.trim();

// In one state: every popover the page shows, opened; every switch it shows, on, then off.
async function sweep(state) {
  step(state);
  for (const details of detailsButtons()) {
    await openPopover(details);
    step(`${state}, ${details.id} open`);
    await closePopover(details);
  }
  for (const element of switches()) {
    const was =
      element.id === "freeze"
        ? window.orizonDebug?.lastFrame?.frozen === true
        : element.getAttribute("aria-pressed") === "true";
    await turn(element, !was);
    step(`${state}, ${element.id || label(element)} ${was ? "off" : "on"}`);
    await turn(element, was);
  }
}

// The settings each state is made of, as the page offers them.
const stateControls = ["display-mode", "time-base", "mode", "compare"];

// Every other choice in a popover, once per display mode, put back after. The trigger's slope and
// source are opened in the sweep, not chosen: they change no box.
const notChosen = new Set([...stateControls, "slope", "source"]);

async function eachOption(state) {
  for (const container of document.querySelectorAll(".popover .segmented")) {
    // Rows of the other display mode stay hidden even with their popover open.
    if (
      notChosen.has(container.id) ||
      container.closest(".popover-row")?.hidden === true
    ) {
      continue;
    }
    const original = chosen(container);
    for (const option of optionsOf(container)) {
      if (option.disabled || option === original) {
        continue;
      }
      await choose(container, option);
      step(`${state}, ${container.id} ${label(option)}`);
    }
    if (original !== undefined) {
      await choose(container, original);
    }
  }
}

// A field opened for typing shows its input; Escape closes it, as nothing in the walk moves focus.
async function typeInField(state) {
  const field = [...document.querySelectorAll(".toolbar .field")].find(shows);
  if (field === undefined) {
    return;
  }
  field.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
  await until(() => field.querySelector("input") !== null, "typing");
  step(`${state}, typing`);
  field
    .querySelector("input")
    .dispatchEvent(
      new KeyboardEvent("keydown", { key: "Escape", bubbles: true }),
    );
  await until(() => field.querySelector("input") === null, "end typing");
}

async function walk() {
  await document.fonts.ready;
  await until(() => frameCount() > 0, "first frame");
  const modes = byId("display-mode");
  const timeBases = byId("time-base");
  for (const mode of optionsOf(modes)) {
    await choose(modes, mode);
    const multi = byId("views-multi").classList.contains("on");
    for (const timeBase of optionsOf(timeBases)) {
      await choose(timeBases, timeBase);
      const triggerModes = shows(byId("trigger-group"))
        ? optionsOf(byId("mode"))
        : [undefined];
      for (const triggerMode of triggerModes) {
        if (triggerMode !== undefined) {
          await choose(byId("mode"), triggerMode);
        }
        const compares = multi ? optionsOf(byId("compare")) : [undefined];
        for (const compare of compares) {
          if (compare?.disabled === true) {
            continue;
          }
          if (compare !== undefined) {
            await choose(byId("compare"), compare);
          }
          const state = [mode, timeBase, triggerMode, compare]
            .filter((option) => option !== undefined)
            .map(label)
            .join(" ");
          await sweep(state);
        }
      }
    }
    await eachOption(label(mode));
    await typeInField(label(mode));
  }
  return output.join("\n");
}

// What the probe prints: the walk's steps, or why it could not finish. The probe runs this file as
// an async function's body followed by `return await report();`.
// eslint-disable-next-line no-unused-vars -- called by the probe
async function report() {
  try {
    return await walk();
  } catch (error) {
    if (error instanceof Stall) {
      return `stalled ${error.message}`;
    }
    return `failed ${String(error?.stack ?? error)}`;
  }
}
