/**
 * Pure geometry builders for GPU marks (data space, no three.js), so the
 * shapes are unit-tested independently of rendering.
 */

import type { NumArray } from './marks';

const finite = (...v: number[]) => v.every(Number.isFinite);

/**
 * Line segments between consecutive finite points: [ax, ay, bx, by] per
 * segment. A non-finite value breaks the line (gap), it is not bridged.
 */
export function lineSegments(xs: NumArray, ys: NumArray): Float32Array {
  const n = Math.min(xs.length, ys.length);
  const out: number[] = [];
  for (let i = 0; i + 1 < n; i++) {
    if (finite(xs[i], ys[i], xs[i + 1], ys[i + 1])) out.push(xs[i], ys[i], xs[i + 1], ys[i + 1]);
  }
  return Float32Array.from(out);
}

/**
 * Triangles filling between a lower and an upper edge, two per segment:
 * per vertex (x, y, v) with v = 0 on the lower edge and 1 on the upper edge.
 * `y0s === null` uses a constant `baseline`.
 */
export function areaTriangles(xs: NumArray, y0s: NumArray | null, y1s: NumArray, baseline = 0): Float32Array {
  const n = Math.min(xs.length, y1s.length, y0s ? y0s.length : Infinity);
  const lo = (i: number) => (y0s ? y0s[i] : baseline);
  const out: number[] = [];
  for (let i = 0; i + 1 < n; i++) {
    const xa = xs[i], xb = xs[i + 1];
    const la = lo(i), lb = lo(i + 1), ha = y1s[i], hb = y1s[i + 1];
    if (!finite(xa, xb, la, lb, ha, hb)) continue;
    // (a,lo) (b,lo) (b,hi)  +  (a,lo) (b,hi) (a,hi)
    out.push(xa, la, 0, xb, lb, 0, xb, hb, 1);
    out.push(xa, la, 0, xb, hb, 1, xa, ha, 1);
  }
  return Float32Array.from(out);
}

/** Index of the x closest to `x` in ascending `xs` (binary search). */
export function nearestIndex(xs: NumArray, x: number): number {
  const n = xs.length;
  if (n === 0 || !Number.isFinite(x)) return -1;
  let lo = 0, hi = n - 1;
  if (x <= xs[0]) return 0;
  if (x >= xs[hi]) return hi;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (xs[mid] <= x) lo = mid;
    else hi = mid;
  }
  return x - xs[lo] <= xs[hi] - x ? lo : hi;
}

/** True when `xs` is non-decreasing (crosshair lookup requires it). */
export function isSorted(xs: NumArray): boolean {
  for (let i = 1; i < xs.length; i++) if (xs[i] < xs[i - 1]) return false;
  return true;
}

/**
 * Starting values for an animated series. When the vertex count matches the
 * previous series, each vertex moves from where it is now; otherwise the
 * caller cross-fades instead (returns null).
 */
export function morphFrom(prevNow: NumArray | null, next: NumArray): Float32Array | null {
  if (!prevNow || prevNow.length !== next.length) return null;
  return Float32Array.from(prevNow as ArrayLike<number>);
}

/** Interpolate two equal-length arrays. */
export function lerpArrays(a: NumArray, b: NumArray, t: number): Float32Array {
  const n = Math.min(a.length, b.length);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) out[i] = a[i] + (b[i] - a[i]) * t;
  return out;
}

export interface BarRect {
  index: number;
  /** Category centre on the band axis (index), value extent on the value axis. */
  center: number;
  halfHeight: number;
  v0: number;
  v1: number;
}

/**
 * Horizontal bars anchored at `baseline`. `padding` is the fraction of each
 * band left empty (the surface gap between adjacent bars).
 */
export function barRects(values: NumArray, padding = 0.28, baseline = 0): BarRect[] {
  const out: BarRect[] = [];
  const half = (1 - Math.min(0.9, Math.max(0, padding))) / 2;
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    if (!Number.isFinite(v)) continue;
    out.push({ index: i, center: i, halfHeight: half, v0: Math.min(baseline, v), v1: Math.max(baseline, v) });
  }
  return out;
}

// ---------------------------------------------------------------------------
// Line transitions
// ---------------------------------------------------------------------------

export interface SegmentPlan {
  /** Per segment: [ax0, ay0, ax1, ay1] (from -> to of endpoint A). */
  a: number[];
  b: number[];
  alpha: number[];
  /** Vertex positions (interleaved x, y) the line ends at; null if none. */
  target: Float32Array | null;
}

export function interleave(xs: ArrayLike<number>, ys: ArrayLike<number>): Float32Array {
  const n = Math.min(xs.length, ys.length);
  const out = new Float32Array(2 * n);
  for (let i = 0; i < n; i++) {
    out[2 * i] = xs[i];
    out[2 * i + 1] = ys[i];
  }
  return out;
}

const ok = (v: Float32Array, i: number) => Number.isFinite(v[2 * i]) && Number.isFinite(v[2 * i + 1]);

/**
 * Plan segment instances for a transition from vertex positions `now`
 * (where the line is at this moment, or null) to `next`.
 */
export function planLine(now: Float32Array | null, next: Float32Array): SegmentPlan {
  const plan: SegmentPlan = { a: [], b: [], alpha: [], target: next };
  const nNext = next.length / 2;
  const push = (from: Float32Array, to: Float32Array, i: number, a0: number, a1: number) => {
    plan.a.push(from[2 * i], from[2 * i + 1], to[2 * i], to[2 * i + 1]);
    plan.b.push(from[2 * i + 2], from[2 * i + 3], to[2 * i + 2], to[2 * i + 3]);
    plan.alpha.push(a0, a1);
  };
  if (now && now.length === next.length) {
    for (let i = 0; i + 1 < nNext; i++) {
      if (ok(next, i) && ok(next, i + 1) && ok(now, i) && ok(now, i + 1)) push(now, next, i, 1, 1);
      else if (ok(next, i) && ok(next, i + 1)) push(next, next, i, 0, 1);
    }
    return plan;
  }
  if (now) {
    const nNow = now.length / 2;
    for (let i = 0; i + 1 < nNow; i++) if (ok(now, i) && ok(now, i + 1)) push(now, now, i, 1, 0);
  }
  for (let i = 0; i + 1 < nNext; i++) if (ok(next, i) && ok(next, i + 1)) push(next, next, i, 0, 1);
  return plan;
}


// ---------------------------------------------------------------------------
// Area transitions
// ---------------------------------------------------------------------------

export interface AreaPlan {
  /** Per vertex (x, y) at the start of the transition. */
  from: Float32Array;
  /** Per vertex (x, y) at the end. */
  to: Float32Array;
  /** Per vertex: 0 on the lower edge, 1 on the upper edge. */
  v: Float32Array;
  /** Per vertex (alpha at start, alpha at end). */
  alpha: Float32Array;
}

/**
 * `now` and `next` are triangle lists from areaTriangles() (x, y, v per
 * vertex). Same vertex count: morph. Otherwise cross-fade.
 */
export function planArea(now: Float32Array | null, next: Float32Array): AreaPlan {
  const nNext = next.length / 3;
  if (now && now.length === next.length) {
    const from = new Float32Array(2 * nNext), to = new Float32Array(2 * nNext);
    const v = new Float32Array(nNext), alpha = new Float32Array(2 * nNext);
    for (let i = 0; i < nNext; i++) {
      from[2 * i] = now[3 * i]; from[2 * i + 1] = now[3 * i + 1];
      to[2 * i] = next[3 * i]; to[2 * i + 1] = next[3 * i + 1];
      v[i] = next[3 * i + 2];
      alpha[2 * i] = 1; alpha[2 * i + 1] = 1;
    }
    return { from, to, v, alpha };
  }
  const nNow = now ? now.length / 3 : 0;
  const total = nNow + nNext;
  const from = new Float32Array(2 * total), to = new Float32Array(2 * total);
  const v = new Float32Array(total), alpha = new Float32Array(2 * total);
  let k = 0;
  const put = (src: Float32Array, i: number, a0: number, a1: number) => {
    from[2 * k] = to[2 * k] = src[3 * i];
    from[2 * k + 1] = to[2 * k + 1] = src[3 * i + 1];
    v[k] = src[3 * i + 2];
    alpha[2 * k] = a0; alpha[2 * k + 1] = a1;
    k++;
  };
  for (let i = 0; i < nNow; i++) put(now!, i, 1, 0);
  for (let i = 0; i < nNext; i++) put(next, i, 0, 1);
  return { from, to, v, alpha };
}

// ---------------------------------------------------------------------------
// Bar transitions (keyed by category)
// ---------------------------------------------------------------------------

export interface BarNow {
  category: string;
  center: number;
  v0: number;
  v1: number;
  alpha: number;
  sel: number;
}

export interface BarPlanned {
  category: string;
  /** Index into the incoming values, or -1 for an exiting bar. */
  dataIndex: number;
  c0: number; c1: number;
  v00: number; v01: number; // v0 from, v1 from
  v10: number; v11: number; // v0 to, v1 to
  a0: number; a1: number;
  s0: number; s1: number;
}

/**
 * Bars keyed by category: matching bars change length and slide to their
 * new row; new bars grow from the baseline; missing bars shrink and fade.
 */
export function planBars(
  now: BarNow[], categories: string[], values: NumArray, selected: string | null, baseline = 0,
): BarPlanned[] {
  const prev = new Map(now.map((b) => [b.category, b]));
  const out: BarPlanned[] = [];
  const seen = new Set<string>();
  const n = Math.min(categories.length, values.length);
  for (let i = 0; i < n; i++) {
    const cat = categories[i];
    const v = values[i];
    if (!Number.isFinite(v) || seen.has(cat)) continue;
    seen.add(cat);
    const lo = Math.min(baseline, v), hi = Math.max(baseline, v);
    const s1 = selected !== null && cat === selected ? 1 : 0;
    const old = prev.get(cat);
    out.push(old
      ? { category: cat, dataIndex: i, c0: old.center, c1: i, v00: old.v0, v01: old.v1, v10: lo, v11: hi,
          a0: old.alpha, a1: 1, s0: old.sel, s1 }
      : { category: cat, dataIndex: i, c0: i, c1: i, v00: baseline, v01: baseline, v10: lo, v11: hi,
          a0: 1, a1: 1, s0: 0, s1 });
  }
  for (const b of now) {
    if (seen.has(b.category) || b.alpha <= 0.001) continue;
    out.unshift({ category: b.category, dataIndex: -1, c0: b.center, c1: b.center, v00: b.v0, v01: b.v1,
      v10: baseline, v11: baseline, a0: b.alpha, a1: 0, s0: b.sel, s1: 0 });
  }
  return out;
}
