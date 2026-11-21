/** Linear domains, "nice" tick generation and easing for chart transitions. */

export interface Domain {
  x0: number;
  x1: number;
  y0: number;
  y1: number;
}

export interface AxisSpec {
  label?: string;
  format?: string;
  min?: number;
  max?: number;
  zero?: boolean;
}

export function niceStep(span: number, count: number): number {
  if (!(span > 0) || !isFinite(span)) return 1;
  const raw = span / Math.max(1, count);
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const nice = norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10;
  return nice * mag;
}

export function ticks(lo: number, hi: number, count: number): { values: number[]; step: number } {
  const step = niceStep(hi - lo, count);
  const start = Math.ceil(lo / step - 1e-9) * step;
  const values: number[] = [];
  for (let v = start; v <= hi + step * 1e-9; v += step) {
    values.push(Math.abs(v) < step * 1e-9 ? 0 : v);
    if (values.length > 50) break;
  }
  return { values, step };
}

/**
 * Domain for one axis: data extent, optional zero, padded, then expanded to
 * the nice tick grid so the outermost gridlines frame the data.
 */
export function axisDomain(
  lo: number,
  hi: number,
  spec: AxisSpec | undefined,
  tickCount: number,
): [number, number] {
  if (!isFinite(lo) || !isFinite(hi)) {
    lo = 0;
    hi = 1;
  }
  if (spec?.zero) {
    lo = Math.min(lo, 0);
    hi = Math.max(hi, 0);
  }
  if (hi - lo < 1e-12) {
    const pad = Math.abs(hi) * 0.1 || 1;
    lo -= pad;
    hi += pad;
  }
  const pad = (hi - lo) * 0.06;
  let a = lo - (spec?.zero && lo === 0 ? 0 : pad);
  let b = hi + (spec?.zero && hi === 0 ? 0 : pad);
  const step = niceStep(b - a, tickCount);
  a = Math.floor(a / step + 1e-9) * step;
  b = Math.ceil(b / step - 1e-9) * step;
  if (typeof spec?.min === 'number') a = spec.min;
  if (typeof spec?.max === 'number') b = spec.max;
  return [a, b];
}

export function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

export function lerpDomain(a: Domain, b: Domain, t: number): Domain {
  return {
    x0: lerp(a.x0, b.x0, t),
    x1: lerp(a.x1, b.x1, t),
    y0: lerp(a.y0, b.y0, t),
    y1: lerp(a.y1, b.y1, t),
  };
}

/** Smooth, slightly front-loaded ease used for every data transition. */
export function easeInOutCubic(t: number): number {
  const c = Math.min(1, Math.max(0, t));
  return c < 0.5 ? 4 * c * c * c : 1 - Math.pow(-2 * c + 2, 3) / 2;
}
