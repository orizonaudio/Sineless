// A list of things to show, each with a check box: the Views popover's Channels (#425). A header
// says how many are ticked and offers None and All; the list is four rows tall and scrolls beyond
// that, at one width, so the popover does not grow with what it lists.
//
// The check boxes are native ones in a labelled group (WAI-ARIA APG, Checkbox). A row that cannot
// be changed now (the one that always shows, or an unticked one once as many are ticked as can
// be) is aria-disabled, not disabled: it keeps its place in the tab order, so a screen reader
// user finds it and hears why (APG, Focusability of disabled controls). The list changes nothing
// by itself: it asks, and shows what it is then given.

export interface CheckRow {
  readonly id: string;
  // The row's name, built for this row (so a long one can roll under the row's pointer).
  readonly name: (row: HTMLElement) => readonly Node[];
  // What the name is built from: the name is built again only when this changes.
  readonly nameKey: string;
  readonly colour: string; // A CSS colour, for the swatch.
  readonly checked: boolean;
  // Why the row cannot be changed now, said to a screen reader; undefined when it can.
  readonly locked?: string;
  // A note after the name ("this track").
  readonly tag?: string;
}

export interface CheckListOptions {
  // The element that labels the group (the row's "Channels").
  readonly labelledBy: string;
  readonly onToggle: (id: string, checked: boolean) => void;
  readonly onNone: () => void;
  readonly onAll: () => void;
}

interface RowNodes {
  readonly row: HTMLLabelElement;
  readonly input: HTMLInputElement;
  readonly swatch: HTMLElement;
  readonly name: HTMLElement;
  readonly tag: HTMLElement;
  nameKey: string | undefined;
}

let lists = 0;

export class CheckList {
  readonly element: HTMLElement;
  private readonly count: HTMLElement;
  private readonly list: HTMLElement;
  private readonly rows = new Map<string, RowNodes>();
  private readonly options: CheckListOptions;
  private readonly noteId: string;

  constructor(host: HTMLElement, options: CheckListOptions) {
    this.options = options;
    this.element = host;
    this.noteId = `check-list-note-${String(++lists)}`;
    host.classList.add("check-list");
    host.setAttribute("role", "group");
    host.setAttribute("aria-labelledby", options.labelledBy);
    const head = document.createElement("div");
    head.className = "check-head";
    this.count = document.createElement("span");
    this.count.className = "check-count";
    // Said when it changes, without moving the focus (WCAG 4.1.3).
    this.count.setAttribute("role", "status");
    const actions = document.createElement("span");
    actions.className = "check-actions";
    const action = (label: string, run: () => void) => {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = label;
      button.addEventListener("click", run);
      return button;
    };
    const divider = document.createElement("i");
    divider.textContent = "|";
    divider.setAttribute("aria-hidden", "true");
    actions.append(
      action("None", options.onNone),
      divider,
      action("All", options.onAll),
    );
    head.append(this.count, actions);
    this.list = document.createElement("div");
    this.list.className = "check-rows";
    host.append(head, this.list);
  }

  // Shows `rows`, in order. Rows are kept by id, so the one with the focus keeps it while the
  // list changes around it.
  show(rows: readonly CheckRow[]): void {
    const ids = new Set(rows.map((row) => row.id));
    for (const [id, nodes] of this.rows) {
      if (!ids.has(id)) {
        nodes.row.remove();
        this.rows.delete(id);
      }
    }
    rows.forEach((row, index) => {
      const nodes = this.rows.get(row.id) ?? this.build(row.id);
      if (this.list.children[index] !== nodes.row) {
        this.list.insertBefore(nodes.row, this.list.children[index] ?? null);
      }
      nodes.input.checked = row.checked;
      nodes.row.classList.toggle("on", row.checked);
      nodes.row.classList.toggle("locked", row.locked !== undefined);
      if (row.locked === undefined) {
        nodes.input.removeAttribute("aria-disabled");
        nodes.input.removeAttribute("aria-description");
      } else {
        nodes.input.setAttribute("aria-disabled", "true");
        nodes.input.setAttribute("aria-description", row.locked);
      }
      nodes.swatch.style.background = row.colour;
      if (nodes.nameKey !== row.nameKey) {
        nodes.nameKey = row.nameKey;
        nodes.name.replaceChildren(...row.name(nodes.row));
      }
      nodes.tag.textContent = row.tag ?? "";
    });
    const ticked = rows.filter((row) => row.checked).length;
    const text = `${String(ticked)}/${String(rows.length)} selected`;
    if (this.count.textContent !== text) {
      this.count.textContent = text;
    }
  }

  private build(id: string): RowNodes {
    const row = document.createElement("label");
    // Its name may be cut: it rolls while the pointer or the focus is on the row (cut-name.ts).
    row.className = "check-row names-cut";
    const input = document.createElement("input");
    input.type = "checkbox";
    input.className = "visually-hidden";
    const box = document.createElement("span");
    box.className = "check-box";
    box.setAttribute("aria-hidden", "true");
    const swatch = document.createElement("span");
    swatch.className = "check-swatch";
    swatch.setAttribute("aria-hidden", "true");
    const name = document.createElement("span");
    name.className = "check-name";
    const tag = document.createElement("span");
    tag.className = "check-tag";
    row.append(input, box, swatch, name, tag);
    // The box shows what the engine says is shown: a click asks, and never ticks by itself. In
    // its click the box already reads as the click would leave it, which is what is asked for.
    input.addEventListener("click", (event) => {
      const wanted = input.checked;
      event.preventDefault();
      if (input.getAttribute("aria-disabled") !== "true") {
        this.options.onToggle(id, wanted);
      }
    });
    const nodes: RowNodes = {
      row,
      input,
      swatch,
      name,
      tag,
      nameKey: undefined,
    };
    this.rows.set(id, nodes);
    return nodes;
  }
}
