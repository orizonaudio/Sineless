// Running totals of the page's work per frame, for performance measurement. The editor's
// benchmark reads them through window.orizonDebug.perf; they cost two clock reads per frame.
export const perf = {
  framesReceived: 0,
  bytesReceived: 0,
  // Turning the native message into a frame and handling it, in ms.
  receiveMs: 0,
  framesDrawn: 0,
  drawMs: 0,
  maxDrawMs: 0,
};

export type Perf = typeof perf;
