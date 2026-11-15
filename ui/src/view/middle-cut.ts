// A long name cut in the middle to fit a width: its start, an ellipsis, its end
// ("Drum Bus Para…ush Right"). The start and the end are what tell similar tracks apart, which a
// cut at the end loses. No CSS cuts in the middle (csswg-drafts #3937 is still open), so the
// longest cut that fits is searched for, as Chromium's gfx::ElideText does (text_elider.cc): by
// halving, asking of each whole candidate whether it fits, so kerning and fallback fonts count.

export const ellipsis = "…";

// The share of the kept characters that come before the cut. A little over half: a name is read
// from its start, and track names mostly differ at the very end ("Left", "Right", "2").
const headShare = 0.55;

const segmenter = new Intl.Segmenter(undefined, { granularity: "grapheme" });

// The name's characters as a reader sees them: an emoji with its modifiers, or a letter with its
// combining marks, is one, and is never cut through.
function graphemes(text: string): string[] {
  return [...segmenter.segment(text)].map((part) => part.segment);
}

// The name with `kept` of its characters, the rest replaced by the ellipsis; spaces beside the
// cut are dropped, as they would only widen it.
function cutTo(parts: readonly string[], kept: number): string {
  const head = Math.ceil(kept * headShare);
  const tail = kept - head;
  return (
    parts.slice(0, head).join("").trimEnd() +
    ellipsis +
    parts
      .slice(parts.length - tail)
      .join("")
      .trimStart()
  );
}

// The name as it is shown where `fits` says what fits: whole when it does, else the longest
// middle cut that does; the ellipsis alone where nothing more fits.
export function middleCut(
  name: string,
  fits: (text: string) => boolean,
): string {
  if (fits(name)) {
    return name;
  }
  const parts = graphemes(name);
  let best = ellipsis;
  let low = 1;
  let high = parts.length - 1;
  while (low <= high) {
    const kept = Math.floor((low + high) / 2);
    const candidate = cutTo(parts, kept);
    if (fits(candidate)) {
      best = candidate;
      low = kept + 1;
    } else {
      high = kept - 1;
    }
  }
  return best;
}
