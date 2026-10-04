// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { isMeasuredDb } from "../protocol/mirror";
import { reduceMax } from "../model/traces";

/** Rows are stored on one fixed scale, so the gradient can move without
 * touching history: 1..255 over these dB, 0 for nothing measured. */
const kFloorDb = -150;
const kCeilingDb = 10;

export function quantise(db: number): number {
  if (!isMeasuredDb(db)) {
    return 0;
  }
  return 1 + Math.round(Math.min(Math.max(((db - kFloorDb) / (kCeilingDb - kFloorDb)) * 254, 0), 254));
}

const vertexSource = `#version 300 es
const vec2 corners[4] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1));
out vec2 position;
void main() {
  vec2 corner = corners[gl_VertexID];
  position = corner * 0.5 + 0.5;
  gl_Position = vec4(corner, 0, 1);
}`;

// Each screen column takes the strongest bin it covers, so a one-bin carrier
// survives zooming out; each screen row is one stored line, newest at the top.
const fragmentSource = `#version 300 es
precision highp float;
precision highp int;
in vec2 position;
out vec4 colour;
uniform highp usampler2D history;
uniform sampler2D colourMap;
uniform vec2 view;          // Hz shown at the left and right edges
uniform vec2 span;          // Hz the stored rows cover
uniform int bins;
uniform int lines;
uniform int writeRow;       // where the next line will go
uniform int pushed;         // lines stored so far, at most lines
uniform float rowsShown;    // screen rows, one line each
uniform float scroll;       // lines back from the newest at the top
uniform vec2 gradient;      // dB at the bottom and top of the colour map
uniform float pixelWidth;   // of one screen column, in the 0..1 of position.x
uniform vec4 background;
uniform bool peak;          // the strongest bin under a column, or its centre's
const float floorDb = ${kFloorDb.toFixed(1)};
const float ceilingDb = ${kCeilingDb.toFixed(1)};

void main() {
  float age = floor((1.0 - position.y) * rowsShown) + scroll;
  if (age >= float(pushed)) { colour = background; return; }
  int row = (writeRow - 1 - int(age) + lines * 2) % lines;

  float hzLeft = mix(view.x, view.y, position.x - pixelWidth * 0.5);
  float hzRight = mix(view.x, view.y, position.x + pixelWidth * 0.5);
  float first = (hzLeft - span.x) / (span.y - span.x) * float(bins);
  float last = (hzRight - span.x) / (span.y - span.x) * float(bins);
  int a = int(floor(first));
  int b = max(int(ceil(last)), a + 1);
  if (b <= 0 || a >= bins) { colour = background; return; }
  a = clamp(a, 0, bins - 1);
  b = clamp(b, a + 1, bins);
  if (!peak) {
    a = clamp(int(floor((first + last) * 0.5)), 0, bins - 1);
    b = a + 1;
  }
  uint strongest = 0u;
  for (int i = 0; i < 64; ++i) {
    int bin = a + (b - a) * i / 64;
    if (i > 0 && bin == a + (b - a) * (i - 1) / 64) { continue; }
    if (bin >= b) { break; }
    strongest = max(strongest, texelFetch(history, ivec2(bin, row), 0).r);
  }
  if (strongest == 0u) { colour = background; return; }
  float db = floorDb + (float(strongest) - 1.0) / 254.0 * (ceilingDb - floorDb);
  float t = clamp((db - gradient.x) / max(gradient.y - gradient.x, 0.001), 0.0, 1.0);
  colour = texture(colourMap, vec2(t, 0.5));
}`;

function compile(gl: WebGL2RenderingContext, type: number, source: string): WebGLShader {
  const shader = gl.createShader(type)!;
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    throw new Error(gl.getShaderInfoLog(shader) ?? "the waterfall shader did not compile");
  }
  return shader;
}

export interface WaterfallView {
  fromHz: number;
  toHz: number;
  gradientMinDb: number;
  gradientMaxDb: number;
  background: [number, number, number, number];
  /** Every bin under a column counts, not only the one at its centre. */
  peakDetect: boolean;
}

/** One panel's waterfall: a ring of lines in a texture, drawn through the
 * colour map by the GPU. */
export class WaterfallRenderer {
  private readonly gl: WebGL2RenderingContext;
  private readonly program: WebGLProgram;
  private readonly uniforms: Record<string, WebGLUniformLocation | null> = {};
  private history: WebGLTexture | null = null;
  private colourMap: WebGLTexture;
  private bins = 0;
  private lines = 0;
  private writeRow = 0;
  private pushed = 0;
  private spanStartHz = 0;
  private spanStopHz = 0;
  private scratch = new Uint8Array(0);
  readonly maxTextureSize: number;
  /** Lines back from the newest, when scrolled into history. */
  scroll = 0;

  constructor(readonly canvas: HTMLCanvasElement) {
    const gl = canvas.getContext("webgl2", { antialias: false, alpha: false, preserveDrawingBuffer: true });
    if (!gl) {
      throw new Error("this browser has no WebGL2");
    }
    this.gl = gl;
    this.maxTextureSize = gl.getParameter(gl.MAX_TEXTURE_SIZE) as number;
    const program = gl.createProgram()!;
    gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, vertexSource));
    gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, fragmentSource));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error(gl.getProgramInfoLog(program) ?? "the waterfall shader did not link");
    }
    this.program = program;
    for (const name of [
      "history",
      "colourMap",
      "view",
      "span",
      "bins",
      "lines",
      "writeRow",
      "pushed",
      "rowsShown",
      "scroll",
      "gradient",
      "pixelWidth",
      "background",
      "peak",
    ]) {
      this.uniforms[name] = gl.getUniformLocation(program, name);
    }
    this.colourMap = gl.createTexture()!;
    this.setColourMap(new Uint8Array(256 * 4).fill(255));
  }

  get linesStored(): number {
    return this.pushed;
  }

  setColourMap(lut: Uint8Array): void {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.colourMap);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, lut);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  }

  /** History for `bins` columns and `lines` rows; any other size starts it
   * again. */
  private ensure(bins: number, lines: number): void {
    bins = Math.min(bins, this.maxTextureSize);
    lines = Math.min(lines, this.maxTextureSize);
    if (this.history && bins === this.bins && lines === this.lines) {
      return;
    }
    const gl = this.gl;
    if (this.history) {
      gl.deleteTexture(this.history);
    }
    this.history = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, this.history);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R8UI, bins, lines);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    this.bins = bins;
    this.lines = lines;
    this.writeRow = 0;
    this.pushed = 0;
    this.scroll = 0;
    this.scratch = new Uint8Array(bins);
  }

  clear(): void {
    this.pushed = 0;
    this.writeRow = 0;
    this.scroll = 0;
  }

  /** One line onto the top. A line over another grid -- a retune, a new span
   * -- starts the history again: old rows mean other frequencies. */
  push(levels: Float32Array, startHz: number, stopHz: number, depth: number): void {
    if (levels.length === 0) {
      return;
    }
    const tolerance = this.bins > 0 ? (this.spanStopHz - this.spanStartHz) / this.bins : 1e-3;
    const regridded =
      Math.abs(startHz - this.spanStartHz) >= tolerance || Math.abs(stopHz - this.spanStopHz) >= tolerance;
    this.ensure(levels.length, depth);
    if (regridded) {
      this.clear();
    }
    this.spanStartHz = startHz;
    this.spanStopHz = stopHz;

    const fitted = reduceMax(levels, this.bins);
    for (let i = 0; i < this.bins; ++i) {
      this.scratch[i] = i < fitted.length ? quantise(fitted[i]!) : 0;
    }
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.history);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, this.writeRow, this.bins, 1, gl.RED_INTEGER, gl.UNSIGNED_BYTE, this.scratch);
    this.writeRow = (this.writeRow + 1) % this.lines;
    this.pushed = Math.min(this.pushed + 1, this.lines);
    if (this.scroll > 0) {
      // Scrolled back means anchored to what is being read.
      this.scroll = Math.min(this.scroll + 1, Math.max(this.pushed - 1, 0));
    }
  }

  draw(view: WaterfallView): void {
    const gl = this.gl;
    const width = this.canvas.width;
    const height = this.canvas.height;
    gl.viewport(0, 0, width, height);
    gl.clearColor(...view.background);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (!this.history || this.pushed === 0) {
      return;
    }
    gl.useProgram(this.program);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.history);
    gl.uniform1i(this.uniforms.history!, 0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.colourMap);
    gl.uniform1i(this.uniforms.colourMap!, 1);
    gl.uniform2f(this.uniforms.view!, view.fromHz, view.toHz);
    gl.uniform2f(this.uniforms.span!, this.spanStartHz, this.spanStopHz);
    gl.uniform1i(this.uniforms.bins!, this.bins);
    gl.uniform1i(this.uniforms.lines!, this.lines);
    gl.uniform1i(this.uniforms.writeRow!, this.writeRow);
    gl.uniform1i(this.uniforms.pushed!, this.pushed);
    gl.uniform1f(this.uniforms.rowsShown!, height);
    gl.uniform1f(this.uniforms.scroll!, this.scroll);
    gl.uniform2f(this.uniforms.gradient!, view.gradientMinDb, view.gradientMaxDb);
    gl.uniform1f(this.uniforms.pixelWidth!, 1 / Math.max(width, 1));
    gl.uniform4f(this.uniforms.background!, ...view.background);
    gl.uniform1i(this.uniforms.peak!, view.peakDetect ? 1 : 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  dispose(): void {
    const gl = this.gl;
    if (this.history) {
      gl.deleteTexture(this.history);
    }
    gl.deleteTexture(this.colourMap);
    gl.deleteProgram(this.program);
  }
}
