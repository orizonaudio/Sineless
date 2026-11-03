// What a screen recording with the frame barcode shows, per strip and per refresh: the command the
// frame-timing skill runs.
//
//   node scripts/frame-barcode/analyse.ts recording.jsonl   from record.sh (ScreenCaptureKit)
//   node scripts/frame-barcode/analyse.ts movie.mov         a QuickTime or other recording
//   node scripts/frame-barcode/analyse.ts --locate <width> <height> <frame.gray>...
//       The strips in whole frames saved by record.swift grab, as JSON for record.swift record.
//
// A movie has no record of the recorder's own idle refreshes, and its timestamps are the file's:
// the recorder's gaps still read as unknown, but a repeat on screen and an idle refresh are told
// apart only by record.sh.
import { spawn, spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";

import { barcodeCellPixels } from "../../protocol/src/limits.ts";
import {
  barcodeRowCells,
  decodeStrip,
} from "../../ui/src/render/frame-barcode.ts";
import {
  locateStrips,
  sampleStrip,
  type GreyFrame,
  type StripPlace,
} from "./locate.ts";
import { countRefreshes, type Counts, type Sample } from "./stats.ts";

// One line of record.sh's recording after its header: when the window server showed the refresh,
// in ms of the recorder's clock, and either idle (nothing changed) or each strip's two rows' greys.
interface RecordedLine {
  readonly t: number;
  readonly idle?: boolean;
  readonly cells?: readonly (readonly [readonly number[], readonly number[]])[];
}

export function parseRecording(text: string): Sample[] {
  const lines = text.split("\n").filter((line) => line.trim() !== "");
  // The first line is the header: the display and the strips' places.
  return lines.slice(1).map((line) => {
    const recorded = JSON.parse(line) as RecordedLine;
    return {
      displayTime: recorded.t,
      idle: recorded.idle === true,
      strips: (recorded.cells ?? []).map(([first, second]) =>
        decodeStrip(first, second),
      ),
    };
  });
}

const percent = (count: number, of: number) =>
  of === 0 ? "—" : `${((100 * count) / of).toFixed(1)}%`;

export function formatCounts(counts: Counts): string {
  const lines = [
    `Recorder period ${counts.periodMs.toFixed(3)} ms (${(1000 / counts.periodMs).toFixed(2)} Hz)`,
  ];
  counts.strips.forEach((strip, index) => {
    const judged =
      strip.newOnTime +
      strip.newLate +
      strip.newUnknown +
      strip.repeatNothingDrawn +
      strip.repeatPresentedLate +
      strip.repeatUnknown;
    lines.push(
      "",
      `Strip ${String(index + 1)}: ${String(judged)} refreshes judged`,
      `  new, on time                 ${String(strip.newOnTime).padStart(7)}  ${percent(strip.newOnTime, judged)}`,
      `  new, presented late          ${String(strip.newLate).padStart(7)}  ${percent(strip.newLate, judged)}`,
      `  new, lateness unknown        ${String(strip.newUnknown).padStart(7)}`,
      `  repeat, nothing drawn        ${String(strip.repeatNothingDrawn).padStart(7)}  ${percent(strip.repeatNothingDrawn, judged)}   (making, or nothing new)`,
      `  repeat, presented late       ${String(strip.repeatPresentedLate).padStart(7)}  ${percent(strip.repeatPresentedLate, judged)}   (presentation)`,
      `  repeat, unknown              ${String(strip.repeatUnknown).padStart(7)}`,
      `  pictures never shown         ${String(strip.lost).padStart(7)}             (presentation)`,
      `  idle refreshes (recorder)    ${String(strip.idle).padStart(7)}`,
      `  unreadable                   ${String(strip.unreadable).padStart(7)}`,
      `  unknown, recorder gaps       ${String(strip.unknownAcrossGaps).padStart(7)}`,
      `  restarts                     ${String(strip.restarts).padStart(7)}`,
    );
  });
  if (counts.strips.length > 1) {
    lines.push(
      "",
      `Refreshes where every strip repeated together: ${String(counts.repeatedTogether)}`,
    );
  }
  return lines.join("\n");
}

// A movie's frames, decoded to grey at their own size, with their timestamps in ms.
export async function movieSamples(
  file: string,
  cell: number,
): Promise<Sample[]> {
  const probe = spawnSync(
    "ffprobe",
    [
      "-v",
      "error",
      "-select_streams",
      "v:0",
      "-show_entries",
      "stream=width,height:frame=best_effort_timestamp_time",
      "-of",
      "json",
      file,
    ],
    { encoding: "utf8", maxBuffer: 1 << 28 },
  );
  if (probe.status !== 0) {
    throw new Error(`ffprobe failed: ${probe.stderr}`);
  }
  const info = JSON.parse(probe.stdout) as {
    streams: { width: number; height: number }[];
    // ffprobe's own field names.
    frames: Record<string, string | undefined>[];
  };
  const width = info.streams[0]?.width ?? 0;
  const height = info.streams[0]?.height ?? 0;
  const times = info.frames.map(
    (frame) => 1000 * Number(frame["best_effort_timestamp_time"] ?? Number.NaN),
  );
  const size = width * height;
  const decoder = spawn(
    "ffmpeg",
    [
      "-v",
      "error",
      "-i",
      file,
      "-fps_mode",
      "passthrough",
      "-f",
      "rawvideo",
      "-pix_fmt",
      "gray",
      "-",
    ],
    { stdio: ["ignore", "pipe", "inherit"] },
  );
  const samples: Sample[] = [];
  const opening: GreyFrame[] = [];
  let places: StripPlace[] | undefined;
  let pending = Buffer.alloc(0);
  let index = 0;
  const take = (data: Uint8Array) => {
    const frame: GreyFrame = { width, height, data };
    if (places === undefined) {
      opening.push(frame);
      // A few frames apart, so the strips have moved on.
      if (opening.length === 6) {
        places = locateStrips([opening[0] as GreyFrame, frame], cell);
        if (places.length === 0) {
          throw new Error(
            "No frame barcode found in the first frames: is the switch on, the transport running?",
          );
        }
        opening.forEach((each, at) => {
          samples.push(sampleOf(each, places ?? [], times[at] ?? Number.NaN));
        });
      }
    } else {
      samples.push(sampleOf(frame, places, times[index] ?? Number.NaN));
    }
    index++;
  };
  for await (const chunk of decoder.stdout) {
    pending = Buffer.concat([pending, chunk as Buffer]);
    while (pending.length >= size) {
      take(new Uint8Array(pending.subarray(0, size)));
      pending = pending.subarray(size);
    }
  }
  return samples;
}

function sampleOf(
  frame: GreyFrame,
  places: readonly StripPlace[],
  time: number,
): Sample {
  return {
    displayTime: time,
    idle: false,
    strips: places.map((place) => decodeStrip(...sampleStrip(frame, place))),
  };
}

async function main(argv: readonly string[]): Promise<number> {
  if (argv[0] === "--locate") {
    const width = Number(argv[1]);
    const height = Number(argv[2]);
    const frames = argv.slice(3).map((path) => ({
      width,
      height,
      data: new Uint8Array(readFileSync(path)),
    }));
    const places = locateStrips(frames, barcodeCellPixels);
    if (places.length === 0) {
      console.error(
        "No frame barcode found: is SINELESS_FRAME_BARCODE=1 set for the host, the editor open and the transport running?",
      );
      return 1;
    }
    // The row's width goes with the places, so the recorder samples what limits.ts says.
    console.log(JSON.stringify({ rowCells: barcodeRowCells, places }));
    return 0;
  }
  const [file] = argv;
  if (file === undefined) {
    console.error(
      "usage: node scripts/frame-barcode/analyse.ts <recording.jsonl | movie>",
    );
    return 2;
  }
  const samples = file.endsWith(".jsonl")
    ? parseRecording(readFileSync(file, "utf8"))
    : await movieSamples(file, barcodeCellPixels);
  console.log(formatCounts(countRefreshes(samples)));
  return 0;
}

if (process.argv[1]?.endsWith("analyse.ts") === true) {
  process.exitCode = await main(process.argv.slice(2));
}
