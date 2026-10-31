// Hand-written WebGL2 renderer for the traces. Grid, labels and controls are DOM.
//
// The trace is drawn on the GPU from the frame's values as they arrived, uploaded as a buffer:
// one instance of a quad (two triangles, six vertices) per min/max column or raw point, covering
// that point's piece of the trace (its vertical line from min to max and the band to the next
// point). Each pixel measures its distance to the piece and writes how much of it the line
// covers into the trace's channel of its group's offscreen target, keeping the larger where
// pieces overlap, which is the coverage of the whole trace; one pass then layers the traces over
// the screen. Edges are smooth at any slope, and a translucent trace blends once, as a group with an
// opacity is composited. The CPU builds no vertices, so the cost of a frame hardly depends on its
// size.
import {
  FrameMode,
  type ChannelView,
  type ViewLayout,
} from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { fullView, type ViewWindow } from "../view/zoom.ts";
import type { FrameRenderer } from "./render-loop.ts";
import {
  cancellationBands,
  compositeLayering,
  fullScale,
  lanes,
  coverageAttachments,
  coverageSlot,
  coverageTargets,
  playheadEdges,
  tracesPerPass,
} from "./trace-geometry.ts";

export type Rgba = readonly [number, number, number, number];

export interface TraceStyle {
  // Resolved at draw time, so colours can come from a stylesheet that loads after start-up.
  readonly colors: () => ReadonlyMap<ChannelView, Rgba>;
  // Multi-track frames: one colour per lane, in lane order, and in a Compare frame which tracks
  // are compared.
  readonly laneColors: () => readonly Rgba[];
  readonly comparedLanes: () => readonly boolean[];
  // A tempo-synced sweep's playhead, and the dimming over the previous cycle to its right.
  readonly sweepColors: () => {
    readonly cursor: Rgba;
    readonly previous: Rgba;
  };
  // Behind a Compare frame's sum lane, where the compared tracks cancel.
  readonly cancellationColor: () => Rgba;
  readonly lineWidthPixels: number;
}

export interface TraceView {
  readonly verticalScaleDb: number;
  readonly layout: ViewLayout;
  // Shows the trace as stale, e.g. while Normal mode waits for a trigger.
  readonly dim?: boolean;
  // The part of the frame on screen.
  readonly view?: ViewWindow;
}

// Shared by the trace shaders, in device pixels from the canvas's bottom left. A block (a view
// or a lane) holds `count` points of `stride` values (min and max, or one sample). Point i sits at frame index i + offset (columns at their centres, raw points
// shifted by the frame's start fraction) of `positions`; a frame covers frameStart to
// frameStart + frameWidth of the span, the screen viewStart to viewStart + viewWidth of it.
const traceCommon = `#version 300 es
precision highp float;
precision highp int;
uniform int count;
uniform int stride;
uniform float offset;
uniform float positions;
uniform float frameStart;
uniform float frameWidth;
uniform float viewStart;
uniform float viewWidth;
uniform float center;
uniform float halfHeight;
uniform float scale;
uniform vec2 canvas;
uniform float lineHalf;
float pixelX(float index) {
  return (frameStart + index / positions * frameWidth - viewStart) / viewWidth * canvas.x;
}
// A value's height, kept within 1e15 px of its lane by limiting the value before it is divided
// by the scale: a huge sample over a small scale would otherwise overflow, and GLSL promises
// nothing for Inf arithmetic. A line to 1e15 px is vertical to within 1e-9 px over the canvas.
float pixelY(float value) {
  float halfPixels = halfHeight * 0.5 * canvas.y;
  float limit = scale * 1e15 / halfPixels;
  return (center + 1.0) * 0.5 * canvas.y + clamp(value, -limit, limit) / scale * halfPixels;
}
// An end beyond the band moved back along the segment to the band's edge: the edge is its height
// exactly and only x is worked out, so the slope is kept.
vec2 clipped(vec2 a, vec2 b, float bottom, float top) {
  if (a.y > top) {
    return vec2(a.x + (top - a.y) / (b.y - a.y) * (b.x - a.x), top);
  }
  if (a.y < bottom) {
    return vec2(a.x + (bottom - a.y) / (b.y - a.y) * (b.x - a.x), bottom);
  }
  return a;
}
`;

// One instance per point: its piece of the trace, from its own point to the next (a missing
// point draws nothing; a missing next point leaves its own line alone), widened for the round
// ends. The quad is a box around the piece, unless one following its lines is smaller: the band
// between them widened across each line by pad times the secant of its slope, which still holds
// the round ends. Both are decided alike at every corner, from the same values.
const traceVertexShader = `${traceCommon}
// The values of points i and i + 1, as their bits.
in uvec2 hereValues;
in uvec2 nextValues;
// A point's vertical range, unless it is missing: outside the block, or a value not finite (read
// from its bits, so no NaN comparison is involved).
bool point(bool inside, uvec2 bits, out float lo, out float hi) {
  lo = 0.0;
  hi = 0.0;
  if (!inside) {
    return false;
  }
  uint low = bits.x;
  uint high = stride == 2 ? bits.y : low;
  if ((low & 0x7f800000u) == 0x7f800000u || (high & 0x7f800000u) == 0x7f800000u) {
    return false;
  }
  lo = pixelY(uintBitsToFloat(low));
  hi = pixelY(uintBitsToFloat(high));
  return true;
}
// Points i and i + 1 (lo, hi; lo > hi when i + 1 is missing).
flat out int piece;
flat out vec2 here;
flat out vec2 next;
void main() {
  int corner = gl_VertexID % 6;
  // Corners of two triangles (0, 1, 2) and (2, 1, 3): at the far end, on the upper side.
  bool far = corner == 2 || corner == 3 || corner == 5;
  bool upper = corner == 1 || corner == 4 || corner == 5;
  int i = gl_InstanceID;
  float pad = lineHalf + 1.0;
  float lo;
  float hi;
  bool present = point(true, hereValues, lo, hi);
  float lo2;
  float hi2;
  bool joined = point(i + 1 < count, nextValues, lo2, hi2);
  piece = i;
  here = vec2(lo, hi);
  next = joined ? vec2(lo2, hi2) : vec2(1.0, -1.0);
  float xa = pixelX(float(i) + offset);
  float xb = joined ? pixelX(float(i + 1) + offset) : xa;
  float x = far ? xb + pad : xa - pad;
  float y = 0.0;
  if (present) {
    float low = joined ? min(lo, lo2) : lo;
    float high = joined ? max(hi, hi2) : hi;
    y = clamp(upper ? high + pad : low - pad, 0.0, canvas.y);
    float run = xb - xa;
    // Only near the canvas, where the slopes stay small enough to follow.
    if (joined && run > 0.0 && low > -canvas.y && high < 2.0 * canvas.y) {
      float slopeHi = (hi2 - hi) / run;
      float slopeLo = (lo2 - lo) / run;
      float acrossHi = pad * sqrt(1.0 + slopeHi * slopeHi);
      float acrossLo = pad * sqrt(1.0 + slopeLo * slopeLo);
      // The band's mean height against the box's.
      float band = 0.5 * ((hi - lo) + (hi2 - lo2)) + acrossHi + acrossLo + abs(slopeHi - slopeLo) * pad;
      if (band < high - low + 2.0 * pad) {
        float slope = upper ? slopeHi : slopeLo;
        float dx = far ? run + pad : -pad;
        // Not clamped to the canvas, which would bend the edge; the GPU clips it there.
        y = upper ? hi + slope * dx + acrossHi : lo + slope * dx - acrossLo;
      }
    }
  } else {
    x = 0.0;
  }
  gl_Position = vec4(x / canvas.x * 2.0 - 1.0, y / canvas.y * 2.0 - 1.0, 0.0, 1.0);
}`;

// The distance from the pixel's centre to its piece: the point's vertical line from min to max,
// and to a next point present, the lines joining their mins and their maxes and the band between
// them. Coverage falls from 1 to 0 over the pixel either side of the line's edge; the colour mask
// lets it into the trace's own channel only, and the blending keeps the larger where pieces
// overlap.
const traceFragmentShader = `${traceCommon}
flat in int piece;
flat in vec2 here;
flat in vec2 next;
uniform float spacing;
out vec4 coverage;
float segment(vec2 p, vec2 a, vec2 b) {
  vec2 ab = b - a;
  float dd = dot(ab, ab);
  float t = dd > 0.0 ? clamp(dot(p - a, ab) / dd, 0.0, 1.0) : 0.0;
  return length(p - a - t * ab);
}
// A line between two points, within a band four canvas heights beyond the canvas (where float
// error stays far below a pixel); a line wholly beyond one side is out of reach.
float stroke(vec2 p, vec2 a, vec2 b) {
  float bottom = -4.0 * canvas.y;
  float top = 5.0 * canvas.y;
  if ((a.y > top && b.y > top) || (a.y < bottom && b.y < bottom)) {
    return 1e30;
  }
  return segment(p, clipped(a, b, bottom, top), clipped(b, a, bottom, top));
}
void main() {
  vec2 p = gl_FragCoord.xy;
  float xa = pixelX(float(piece) + offset);
  float d = length(vec2(p.x - xa, p.y - clamp(p.y, here.x, here.y)));
  if (next.x <= next.y) {
    float xb = pixelX(float(piece + 1) + offset);
    // The band between the two, from their own heights rather than the clipped lines.
    float f = (p.x - xa) / spacing;
    if (f >= 0.0 && f <= 1.0 && p.y >= mix(here.x, next.x, f) && p.y <= mix(here.y, next.y, f)) {
      d = 0.0;
    }
    d = min(d, stroke(p, vec2(xa, here.y), vec2(xb, next.y)));
    d = min(d, stroke(p, vec2(xa, here.x), vec2(xb, next.x)));
  }
  float covered = clamp(lineHalf + 0.5 - d, 0.0, 1.0);
  if (covered <= 0.0) {
    discard;
  }
  coverage = vec4(covered);
}`;

// One triangle over the whole screen.
const compositeVertexShader = `#version 300 es
void main() {
  vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  gl_Position = vec4(corner, 0.0, 1.0);
}`;

// The traces layered at each pixel in drawing order, later over earlier, then blended over
// the screen once: the same as blending each in turn, since layering is associative.
const compositeFragmentShader = `#version 300 es
precision highp float;
uniform sampler2D coverage0;
uniform sampler2D coverage1;
uniform sampler2D coverage2;
uniform sampler2D coverage3;
uniform vec4 colors[${String(tracesPerPass)}];
uniform int traces;
uniform int attachments;
out vec4 fragmentColor;
void main() {
  ivec2 at = ivec2(gl_FragCoord.xy);
  vec4 coverage[${String(coverageAttachments)}];
  coverage[0] = texelFetch(coverage0, at, 0);
  coverage[1] = attachments > 1 ? texelFetch(coverage1, at, 0) : vec4(0.0);
  coverage[2] = attachments > 2 ? texelFetch(coverage2, at, 0) : vec4(0.0);
  coverage[3] = attachments > 3 ? texelFetch(coverage3, at, 0) : vec4(0.0);
  vec3 rgb = vec3(0.0);
  float alpha = 0.0;
${compositeLayering()}
  if (alpha <= 0.0) {
    discard;
  }
  // Not premultiplied: the blend multiplies by alpha.
  fragmentColor = vec4(rgb / alpha, alpha);
}`;

// Ready-made clip-space triangles, for the cancellation shading and the playhead.
const plainShader = `#version 300 es
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }`;

const plainFragmentShader = `#version 300 es
precision mediump float;
uniform vec4 color;
out vec4 fragmentColor;
void main() { fragmentColor = color; }`;

function compile(
  gl: WebGL2RenderingContext,
  type: number,
  source: string,
): WebGLShader {
  const shader = gl.createShader(type);
  if (shader === null) {
    throw new Error("Could not create a WebGL shader");
  }
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (gl.getShaderParameter(shader, gl.COMPILE_STATUS) !== true) {
    throw new Error(
      `Shader compilation failed: ${gl.getShaderInfoLog(shader) ?? ""}`,
    );
  }
  return shader;
}

// A linked program with its uniform and attribute locations, looked up once.
class Program {
  readonly program: WebGLProgram;
  private readonly uniforms = new Map<string, WebGLUniformLocation | null>();
  private readonly gl: WebGL2RenderingContext;

  constructor(
    gl: WebGL2RenderingContext,
    vertexSource: string,
    fragmentSource: string,
  ) {
    this.gl = gl;
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, vertexSource));
    gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, fragmentSource));
    gl.linkProgram(program);
    if (gl.getProgramParameter(program, gl.LINK_STATUS) !== true) {
      throw new Error(
        `Shader linking failed: ${gl.getProgramInfoLog(program) ?? ""}`,
      );
    }
    this.program = program;
  }

  uniform(name: string): WebGLUniformLocation | null {
    if (!this.uniforms.has(name)) {
      this.uniforms.set(name, this.gl.getUniformLocation(this.program, name));
    }
    return this.uniforms.get(name) ?? null;
  }

  attribute(name: string): number {
    return this.gl.getAttribLocation(this.program, name);
  }
}

// The graphics memory for the traces' coverage could not be allocated: the page says so rather
// than drawing nothing.
export class TraceMemoryError extends Error {}

export class TraceRenderer implements FrameRenderer {
  private readonly canvas: HTMLCanvasElement;
  private readonly gl: WebGL2RenderingContext;
  private readonly trace: Program;
  private readonly composite: Program;
  private readonly plain: Program;
  // The traces' coverage, a channel each, drawn offscreen and layered over the screen; all
  // allocated together at the drawing buffer's size, each the only target of its framebuffer.
  private readonly coverage: WebGLTexture[];
  private readonly coverageFramebuffers: WebGLFramebuffer[];
  private coverageWidth = 0;
  private coverageHeight = 0;
  // The frame's values as received, and how many bytes the buffer holds.
  private readonly valuesBuffer: WebGLBuffer;
  private valuesAllocated = 0;
  // The cancellation shading's and the playhead's triangles.
  private readonly shadingBuffer: WebGLBuffer;
  private readonly style: TraceStyle;
  private readonly view: () => TraceView;
  // Average CPU time of draw(), in milliseconds, for profiling.
  drawMilliseconds = 0;

  constructor(
    canvas: HTMLCanvasElement,
    style: TraceStyle,
    view: () => TraceView,
  ) {
    // No multisampling: the trace shader works out each pixel's coverage itself, into targets of
    // its own.
    const gl = canvas.getContext("webgl2", { antialias: false, alpha: true });
    if (gl === null) {
      throw new Error("WebGL2 is not available");
    }
    this.canvas = canvas;
    this.gl = gl;
    this.style = style;
    this.view = view;
    this.trace = new Program(gl, traceVertexShader, traceFragmentShader);
    this.composite = new Program(
      gl,
      compositeVertexShader,
      compositeFragmentShader,
    );
    this.plain = new Program(gl, plainShader, plainFragmentShader);
    this.coverage = Array.from({ length: coverageAttachments }, () =>
      gl.createTexture(),
    );
    this.coverageFramebuffers = this.coverage.map(() => gl.createFramebuffer());
    this.shadingBuffer = gl.createBuffer();
    this.valuesBuffer = gl.createBuffer();
    gl.enable(gl.BLEND);
    // The canvas is composited as premultiplied: alpha accumulates as coverage.
    gl.blendFuncSeparate(
      gl.SRC_ALPHA,
      gl.ONE_MINUS_SRC_ALPHA,
      gl.ONE,
      gl.ONE_MINUS_SRC_ALPHA,
    );
    this.fitCoverage();
  }

  // The coverage targets at the drawing buffer's size, which is at least 1×1 even for a hidden
  // editor's canvas of no size (WebKit and Chromium), so an attachment is never empty.
  private fitCoverage(): void {
    const gl = this.gl;
    const width = gl.drawingBufferWidth;
    const height = gl.drawingBufferHeight;
    if (width === this.coverageWidth && height === this.coverageHeight) {
      return;
    }
    this.coverageWidth = width;
    this.coverageHeight = height;
    const statuses = this.coverage.map((texture, target) => {
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA8,
        width,
        height,
        0,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        null,
      );
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.bindFramebuffer(
        gl.FRAMEBUFFER,
        this.coverageFramebuffers[target] ?? null,
      );
      gl.framebufferTexture2D(
        gl.FRAMEBUFFER,
        gl.COLOR_ATTACHMENT0,
        gl.TEXTURE_2D,
        texture,
        0,
      );
      return gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    });
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    const status = statuses.find((each) => each !== gl.FRAMEBUFFER_COMPLETE);
    if (status !== undefined) {
      throw new TraceMemoryError(
        `The traces' coverage targets are incomplete (${String(status)})`,
      );
    }
  }

  // Ready-made clip-space triangles in one colour.
  private drawTriangles(vertices: Float32Array, color: Rgba): void {
    const gl = this.gl;
    const program = this.plain;
    gl.useProgram(program.program);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.shadingBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STREAM_DRAW);
    const position = program.attribute("position");
    gl.enableVertexAttribArray(position);
    gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0);
    gl.uniform4f(
      program.uniform("color"),
      color[0],
      color[1],
      color[2],
      color[3],
    );
    gl.drawArrays(gl.TRIANGLES, 0, vertices.length / 2);
    gl.disableVertexAttribArray(position);
  }

  // The playhead and the dimmed previous cycle, over the traces; drawn here rather than as page
  // elements, so that a sweep changes nothing on the page but this canvas.
  private drawSweep(position: number): void {
    const { cursor, previous } = this.style.sweepColors();
    const { left, right } = playheadEdges(
      position,
      this.canvas.width,
      window.devicePixelRatio,
    );
    const rectangle = (left: number, right: number) =>
      new Float32Array([
        left,
        -1,
        left,
        1,
        right,
        -1,
        right,
        -1,
        left,
        1,
        right,
        1,
      ]);
    // The dimming starts under the line, so its edge is on the same pixel boundary.
    if (position < 1) {
      this.drawTriangles(rectangle(left, 1), previous);
    }
    if (position >= 0 && position <= 1) {
      this.drawTriangles(rectangle(left, right), cursor);
    }
  }

  // The values of all blocks, which lie one after another in the frame's bytes, with room for
  // one point more, which the last point's next-point attribute reads (and its block's count
  // marks missing).
  private upload(blocks: readonly Float32Array[]): number {
    const gl = this.gl;
    const start = Math.min(...blocks.map((values) => values.byteOffset));
    const end = Math.max(
      ...blocks.map((values) => values.byteOffset + values.byteLength),
    );
    const first = blocks[0];
    if (first === undefined) {
      return start;
    }
    const values = new Uint8Array(first.buffer, start, end - start);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.valuesBuffer);
    const size = values.byteLength + 8;
    if (size > this.valuesAllocated) {
      gl.bufferData(gl.ARRAY_BUFFER, size, gl.STREAM_DRAW);
      this.valuesAllocated = size;
    }
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, values);
    return start;
  }

  draw(frame: Frame): void {
    const started = performance.now();
    const gl = this.gl;
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const view = this.view();
    const white: Rgba = [1, 1, 1, 1];
    const alpha = (color: Rgba) =>
      view.dim === true ? color[3] * 0.45 : color[3];

    // The traces: views in ChannelView order, or lanes, each in its own lane of the screen.
    let blocks: [Float32Array, Rgba][];
    if (frame.lanes.length > 0) {
      const colors = this.style.laneColors();
      blocks = frame.lanes.map((values, index) => [
        values,
        colors[index] ?? white,
      ]);
    } else {
      const colors = this.style.colors();
      blocks = [...frame.views].map(([channelView, values]) => [
        values,
        colors.get(channelView) ?? white,
      ]);
    }

    const layout = {
      widthPixels: this.canvas.width,
      heightPixels: this.canvas.height,
      verticalScaleDb: view.verticalScaleDb,
      layout: view.layout,
      lineWidthPixels: this.style.lineWidthPixels,
      ...(view.view === undefined ? {} : { view: view.view }),
    };
    const shading = cancellationBands(
      frame,
      layout,
      this.style.comparedLanes(),
    );
    if (shading.length > 0) {
      const color = this.style.cancellationColor();
      const [r, g, b] = color;
      this.drawTriangles(shading, [r, g, b, alpha(color)]);
    }

    // A tempo-synced sweep's playhead, as a fraction of the screen from the left.
    const screen = view.view ?? fullView;
    const sweep =
      frame.synced && frame.cursor !== undefined
        ? (frame.cursor - screen.start) / (screen.end - screen.start)
        : undefined;
    if (blocks.length === 0 || frame.pointCount === 0) {
      if (sweep !== undefined) {
        this.drawSweep(sweep);
      }
      this.finish(started);
      return;
    }
    const start = this.upload(blocks.map(([values]) => values));
    const minMax = frame.mode === FrameMode.minMax;
    const program = this.trace;
    this.fitCoverage();
    gl.useProgram(program.program);
    const here = program.attribute("hereValues");
    const next = program.attribute("nextValues");
    const stride = minMax ? 2 : 1;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.valuesBuffer);
    for (const attribute of [here, next]) {
      gl.enableVertexAttribArray(attribute);
      gl.vertexAttribDivisor(attribute, 1);
    }
    const positions = minMax
      ? frame.pointCount
      : frame.visibleSamples > 0
        ? frame.visibleSamples
        : frame.pointCount;
    const frameWidth = frame.viewEnd - frame.viewStart;
    const viewWidth = screen.end - screen.start;
    const spacing = (this.canvas.width * frameWidth) / (viewWidth * positions);
    const lineHalf = this.style.lineWidthPixels / 2;
    gl.uniform1i(program.uniform("count"), frame.pointCount);
    gl.uniform1i(program.uniform("stride"), stride);
    gl.uniform1f(
      program.uniform("offset"),
      minMax ? 0.5 : -frame.startFraction,
    );
    gl.uniform1f(program.uniform("positions"), positions);
    gl.uniform1f(program.uniform("spacing"), spacing);
    gl.uniform1f(program.uniform("scale"), fullScale(view.verticalScaleDb));
    gl.uniform1f(program.uniform("frameStart"), frame.viewStart);
    gl.uniform1f(program.uniform("frameWidth"), frameWidth);
    gl.uniform1f(program.uniform("viewStart"), screen.start);
    gl.uniform1f(program.uniform("viewWidth"), viewWidth);
    gl.uniform2f(
      program.uniform("canvas"),
      this.canvas.width,
      this.canvas.height,
    );
    gl.uniform1f(program.uniform("lineHalf"), lineHalf);

    const blockLanes = lanes(blocks.length, view.layout);
    // A frame holds at most maxLanes traces, which one pass holds.
    const traces = blocks.slice(0, tracesPerPass);
    const targets = coverageTargets(traces.length);
    // Each trace's coverage into its channel, the larger kept where its pieces overlap. Writing
    // only that channel of that target, rather than zeros into all four targets, takes about a
    // third of the GPU time (16 overlaid lanes on an M1); a framebuffer per target, rather than
    // one switching its draw buffers, keeps ANGLE from converting between draws.
    gl.blendEquation(gl.MAX);
    const colors = new Float32Array(tracesPerPass * 4);
    let bound = -1;
    traces.forEach(([values, color], index) => {
      const lane = blockLanes[index] ?? { center: 0, halfHeight: 1 };
      const { attachment, channel } = coverageSlot(index);
      if (attachment !== bound) {
        // A clear honours the colour mask: every channel, then this trace's own.
        gl.bindFramebuffer(
          gl.FRAMEBUFFER,
          this.coverageFramebuffers[attachment] ?? null,
        );
        gl.colorMask(true, true, true, true);
        gl.clear(gl.COLOR_BUFFER_BIT);
        bound = attachment;
      }
      gl.colorMask(channel === 0, channel === 1, channel === 2, channel === 3);
      gl.uniform1f(program.uniform("center"), lane.center);
      gl.uniform1f(program.uniform("halfHeight"), lane.halfHeight);
      // One point per instance: its own values, and the next point's.
      const offset = values.byteOffset - start;
      gl.vertexAttribIPointer(
        here,
        stride,
        gl.UNSIGNED_INT,
        stride * 4,
        offset,
      );
      gl.vertexAttribIPointer(
        next,
        stride,
        gl.UNSIGNED_INT,
        stride * 4,
        offset + stride * 4,
      );
      gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, frame.pointCount);
      colors.set([color[0], color[1], color[2], alpha(color)], index * 4);
    });
    for (const attribute of [here, next]) {
      gl.vertexAttribDivisor(attribute, 0);
      gl.disableVertexAttribArray(attribute);
    }
    gl.colorMask(true, true, true, true);
    // Then layered over the screen, once.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.blendEquation(gl.FUNC_ADD);
    const composite = this.composite;
    gl.useProgram(composite.program);
    this.coverage.forEach((texture, attachment) => {
      gl.activeTexture(gl.TEXTURE1 + attachment);
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.uniform1i(
        composite.uniform(`coverage${String(attachment)}`),
        1 + attachment,
      );
    });
    gl.activeTexture(gl.TEXTURE0);
    gl.uniform4fv(composite.uniform("colors"), colors);
    gl.uniform1i(composite.uniform("traces"), traces.length);
    gl.uniform1i(composite.uniform("attachments"), targets);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    if (sweep !== undefined) {
      this.drawSweep(sweep);
    }
    this.finish(started);
  }

  private finish(started: number): void {
    const elapsed = performance.now() - started;
    this.drawMilliseconds = this.drawMilliseconds * 0.95 + elapsed * 0.05;
  }
}
