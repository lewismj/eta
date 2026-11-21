/**
 * Heatmap model (pure): shape validation, colour scale choice, legend and
 * cell lookup. Rendering is in Heatmap.tsx.
 */

import { formatNumber } from '../format';
import { ColorScale, Theme, divergingScale, rgbToHex, sequentialScale, symmetricExtent } from './color';
import type { NumArray } from './marks';
import { toNumbers } from './marks';

export interface HeatmapSpec {
  rows: string[];
  cols: string[];
  /** Row-major values, rows.length * cols.length. */
  values: NumArray;
  scale: 'diverging' | 'sequential';
  /** Optional explicit domain: [min, max] (diverging uses [-m, m] if absent). */
  min?: number;
  max?: number;
  format: string;
  selectedRow: number | null;
  showValues: boolean;
}

const labels = (v: unknown): string[] =>
  Array.isArray(v) ? v.map((x) => (typeof x === 'string' ? x : String(x))) : v instanceof Float64Array ? Array.from(v, String) : [];

export function parseHeatmap(p: Record<string, unknown>): HeatmapSpec {
  const rows = labels(p.rows);
  const cols = labels(p.cols);
  const sel = p['selected-row'];
  return {
    rows,
    cols,
    values: toNumbers(p.values),
    scale: p.scale === 'sequential' ? 'sequential' : 'diverging',
    min: typeof p.min === 'number' ? p.min : undefined,
    max: typeof p.max === 'number' ? p.max : undefined,
    format: typeof p.format === 'string' ? p.format : 'num',
    selectedRow: typeof sel === 'number' ? sel : typeof sel === 'string' ? Math.max(-1, rows.indexOf(sel)) : null,
    showValues: p['show-values'] === true,
  };
}

/** Problems that make the heatmap unrenderable (shown instead of the grid). */
export function validateHeatmap(h: HeatmapSpec): string | null {
  if (h.rows.length === 0 || h.cols.length === 0) return 'heatmap needs row and column labels';
  if (h.values.length !== h.rows.length * h.cols.length) {
    return `heatmap expects ${h.rows.length} × ${h.cols.length} = ${h.rows.length * h.cols.length} values, got ${h.values.length}`;
  }
  return null;
}

export function heatmapScale(h: HeatmapSpec, theme: Theme): ColorScale {
  if (h.scale === 'sequential') {
    let lo = Infinity, hi = -Infinity;
    for (let i = 0; i < h.values.length; i++) {
      const v = h.values[i];
      if (!Number.isFinite(v)) continue;
      lo = Math.min(lo, v);
      hi = Math.max(hi, v);
    }
    if (!Number.isFinite(lo)) { lo = 0; hi = 1; }
    return sequentialScale(h.min ?? lo, h.max ?? hi, theme);
  }
  const m = h.max !== undefined || h.min !== undefined
    ? Math.max(Math.abs(h.min ?? 0), Math.abs(h.max ?? 0))
    : symmetricExtent(h.values);
  return divergingScale(-m, 0, m, theme);
}

export const valueAt = (h: HeatmapSpec, row: number, col: number): number => h.values[row * h.cols.length + col];

export function cellColor(scale: ColorScale, v: number): string {
  return rgbToHex(scale.color(v));
}

/** Ink for a value printed on a cell: light text on dark fills and vice versa. */
export function cellInk(hex: string): string {
  const n = parseInt(hex.slice(1), 16);
  const r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
  const lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
  return lum > 0.55 ? '#0b0b0b' : '#ffffff';
}

export interface LegendTick {
  /** Position 0..1 along the legend bar. */
  at: number;
  label: string;
}

export function legendTicks(scale: ColorScale, format: string): LegendTick[] {
  const d = scale.domain;
  const fmt = (v: number) => formatNumber(v, format);
  if (scale.kind === 'diverging') {
    return [
      { at: 0, label: fmt(d[0]) },
      { at: 0.5, label: fmt(d[1]) },
      { at: 1, label: fmt(d[2]) },
    ];
  }
  return [
    { at: 0, label: fmt(d[0]) },
    { at: 1, label: fmt(d[1]) },
  ];
}

/** CSS linear-gradient for the legend bar. */
export function legendGradient(scale: ColorScale, stops = 9): string {
  const colors = scale.legend(stops);
  return `linear-gradient(90deg, ${colors.map((c, i) => `${c} ${Math.round((i / (stops - 1)) * 100)}%`).join(', ')})`;
}
