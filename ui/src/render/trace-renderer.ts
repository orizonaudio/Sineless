// Hand-written WebGL2 renderer for the traces. Grid, labels and controls are DOM.
//
// The trace geometry is built on the GPU: a frame's bytes are uploaded as they arrived, and a
// vertex shader turns each min/max column, or each segment between two raw points, into one
// instance of a quad (two triangles, six vertices). The CPU builds no vertices, so the cost of a
// frame hardly depends on its size.
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

// Shared by both trace shaders: a value's height in its lane, and a frame position's x, in clip
// space. A frame covers frameStart to frameStart + frameWidth of the span; the screen shows
// viewStart to viewStart + viewWidth of it.
const placement = `
uniform float center;
uniform float halfHeight;
uniform float scale;
uniform float frameStart;
uniform float frameWidth;
uniform float viewStart;
uniform float viewWidth;
float heightOf(float value) { return center + value / scale * halfHeight; }
float screenX(float position) { return -1.0 + 2.0 * (position - viewStart) / viewWidth; }
// Corners of an instance's quad, in the order of two triangles (0, 1, 2) and (2, 1, 3): whether
// the vertex is at the far end, and whether it is on the upper (or positive) side.
bool farEnd() { int corner = gl_VertexID % 6; return corner == 2 || corner == 3 || corner == 5; }
bool upper() { int corner = gl_VertexID % 6; return corner == 1 || corner == 4 || corner == 5; }
`;

// One instance per min/max column: from min to max, at least one line width tall. A column lying
// wholly above or below its predecessor is extended to meet it: the signal passes through that
// gap, and without the join, columns holding only one or two samples would show as dots.
const bandShader = `#version 300 es
in vec2 column;
in vec2 previous;
uniform float columns;
uniform float minimumHalf;
${placement}
void main() {
  float low = heightOf(column.x);
  float high = heightOf(column.y);
  if (gl_InstanceID > 0) {
    float previousLow = heightOf(previous.x);
    float previousHigh = heightOf(previous.y);
    if (low > previousHigh) {
      low = previousHigh;
    } else if (high < previousLow) {
      high = previousLow;
    }
  }
  if (high - low < 2.0 * minimumHalf) {
    float middle = (low + high) * 0.5;
    low = middle - minimumHalf;
    high = middle + minimumHalf;
  }
  float edge = float(gl_InstanceID) + (farEnd() ? 1.0 : 0.0);
  gl_Position = vec4(screenX(frameStart + edge / columns * frameWidth), upper() ? high : low,
                     0.0, 1.0);
}`;

// One instance per segment between raw points i and i + 1, a line `halfWidth` pixels either side,
// offset perpendicular to the segment in pixels so the width is even.
const lineShader = `#version 300 es
in float first;
in float second;
uniform float samples;
uniform float startFraction;
uniform vec2 toPixels;
uniform float halfWidth;
${placement}
void main() {
  float index = float(gl_InstanceID);
  vec2 start = vec2(screenX(frameStart + (index - startFraction) / samples * frameWidth),
                    heightOf(first));
  vec2 end = vec2(screenX(frameStart + (index + 1.0 - startFraction) / samples * frameWidth),
                  heightOf(second));
  vec2 pixels = (end - start) * toPixels;
  float pixelLength = length(pixels);
  if (pixelLength == 0.0) {
    pixelLength = 1.0;
  }
  vec2 offset = vec2(-pixels.y, pixels.x) / pixelLength * halfWidth / toPixels;
  vec2 point = farEnd() ? end : start;
  gl_Position = vec4(upper() ? point + offset : point - offset, 0.0, 1.0);
}`;

// Ready-made clip-space triangles, for the cancellation shading.
const plainShader = `#version 300 es
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }`;

const fragmentShader = `#version 300 es
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

  constructor(gl: WebGL2RenderingContext, vertexSource: string) {
    this.gl = gl;
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, vertexSource));
    gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, fragmentShader));
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
  private readonly band: Program;
  private readonly line: Program;
  private readonly plain: Program;
  // The frame's bytes as received, and the cancellation shading's triangles.
  private readonly frameBuffer: WebGLBuffer;
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
    const gl = canvas.getContext("webgl2", { antialias: true, alpha: true });
    if (gl === null) {
      throw new Error("WebGL2 is not available");
    }
    this.canvas = canvas;
    this.gl = gl;
    this.style = style;
    this.view = view;
    this.band = new Program(gl, bandShader);
    this.line = new Program(gl, lineShader);
    this.plain = new Program(gl, plainShader);
    this.frameBuffer = gl.createBuffer();
    this.shadingBuffer = gl.createBuffer();
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
    gl.vertexAttribDivisor(position, 0);
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
    const first = blocks[0]?.[0];
    if (first === undefined || frame.pointCount === 0) {
      if (sweep !== undefined) {
        this.drawSweep(sweep);
      }
      this.finish(started);
      return;
    }
    // The whole received frame, once; each block is read in place at its byte offset.
    gl.bindBuffer(gl.ARRAY_BUFFER, this.frameBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Uint8Array(first.buffer, 0, first.buffer.byteLength),
      gl.STREAM_DRAW,
    );
    const minMax = frame.mode === FrameMode.minMax;
    const program = minMax ? this.band : this.line;
    gl.useProgram(program.program);
    const scale = fullScale(view.verticalScaleDb);
    gl.uniform1f(program.uniform("scale"), scale);
    gl.uniform1f(program.uniform("frameStart"), frame.viewStart);
    gl.uniform1f(
      program.uniform("frameWidth"),
      frame.viewEnd - frame.viewStart,
    );
    gl.uniform1f(program.uniform("viewStart"), screen.start);
    gl.uniform1f(program.uniform("viewWidth"), screen.end - screen.start);
    const [current, other] = minMax
      ? [program.attribute("column"), program.attribute("previous")]
      : [program.attribute("first"), program.attribute("second")];
    for (const location of [current, other]) {
      gl.enableVertexAttribArray(location);
      gl.vertexAttribDivisor(location, 1);
    }
    if (minMax) {
      gl.uniform1f(program.uniform("columns"), frame.pointCount);
      gl.uniform1f(
        program.uniform("minimumHalf"),
        this.style.lineWidthPixels / this.canvas.height,
      );
    } else {
      gl.uniform1f(
        program.uniform("samples"),
        frame.visibleSamples > 0 ? frame.visibleSamples : frame.pointCount,
      );
      gl.uniform1f(program.uniform("startFraction"), frame.startFraction);
      gl.uniform2f(
        program.uniform("toPixels"),
        this.canvas.width / 2,
        this.canvas.height / 2,
      );
      gl.uniform1f(
        program.uniform("halfWidth"),
        this.style.lineWidthPixels / 2,
      );
    }

    const blockLanes = lanes(blocks.length, view.layout);
    blocks.forEach(([values, color], index) => {
      const lane = blockLanes[index] ?? { center: 0, halfHeight: 1 };
      gl.uniform1f(program.uniform("center"), lane.center);
      gl.uniform1f(program.uniform("halfHeight"), lane.halfHeight);
      gl.uniform4f(
        program.uniform("color"),
        color[0],
        color[1],
        color[2],
        alpha(color),
      );
      if (minMax) {
        // The previous column starts one pair earlier; the first instance ignores it.
        gl.vertexAttribPointer(
          current,
          2,
          gl.FLOAT,
          false,
          8,
          values.byteOffset,
        );
        gl.vertexAttribPointer(
          other,
          2,
          gl.FLOAT,
          false,
          8,
          Math.max(values.byteOffset - 8, 0),
        );
        gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, frame.pointCount);
      } else if (frame.pointCount > 1) {
        gl.vertexAttribPointer(
          current,
          1,
          gl.FLOAT,
          false,
          4,
          values.byteOffset,
        );
        gl.vertexAttribPointer(
          other,
          1,
          gl.FLOAT,
          false,
          4,
          values.byteOffset + 4,
        );
        gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, frame.pointCount - 1);
      }
    });
    for (const location of [current, other]) {
      gl.vertexAttribDivisor(location, 0);
      gl.disableVertexAttribArray(location);
    }
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
