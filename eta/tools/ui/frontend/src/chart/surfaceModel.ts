/**
 * 3D surface model (pure): parsing, validation, smooth resampling and the
 * mapping into a unit box. Rendering lives in Surface.tsx.
 *
 * Grid convention (from std.chart): xs has nx values, ys has ny values and
 * zs is row-major with ny rows of nx values, so z(i, j) = zs[j * nx + i].
 */

import { toNumbers } from './marks';
import type { AxisSpec } from './scale';

export interface SurfaceSpec {
  xs: Float64Array;
  ys: Float64Array;
  zs: Float64Array;
  nx: number;
  ny: number;
  x: AxisSpec;
  y: AxisSpec;
  z: AxisSpec;
}

function axis(v: unknown): AxisSpec {
  const p = (v && typeof v === 'object' ? (v as { p?: Record<string, unknown> }).p : undefined) ?? {};
  return {
    label: typeof p.label === 'string' ? p.label : undefined,
    format: typeof p.format === 'string' ? p.format : undefined,
  };
}

const f64 = (v: unknown) => Float64Array.from(toNumbers(v) as ArrayLike<number>);

export function parseSurface(p: Record<string, unknown>): SurfaceSpec {
  const xs = f64(p.xs), ys = f64(p.ys), zs = f64(p.zs);
  return { xs, ys, zs, nx: xs.length, ny: ys.length, x: axis(p.x), y: axis(p.y), z: axis(p.z) };
}

export function validateSurface(s: SurfaceSpec): string | null {
  if (s.nx < 2 || s.ny < 2) return 'surface needs at least a 2 × 2 grid';
  if (s.zs.length !== s.nx * s.ny) return `surface expects ${s.ny} × ${s.nx} = ${s.nx * s.ny} values, got ${s.zs.length}`;
  return null;
}

export function zRange(zs: ArrayLike<number>): [number, number] {
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < zs.length; i++) {
    const v = zs[i];
    if (!Number.isFinite(v)) continue;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (!Number.isFinite(lo)) return [0, 1];
  if (hi - lo < 1e-12) return [lo - 0.5, hi + 0.5];
  return [lo, hi];
}

/** Catmull-Rom interpolation of p1..p2 (p0, p3 neighbours) at t in [0, 1]. */
export function catmullRom(p0: number, p1: number, p2: number, p3: number, t: number): number {
  const t2 = t * t, t3 = t2 * t;
  return 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}

/** Resample a 1D series to n points (passes through the originals). */
export function resample1d(v: ArrayLike<number>, n: number): Float64Array {
  const m = v.length;
  const out = new Float64Array(n);
  if (m === 0) return out;
  if (m === 1 || n === 1) return out.fill(v[0]);
  for (let k = 0; k < n; k++) {
    const pos = (k / (n - 1)) * (m - 1);
    const i = Math.min(m - 2, Math.floor(pos));
    const t = pos - i;
    const p1 = v[i], p2 = v[i + 1];
    // Ghost points beyond the ends are extrapolated linearly, so edge
    // intervals do not bend (linear data is reproduced exactly).
    const p0 = i > 0 ? v[i - 1] : 2 * p1 - p2;
    const p3 = i + 2 < m ? v[i + 2] : 2 * p2 - p1;
    out[k] = catmullRom(p0, p1, p2, p3, t);
  }
  return out;
}

/**
 * Smoothly upsample a row-major grid (ny rows of nx) to (oy rows of ox).
 * Separable Catmull-Rom: rows first, then columns.
 */
export function resampleGrid(z: ArrayLike<number>, nx: number, ny: number, ox: number, oy: number): Float64Array {
  const rows: Float64Array[] = [];
  for (let j = 0; j < ny; j++) {
    const row = new Float64Array(nx);
    for (let i = 0; i < nx; i++) row[i] = z[j * nx + i];
    rows.push(resample1d(row, ox));
  }
  const out = new Float64Array(ox * oy);
  const col = new Float64Array(ny);
  for (let i = 0; i < ox; i++) {
    for (let j = 0; j < ny; j++) col[j] = rows[j][i];
    const c = resample1d(col, oy);
    for (let j = 0; j < oy; j++) out[j * ox + i] = c[j];
  }
  return out;
}

/** Output resolution for display: about `target` samples along each axis. */
export function displayResolution(n: number, target = 64): number {
  return Math.max(n, Math.min(target, (n - 1) * 4 + 1));
}

/** Box mapping: x, y into [-1, 1], z into [0, height]. */
export interface Box {
  height: number;
  zMin: number;
  zMax: number;
}

export const boxX = (i: number, n: number) => (n <= 1 ? 0 : (i / (n - 1)) * 2 - 1);
export const boxZ = (z: number, b: Box) => ((z - b.zMin) / Math.max(1e-12, b.zMax - b.zMin)) * b.height;

/** Nearest original grid cell to a box-space point (bx, by in [-1, 1]). */
export function nearestCell(bx: number, by: number, nx: number, ny: number): { i: number; j: number } {
  const i = Math.round(((Math.min(1, Math.max(-1, bx)) + 1) / 2) * (nx - 1));
  const j = Math.round(((Math.min(1, Math.max(-1, by)) + 1) / 2) * (ny - 1));
  return { i, j };
}
