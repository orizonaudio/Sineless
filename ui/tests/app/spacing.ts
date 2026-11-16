// For tests of the space between the bars' controls (#409, #424): what a control draws, the spacing
// tokens as the page resolves them, and a host built against an older macOS SDK, emulated.

export const shownBox = (element: Element): DOMRect | undefined => {
  const box = element.getBoundingClientRect();
  return box.width > 0 && getComputedStyle(element).visibility !== "hidden"
    ? box
    : undefined;
};

// The element that draws a control's shape: the control itself, or its shown button. A wrapped
// field's wrapper can be wider than what it draws (#424), so spacing is measured between these.
export function surface(control: Element): HTMLElement {
  if (control.matches("button, .icon-field")) {
    return control as HTMLElement;
  }
  const button = [
    ...control.querySelectorAll<HTMLElement>(":scope .field-button"),
  ].find((candidate) => shownBox(candidate) !== undefined);
  return button ?? (control as HTMLElement);
}

// A spacing token in pixels, as the page resolves it.
export function token(name: "--sl-gap-inner" | "--sl-group-space"): number {
  return parseFloat(
    getComputedStyle(document.documentElement).getPropertyValue(name),
  );
}

// WebKit gives form controls a 2 px margin in hosts built against macOS SDKs before 13.3, a margin
// any margin the page sets replaces (WebCore's StyleAdjuster). A layered rule does the same here:
// it loses to every unlayered rule of the page's, so only a control the page leaves to the browser
// keeps it. Returns what removes it.
export function emulateOldHostMargins(): () => void {
  const style = document.createElement("style");
  style.textContent =
    "@layer old-host { button, input, select, textarea { margin: 2px; } }";
  document.head.append(style);
  return () => {
    style.remove();
  };
}
