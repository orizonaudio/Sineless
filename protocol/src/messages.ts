// Control messages exchanged as JSON over the native channel. Every message
// is an object whose `type` holds one of the MessageType values; the payloads are below.
// Setting values use the JSON form of their kind: numbers, enum value names, or arrays of names.
import type { SettingName } from "./settings.ts";

export const MessageType = {
  hello: "hello",
  snapshot: "snapshot",
  setSetting: "setSetting",
  stateChanged: "stateChanged",
  error: "error",
  viewport: "viewport",
  freeze: "freeze",
  pan: "pan",
  arm: "arm",
  view: "view",
  lanes: "lanes",
  compare: "compare",
  previewFix: "previewFix",
  resize: "resize",
  traceView: "traceView",
  nativeGraphicsLost: "nativeGraphicsLost",
  about: "about",
  contact: "contact",
} as const;
export type MessageType = (typeof MessageType)[keyof typeof MessageType];

export const ErrorCode = {
  protocolMismatch: "protocolMismatch",
  // The editor cannot reach the processor's engine.
  engineUnavailable: "engineUnavailable",
} as const;
export type ErrorCode = (typeof ErrorCode)[keyof typeof ErrorCode];

export type SettingValue = number | string | readonly string[];

// UI to engine. Starts (or restarts) a session; `viewWidth` is the trace area in CSS pixels.
export interface HelloMessage {
  readonly type: "hello";
  readonly protocolVersion: number;
  readonly viewWidth: number;
  readonly pixelRatio: number;
}

// Engine to UI. The full state, in reply to hello. `nativeTraces` is true when the editor draws the
// traces itself (macOS): the page then draws none and sends traceView. `nativeGraphicsLost` is
// present only then: true once native drawing has stopped for good this session. `frameBarcode` is
// true while the diagnostic switch is on (SINELESS_FRAME_BARCODE=1 for the host): every picture of
// the traces then carries the frame barcode, and the cursor readout gives it its corner.
export interface SnapshotMessage {
  readonly type: "snapshot";
  readonly protocolVersion: number;
  readonly settings: Readonly<Record<SettingName, SettingValue>>;
  readonly sampleRate: number;
  readonly inputChannels: number;
  readonly nativeTraces?: boolean;
  readonly nativeGraphicsLost?: boolean;
  readonly frameBarcode?: boolean;
}

// UI to engine. The engine always answers with stateChanged holding the value it kept.
export interface SetSettingMessage {
  readonly type: "setSetting";
  readonly name: SettingName;
  readonly value: SettingValue;
}

// Engine to UI. A setting's current value, after setSetting or a host state change.
export interface StateChangedMessage {
  readonly type: "stateChanged";
  readonly name: SettingName;
  readonly value: SettingValue;
}

// Engine to UI.
export interface ErrorMessage {
  readonly type: "error";
  readonly code: ErrorCode;
  readonly message: string;
}

// UI to engine. The trace area was resized.
export interface ViewportMessage {
  readonly type: "viewport";
  readonly viewWidth: number;
  readonly pixelRatio: number;
}

// UI to engine. Freeze or resume the display.
export interface FreezeMessage {
  readonly type: "freeze";
  readonly frozen: boolean;
}

// UI to engine. While frozen, how far back to move the view, in seconds.
export interface PanMessage {
  readonly type: "pan";
  readonly seconds: number;
}

// UI to engine. Single mode: wait for the next trigger again.
export interface ArmMessage {
  readonly type: "arm";
}

// UI to engine. The part of the span on screen, as fractions; a temporary view, not a
// setting. The whole span is { start: 0, end: 1 }.
export interface ViewMessage {
  readonly type: "view";
  readonly start: number;
  readonly end: number;
}

// One track of the multi-track view. `id` is stable while the track exists; `name` is empty and
// `colour` null when the host reports none; `own` marks the instance whose window shows the lanes.
// In the Compare view, `compared` marks the tracks summed and measured against this one (this
// track included whenever another is).
export interface LaneLabel {
  readonly id: number;
  readonly name: string;
  readonly colour: number | null; // 0xRRGGBB
  readonly own: boolean;
  readonly compared: boolean;
}

// Engine to UI. The tracks behind the lanes of frames whose laneSet matches, in lane order, and
// how many more tracks of the group are not drawn. Sent when the set changes.
export interface LanesMessage {
  readonly type: "lanes";
  readonly laneSet: number;
  readonly lanes: readonly LaneLabel[];
  readonly hidden: number;
}

// UI to engine. Compare view: the other tracks to compare with this one, by the ids of their
// lanes entries. Temporary, not a setting.
export interface CompareMessage {
  readonly type: "compare";
  readonly ids: readonly number[];
}

// UI to engine. Compare view: draw the sum as if every compared track were shifted and flipped
// as the analysis suggests. Temporary, not a setting.
export interface PreviewFixMessage {
  readonly type: "previewFix";
  readonly on: boolean;
}

// UI to editor. The user dragged the resize grip: the size the whole editor should have, in CSS
// pixels. The editor keeps it no smaller than its minimum and asks the host for it.
export interface ResizeMessage {
  readonly type: "resize";
  readonly width: number;
  readonly height: number;
}

// A colour as red, green, blue and alpha, each 0 to 1 (sRGB, not premultiplied).
export type Rgba = readonly number[];

// One track's colour for a lane set: `id` as in the lanes message (or the Sum lane's), whether it
// is compared.
export interface TraceLaneColour {
  readonly id: number;
  readonly colour: Rgba;
  readonly compared: boolean;
}

// UI to editor, while the editor draws the traces (snapshot.nativeTraces): what only the page knows
// about drawing them. Everything that changes with a frame (lanes, views, the comparison, the
// window the frame covers, the playhead) the editor reads from the frame itself. Sent after the
// snapshot and whenever a field changes.
export interface TraceViewMessage {
  readonly type: "traceView";
  // The part of the span on screen, as fractions; the frame's own window is stretched into it.
  readonly viewStart: number;
  readonly viewEnd: number;
  // A ViewLayout name.
  readonly layout: string;
  readonly verticalScaleDb: number;
  // The traces are stale (Waiting): drawn faint.
  readonly dim: boolean;
  // Trace line width in device pixels.
  readonly lineWidth: number;
  // The lanes the page lays out for a frame that carries no views and no lanes (Beats before the
  // host reports a tempo, or before the first frame): one per view shown when stacked, else one.
  // A frame with views or lanes is laid out from them instead.
  readonly emptyFrameLanes: number;
  // One per ChannelView, in ChannelView order.
  readonly viewColours: readonly Rgba[];
  // Track colours for each lane set the page holds; a frame whose set is missing takes
  // `fallbackColours` by position.
  readonly laneSets: readonly {
    readonly laneSet: number;
    readonly lanes: readonly TraceLaneColour[];
  }[];
  readonly fallbackColours: readonly Rgba[];
  readonly sumColour: Rgba;
  readonly screen: Rgba;
  readonly grid: Rgba;
  readonly axis: Rgba;
  readonly cursor: Rgba;
  readonly cancellation: Rgba;
  // The grid the page draws now (ui/src/view/grid-settings.ts): GridType, GridDensity (Time and
  // Level) and BeatDivision names.
  readonly gridChoice: {
    readonly type: string;
    readonly time: string;
    readonly level: string;
    readonly division: string;
  };
}

// Editor to UI: native drawing stopped for good this session (a removed GPU, persistent errors,
// refused graphics memory). The page shows what to do.
export interface NativeGraphicsLostMessage {
  readonly type: "nativeGraphicsLost";
}

// Editor to UI, in reply to every hello, before anything else and whether or not the editor can
// reach the engine: the plugin's version as the host lists it, and whether this build has a contact
// page to open (a build without the brand has none).
export interface AboutMessage {
  readonly type: "about";
  readonly version: string;
  readonly contact: boolean;
}

// UI to editor: open the vendor's site in the default browser. The page names no address;
// the editor opens only the one it was built with, and nothing in a build without the brand.
export interface ContactMessage {
  readonly type: "contact";
}

export type UiToEngineMessage =
  | HelloMessage
  | SetSettingMessage
  | ViewportMessage
  | FreezeMessage
  | PanMessage
  | ArmMessage
  | ViewMessage
  | CompareMessage
  | PreviewFixMessage
  | ResizeMessage
  | TraceViewMessage
  | ContactMessage;

export type EngineToUiMessage =
  | SnapshotMessage
  | StateChangedMessage
  | ErrorMessage
  | LanesMessage
  | NativeGraphicsLostMessage
  | AboutMessage;
