/**
 * Colour scales for charts.
 *
 * Values come from the validated reference palette (dataviz skill,
 * references/palette.md):
 *  - categorical: eight fixed slots, assigned in order, never cycled;
 *  - sequential: one hue (blue), magnitude = distance from the surface;
 *  - diverging: blue <-> red with a neutral grey midpoint, equal steps per arm.
 *
 * Ramps are interpolated in OKLab so equal value steps look equally different.
 */

export type Rgb = [number, number, number]; // sRGB, 0..1
export type Theme = 'dark' | 'light';

export const CATEGORICAL: Record<Theme, string[]> = {
  dark: ['#3987e5', '#d95926', '#199e70', '#c98500', '#d55181', '#0ca30c', '#9085e9', '#e66767'],
  light: ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#6250d6', '#e34948'],
};

/** Sequential blue ramp, lightest (100) to darkest (700). */
const BLUE_RAMP = ['#cde2fb', '#9ec5f4', '#6da7ec', '#3987e5', '#256abf', '#184f95', '#0d366b'];

const DIVERGING: Record<Theme, { neg: string; mid: string; pos: string }> = {
  // Negative = blue, positive = red (risk up reads warm).
  dark: { neg: '#3987e5', mid: '#383835', pos: '#e66767' },
  light: { neg: '#2a78d6', mid: '#f0efec', pos: '#e34948' },
};

export function hexToRgb(hex: string): Rgb {
  const h = hex.trim().replace('#', '');
  const full = h.length === 3 ? h.split('').map((c) => c + c).join('') : h;
  const n = parseInt(full.slice(0, 6), 16);
  if (Number.isNaN(n)) return [0, 0, 0];
  return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

export function rgbToHex([r, g, b]: Rgb): string {
  const c = (v: number) => Math.round(Math.min(1, Math.max(0, v)) * 255).toString(16).padStart(2, '0');
  return `#${c(r)}${c(g)}${c(b)}`;
}

const toLinear = (c: number) => (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
const toSrgb = (c: number) => (c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055);

export function rgbToOklab([r, g, b]: Rgb): Rgb {
  const lr = toLinear(r), lg = toLinear(g), lb = toLinear(b);
  const l = Math.cbrt(0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb);
  const m = Math.cbrt(0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb);
  const s = Math.cbrt(0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb);
  return [
    0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
  ];
}

export function oklabToRgb([L, a, b]: Rgb): Rgb {
  const l = Math.pow(L + 0.3963377774 * a + 0.2158037573 * b, 3);
  const m = Math.pow(L - 0.1055613458 * a - 0.0638541728 * b, 3);
  const s = Math.pow(L - 0.0894841775 * a - 1.291485548 * b, 3);
  const clamp = (v: number) => Math.min(1, Math.max(0, v));
  return [
    clamp(toSrgb(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s)),
    clamp(toSrgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s)),
    clamp(toSrgb(-0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s)),
  ];
}

/** Interpolate along `stops` (hex) in OKLab; t in [0, 1]. */
export function rampAt(stops: string[], t: number): Rgb {
  if (stops.length === 0) return [0, 0, 0];
  if (stops.length === 1 || !Number.isFinite(t)) return hexToRgb(stops[0]);
  const c = Math.min(1, Math.max(0, t)) * (stops.length - 1);
  const i = Math.min(stops.length - 2, Math.floor(c));
  const f = c - i;
  const a = rgbToOklab(hexToRgb(stops[i]));
  const b = rgbToOklab(hexToRgb(stops[i + 1]));
  return oklabToRgb([a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f]);
}

export interface ColorScale {
  kind: 'sequential' | 'diverging';
  /** Domain: [min, max] for sequential; [min, mid, max] for diverging. */
  domain: number[];
  color(v: number): Rgb;
  /** Evenly spaced legend stops as hex. */
  legend(n: number): string[];
}

/** Sequential: low values recede toward the surface. */
export function sequentialScale(min: number, max: number, theme: Theme = 'dark'): ColorScale {
  // On dark surfaces the darkest step is "near zero"; on light, the lightest.
  // Dark: step 600 -> 200 (no darker than 600, so low values stay >= 2:1
  // against the surface). Light: step 200 -> 700.
  const stops = theme === 'dark' ? [...BLUE_RAMP].reverse().slice(1, 6) : BLUE_RAMP.slice(1);
  const span = max - min || 1;
  const color = (v: number) => rampAt(stops, (v - min) / span);
  return {
    kind: 'sequential',
    domain: [min, max],
    color,
    legend: (n) => Array.from({ length: n }, (_, i) => rgbToHex(rampAt(stops, n === 1 ? 1 : i / (n - 1)))),
  };
}

/**
 * Diverging around `mid`: each arm is scaled by its own extent so equal
 * distances from the midpoint on either side get equal colour steps when the
 * domain is symmetric. Pass a symmetric domain to compare magnitudes fairly.
 */
export function divergingScale(min: number, mid: number, max: number, theme: Theme = 'dark'): ColorScale {
  const d = DIVERGING[theme];
  const negArm = [d.mid, d.neg];
  const posArm = [d.mid, d.pos];
  const color = (v: number): Rgb => {
    if (!Number.isFinite(v)) return hexToRgb(d.mid);
    if (v >= mid) return rampAt(posArm, max > mid ? (v - mid) / (max - mid) : 0);
    return rampAt(negArm, mid > min ? (mid - v) / (mid - min) : 0);
  };
  return {
    kind: 'diverging',
    domain: [min, mid, max],
    color,
    legend: (n) =>
      Array.from({ length: n }, (_, i) => rgbToHex(color(min + ((max - min) * i) / Math.max(1, n - 1)))),
  };
}

/** Symmetric diverging domain around zero covering the data. */
export function symmetricExtent(values: ArrayLike<number>): number {
  let m = 0;
  for (let i = 0; i < values.length; i++) {
    const v = Math.abs(values[i]);
    if (Number.isFinite(v) && v > m) m = v;
  }
  return m || 1;
}

/** Categorical colour for series slot `i` (0-based). Never cycles past 8. */
export function seriesColor(i: number, theme: Theme = 'dark'): string {
  const slots = CATEGORICAL[theme];
  return slots[Math.min(Math.max(0, i), slots.length - 1)];
}

/** Current theme from the document (data-theme attribute). */
export function currentTheme(): Theme {
  if (typeof document === 'undefined') return 'dark';
  return document.documentElement.dataset.theme === 'light' ? 'light' : 'dark';
}
