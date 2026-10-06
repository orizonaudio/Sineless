// Hand-written WebGL2 renderer for the traces (AD-5). Grid, labels and controls are DOM.
import type { ChannelView, ViewLayout } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import type { FrameRenderer } from "./render-loop.ts";
import { traceGeometry } from "./trace-geometry.ts";

export type Rgba = readonly [number, number, number, number];

export interface TraceStyle {
  // Resolved at draw time, so colours can come from a stylesheet that loads after start-up.
  readonly colors: () => ReadonlyMap<ChannelView, Rgba>;
  readonly lineWidthPixels: number;
}

export interface TraceView {
  readonly verticalScaleDb: number;
  readonly layout: ViewLayout;
  // Shows the trace as stale, e.g. while Normal mode waits for a trigger.
  readonly dim?: boolean;
}

const vertexShader = `#version 300 es
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

export class TraceRenderer implements FrameRenderer {
  private readonly canvas: HTMLCanvasElement;
  private readonly gl: WebGL2RenderingContext;
  private readonly colorLocation: WebGLUniformLocation | null;
  private readonly buffer: WebGLBuffer;
  private readonly style: TraceStyle;
  private readonly view: () => TraceView;
  // Average CPU time of draw(), in milliseconds, for profiling (T-031).
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

    const program = gl.createProgram();
    gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, vertexShader));
    gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, fragmentShader));
    gl.linkProgram(program);
    if (gl.getProgramParameter(program, gl.LINK_STATUS) !== true) {
      throw new Error(
        `Shader linking failed: ${gl.getProgramInfoLog(program) ?? ""}`,
      );
    }
    gl.useProgram(program);
    this.colorLocation = gl.getUniformLocation(program, "color");
    this.buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
    const position = gl.getAttribLocation(program, "position");
    gl.enableVertexAttribArray(position);
    gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  }

  draw(frame: Frame): void {
    const started = performance.now();
    const gl = this.gl;
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const view = this.view();
    const geometry = traceGeometry(frame, {
      widthPixels: this.canvas.width,
      heightPixels: this.canvas.height,
      verticalScaleDb: view.verticalScaleDb,
      layout: view.layout,
      lineWidthPixels: this.style.lineWidthPixels,
    });
    const colors = this.style.colors();
    for (const [channelView, vertices] of geometry) {
      const color = colors.get(channelView) ?? [1, 1, 1, 1];
      const alpha = view.dim === true ? color[3] * 0.45 : color[3];
      gl.uniform4f(this.colorLocation, color[0], color[1], color[2], alpha);
      gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);
      gl.drawArrays(gl.TRIANGLES, 0, vertices.length / 2);
    }
    const elapsed = performance.now() - started;
    this.drawMilliseconds = this.drawMilliseconds * 0.95 + elapsed * 0.05;
  }
}
