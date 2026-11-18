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
  tracks: "tracks",
  showTracks: "showTracks",
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
// With Compare on, every lane is summed and measured against this track's. `instance` is the
// lane's number among the instances in the process on its host track (the same track position, or
// with no positions the same name), from 1 in the order they were created and counting those not
// shown; 0 for a track's only instance.
export interface LaneLabel {
  readonly id: number;
  readonly name: string;
  readonly colour: number | null; // 0xRRGGBB
  readonly own: boolean;
  readonly instance: number;
}

// Engine to UI. The tracks behind the lanes of frames whose laneSet matches, in lane order. Sent
// when the set changes.
export interface LanesMessage {
  readonly type: "lanes";
  readonly laneSet: number;
  readonly lanes: readonly LaneLabel[];
}

// One Sineless instance in the process, as the multi-track view can show it: a lane's label
// fields, and whether this instance's Multi shows it (`shown`; always for its `own`).
export interface TrackEntry extends LaneLabel {
  readonly shown: boolean;
}

// Engine to UI. Every Sineless instance in the process, in the host's track order, then creation
// (the order of the lanes). Sent in Multi when the list changes: an instance added or gone, a
// name, a colour, the order, or the tracks shown.
export interface TracksMessage {
  readonly type: "tracks";
  readonly tracks: readonly TrackEntry[];
}

// UI to engine. The other tracks this instance's Multi shows, by the ids of their tracks entries,
// at most maxShownTracks - 1 (this instance's own is always shown). Kept while the plugin is
// loaded, not saved.
export interface ShowTracksMessage {
  readonly type: "showTracks";
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

// One track's colour for a lane set: `id` as in the lanes message (or the Sum lane's), and whether
// it is this track's (a difference's lane is filled from it).
export interface TraceLaneColour {
  readonly id: number;
  readonly colour: Rgba;
  readonly own: boolean;
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
  // A TraceShape name: each trace's wave as a line, or the area between it and zero.
  readonly traceShape: string;
  readonly verticalScaleDb: number;
  // The traces are stale (Waiting): drawn faint.
  readonly dim: boolean;
  // The overlaid trace highlighted (FR-16.4): a ChannelView name in Single, a lane's track id (an
  // id of `laneSets`) in Multi, or null for none. The editor finds it in the frame it draws and
  // highlights it only where the page would (trace-geometry.ts, traceLayering).
  readonly highlight:
    { readonly view: string } | { readonly lane: number } | null;
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
  // A difference's lane: what only the other tracks cover, and what only this track does.
  readonly gained: Rgba;
  readonly lost: Rgba;
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
  | ShowTracksMessage
  | PreviewFixMessage
  | ResizeMessage
  | TraceViewMessage
  | ContactMessage;

export type EngineToUiMessage =
  | SnapshotMessage
  | StateChangedMessage
  | ErrorMessage
  | LanesMessage
  | TracksMessage
  | NativeGraphicsLostMessage
  | AboutMessage;
