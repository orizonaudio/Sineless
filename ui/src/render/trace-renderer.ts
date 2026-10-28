// Hand-written WebGL2 renderer for the traces. Grid, labels and controls are DOM.
//
// The trace is drawn on the GPU from the frame's values as they arrived, uploaded as a texture:
// one instance of a quad (two triangles, six vertices) per min/max column or raw point, covering
// that point's cell of the screen. Each pixel measures its distance to the trace (the band
// between the columns' min and max lines, with round ends) and is shaded by how much of it the
// line covers, so edges are smooth at any slope, and each pixel is shaded once, so a translucent
// trace blends once. The CPU builds no vertices, so the cost of a frame hardly depends on its
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
  fullScale,
  lanes,
  playheadEdges,
  reachCount,
  textureRowLength,
  textureRows,
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

// Shared by both trace shaders, in device pixels from the canvas's bottom left. A block (a view
// or a lane) holds `count` points of `stride` values (min and max, or one sample) from texel
// `blockStart`. Point i sits at frame index i + offset (columns at their centres, raw points
// shifted by the frame's start fraction) of `positions`; a frame covers frameStart to
// frameStart + frameWidth of the span, the screen viewStart to viewStart + viewWidth of it.
const traceCommon = `#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform usampler2D frameValues;
uniform int count;
uniform int stride;
uniform int blockStart;
uniform int reach;
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
uint texel(int k) {
  return texelFetch(frameValues, ivec2(k % ${String(textureRowLength)}, k / ${String(textureRowLength)}), 0).r;
}
// Point j's vertical range, unless it is missing: outside the block, or a value not finite (read
// from its bits, so no NaN comparison is involved).
bool point(int j, out float lo, out float hi) {
  lo = 0.0;
  hi = 0.0;
  if (j < 0 || j >= count) {
    return false;
  }
  int k = blockStart + j * stride;
  uint low = texel(k);
  uint high = stride == 2 ? texel(k + 1) : low;
  if ((low & 0x7f800000u) == 0x7f800000u || (high & 0x7f800000u) == 0x7f800000u) {
    return false;
  }
  lo = pixelY(uintBitsToFloat(low));
  hi = pixelY(uintBitsToFloat(high));
  return true;
}
`;

// One instance per point: its cell, between the frame indices half a point either side, the
// first and last widened for the round ends; as tall as the points it can be reached from. Each
// edge between two cells comes from the same expression of the same edge index in both, and the
// position is invariant, so the cells meet exactly and each pixel belongs to one of them.
const traceVertexShader = `${traceCommon}
invariant gl_Position;
flat out int cell;
// Points cell − 1, cell and cell + 1 (lo, hi; lo > hi when missing), read here once rather than
// by every pixel: inside the trace, a pixel needs no others.
flat out vec2 before;
flat out vec2 here;
flat out vec2 after;
vec2 range(int j) {
  float lo;
  float hi;
  return point(j, lo, hi) ? vec2(lo, hi) : vec2(1.0, -1.0);
}
void main() {
  int corner = gl_VertexID % 6;
  // Corners of two triangles (0, 1, 2) and (2, 1, 3): at the far edge, on the upper side.
  bool far = corner == 2 || corner == 3 || corner == 5;
  bool upper = corner == 1 || corner == 4 || corner == 5;
  int edge = gl_InstanceID + (far ? 1 : 0);
  float pad = lineHalf + 1.0;
  float x = pixelX(float(edge) + offset - 0.5);
  if (edge == 0) {
    x -= pad;
  } else if (edge == count) {
    x += pad;
  }
  float low = 0.0;
  float high = -1.0;
  bool any = false;
  for (int j = gl_InstanceID - reach; j <= gl_InstanceID + reach; j++) {
    float lo;
    float hi;
    if (point(j, lo, hi)) {
      low = any ? min(low, lo) : lo;
      high = any ? max(high, hi) : hi;
      any = true;
    }
  }
  // Nothing in reach: no area.
  float y = 0.0;
  if (any) {
    y = clamp(upper ? high + pad : low - pad, 0.0, canvas.y);
  }
  gl_Position = vec4(x / canvas.x * 2.0 - 1.0, y / canvas.y * 2.0 - 1.0, 0.0, 1.0);
  cell = gl_InstanceID;
  before = range(gl_InstanceID - 1);
  here = range(gl_InstanceID);
  after = range(gl_InstanceID + 1);
}`;

// The distance from the pixel's centre to the trace near its cell: each point's vertical line
// from min to max, and between two points both present, the lines joining their mins and their
// maxes and the band between them. Coverage falls from 1 to 0 over the pixel either side of the
// line's edge.
const traceFragmentShader = `${traceCommon}
flat in int cell;
flat in vec2 before;
flat in vec2 here;
flat in vec2 after;
uniform float spacing;
uniform vec4 color;
out vec4 fragmentColor;
float segment(vec2 p, vec2 a, vec2 b) {
  vec2 ab = b - a;
  float dd = dot(ab, ab);
  float t = dd > 0.0 ? clamp(dot(p - a, ab) / dd, 0.0, 1.0) : 0.0;
  return length(p - a - t * ab);
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
// Point j, from the vertex shader for the cell and its neighbours.
bool near(int j, out float lo, out float hi) {
  int k = j - cell;
  if (k >= -1 && k <= 1) {
    vec2 r = k == -1 ? before : (k == 0 ? here : after);
    lo = r.x;
    hi = r.y;
    return r.x <= r.y;
  }
  return point(j, lo, hi);
}
void main() {
  vec2 p = gl_FragCoord.xy;
  float solid = lineHalf - 0.5;
  float d = 1e30;
  // Outward from the pixel's own cell, so a pixel inside the trace stops at its first points.
  for (int k = 0; k <= 2 * reach && d > solid; k++) {
    int j = cell + ((k & 1) == 1 ? -(k + 1) / 2 : k / 2);
    float loA;
    float hiA;
    if (!near(j, loA, hiA)) {
      continue;
    }
    float xa = pixelX(float(j) + offset);
    d = min(d, length(vec2(p.x - xa, p.y - clamp(p.y, loA, hiA))));
    float loB;
    float hiB;
    if (near(j + 1, loB, hiB)) {
      float xb = pixelX(float(j + 1) + offset);
      // The band between the two, from their own heights rather than the clipped lines.
      float f = (p.x - xa) / spacing;
      if (f >= 0.0 && f <= 1.0 && p.y >= mix(loA, loB, f) && p.y <= mix(hiA, hiB, f)) {
        d = 0.0;
      }
      d = min(d, stroke(p, vec2(xa, hiA), vec2(xb, hiB)));
      d = min(d, stroke(p, vec2(xa, loA), vec2(xb, loB)));
    }
  }
  float coverage = clamp(lineHalf + 0.5 - d, 0.0, 1.0);
  if (coverage <= 0.0) {
    discard;
  }
  fragmentColor = vec4(color.rgb, color.a * coverage);
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

export class TraceRenderer implements FrameRenderer {
  private readonly canvas: HTMLCanvasElement;
  private readonly gl: WebGL2RenderingContext;
  private readonly trace: Program;
  private readonly plain: Program;
  // The frame's values as received, a row of texels at a time, and how many rows it holds.
  private readonly frameTexture: WebGLTexture;
  private textureRowsAllocated = 0;
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
    // No multisampling: the trace shader works out each pixel's coverage itself, and each pixel
    // must be shaded once for a translucent trace to blend once.
    const gl = canvas.getContext("webgl2", { antialias: false, alpha: true });
    if (gl === null) {
      throw new Error("WebGL2 is not available");
    }
    this.canvas = canvas;
    this.gl = gl;
    this.style = style;
    this.view = view;
    this.trace = new Program(gl, traceVertexShader, traceFragmentShader);
    this.plain = new Program(gl, plainShader, plainFragmentShader);
    this.shadingBuffer = gl.createBuffer();
    this.frameTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.frameTexture);
    // An integer texture is complete only with nearest filtering and no mipmaps.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.enable(gl.BLEND);
    // The canvas is composited as premultiplied: alpha accumulates as coverage.
    gl.blendFuncSeparate(
      gl.SRC_ALPHA,
      gl.ONE_MINUS_SRC_ALPHA,
      gl.ONE,
      gl.ONE_MINUS_SRC_ALPHA,
    );
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

  // The values of all blocks, which lie one after another in the frame's bytes, as texels.
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
    const texels = new Uint32Array(first.buffer, start, (end - start) / 4);
    const { rows, full, remainder } = textureRows(texels.length);
    gl.bindTexture(gl.TEXTURE_2D, this.frameTexture);
    if (rows > this.textureRowsAllocated) {
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.R32UI,
        textureRowLength,
        rows,
        0,
        gl.RED_INTEGER,
        gl.UNSIGNED_INT,
        null,
      );
      this.textureRowsAllocated = rows;
    }
    if (full > 0) {
      gl.texSubImage2D(
        gl.TEXTURE_2D,
        0,
        0,
        0,
        textureRowLength,
        full,
        gl.RED_INTEGER,
        gl.UNSIGNED_INT,
        texels.subarray(0, full * textureRowLength),
      );
    }
    if (remainder > 0) {
      gl.texSubImage2D(
        gl.TEXTURE_2D,
        0,
        0,
        full,
        remainder,
        1,
        gl.RED_INTEGER,
        gl.UNSIGNED_INT,
        texels.subarray(full * textureRowLength),
      );
    }
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
    gl.useProgram(program.program);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.frameTexture);
    gl.uniform1i(program.uniform("frameValues"), 0);
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
    gl.uniform1i(program.uniform("stride"), minMax ? 2 : 1);
    gl.uniform1i(program.uniform("reach"), reachCount(lineHalf + 0.5, spacing));
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
    blocks.forEach(([values, color], index) => {
      const lane = blockLanes[index] ?? { center: 0, halfHeight: 1 };
      gl.uniform1f(program.uniform("center"), lane.center);
      gl.uniform1f(program.uniform("halfHeight"), lane.halfHeight);
      gl.uniform1i(
        program.uniform("blockStart"),
        (values.byteOffset - start) / 4,
      );
      gl.uniform4f(
        program.uniform("color"),
        color[0],
        color[1],
        color[2],
        alpha(color),
      );
      gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, frame.pointCount);
    });
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
