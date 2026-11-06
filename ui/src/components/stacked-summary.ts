// A field's value, shown among all its values stacked in one place with the others invisible: the
// field is as wide as its longest value as the page renders it, so a change moves nothing after
// it, and no width is written down to go stale when a label changes.
export class StackedSummary {
  private readonly values = new Map<string, HTMLElement>();

  constructor(host: HTMLElement, labels: Readonly<Record<string, string>>) {
    host.classList.add("choices");
    for (const [value, label] of Object.entries(labels)) {
      const span = document.createElement("span");
      span.textContent = label;
      host.append(span);
      this.values.set(value, span);
    }
  }

  show(value: string): void {
    for (const [choice, span] of this.values) {
      span.className = choice === value ? "on" : "";
    }
  }
}
