/**
 * Mark specifications: the typed, validated form of chart child nodes.
 * Pure (no GPU), so parsing, extents and series matching are unit-tested.
 */

import type { UiNode } from '../model';
import { isHandler, HandlerRef } from '../model';

export type NumArray = ArrayLike<number>;

interface MarkBase {
  id: string;
  /** Categorical colour slot (0-based), assigned in order of appearance. */
  slot: number;
  label: string;
  onSelect?: HandlerRef;
}

export interface PointsMark extends MarkBase {
  kind: 'points';
  xs: NumArray;
  ys: NumArray;
  keys: ArrayLike<unknown>;
  labels?: ArrayLike<unknown>;
  selected?: unknown;
  size?: number;
}

export interface LineMark extends MarkBase {
  kind: 'line';
  xs: NumArray;
  ys: NumArray;
  width: number;
  emphasis: boolean;
}

export interface AreaMark extends MarkBase {
  kind: 'area';
  xs: NumArray;
  /** Lower edge (baseline when absent). */
  y0s: NumArray | null;
  y1s: NumArray;
  baseline: number;
  /** 'gradient' fades toward the lower edge; 'band' is flat. */
  fill: 'gradient' | 'band';
  opacity: number;
}

export interface BarsMark extends MarkBase {
  kind: 'bars';
  categories: string[];
  values: NumArray;
  selected?: string | null;
}

export type Mark = PointsMark | LineMark | AreaMark | BarsMark;

export const MARK_TYPES = new Set(['points', 'line', 'area', 'bars']);

export function toNumbers(v: unknown): NumArray {
  if (v instanceof Float64Array) return v;
  if (Array.isArray(v)) return Float64Array.from(v, (x) => (typeof x === 'number' ? x : NaN));
  return new Float64Array(0);
}

function toList(v: unknown): unknown[] {
  if (v instanceof Float64Array) return Array.from(v);
  if (Array.isArray(v)) return v.slice();
  return [];
}

const str = (v: unknown, d = '') => (typeof v === 'string' ? v : typeof v === 'number' ? String(v) : d);
const num = (v: unknown, d: number) => (typeof v === 'number' && Number.isFinite(v) ? v : d);

/**
 * Convert child nodes of a plot into marks. Colour slots are assigned in
 * order to series-like marks (line, area, bars, points); an area that
 * declares 'series' shares the slot of the line with that label.
 */
export function parseMarks(children: UiNode[]): Mark[] {
  const marks: Mark[] = [];
  const slotByLabel = new Map<string, number>();
  let nextSlot = 0;
  const slotFor = (label: string, explicit: unknown): number => {
    if (typeof explicit === 'number') return Math.max(0, Math.min(7, Math.floor(explicit)));
    if (label && slotByLabel.has(label)) return slotByLabel.get(label)!;
    const s = Math.min(7, nextSlot++);
    if (label) slotByLabel.set(label, s);
    return s;
  };

  for (const c of children) {
    if (!MARK_TYPES.has(c.t)) continue;
    const p = c.p as Record<string, unknown>;
    const label = str(p.label);
    const onSelect = isHandler(p['on-select']) ? (p['on-select'] as HandlerRef) : undefined;
    switch (c.t) {
      case 'points':
        marks.push({
          kind: 'points', id: c.id, label, onSelect, slot: slotFor(label, p.slot),
          xs: toNumbers(p.xs), ys: toNumbers(p.ys), keys: toList(p.keys),
          labels: Array.isArray(p.labels) ? p.labels : undefined,
          selected: p.selected, size: typeof p.size === 'number' ? p.size : undefined,
        });
        break;
      case 'line':
        marks.push({
          kind: 'line', id: c.id, label, onSelect, slot: slotFor(label, p.slot),
          xs: toNumbers(p.xs), ys: toNumbers(p.ys),
          width: Math.max(1, num(p.width, 2)), emphasis: p.emphasis === true,
        });
        break;
      case 'area': {
        const series = str(p.series);
        marks.push({
          kind: 'area', id: c.id, label, onSelect,
          slot: series && slotByLabel.has(series) ? slotByLabel.get(series)! : slotFor(series || label, p.slot),
          xs: toNumbers(p.xs), y0s: p.y0s === undefined || p.y0s === false ? null : toNumbers(p.y0s),
          y1s: toNumbers(p.y1s), baseline: num(p.baseline, 0),
          fill: p.fill === 'band' ? 'band' : 'gradient', opacity: Math.min(1, Math.max(0, num(p.opacity, 0.32))),
        });
        break;
      }
      case 'bars':
        marks.push({
          kind: 'bars', id: c.id, label, onSelect, slot: slotFor(label, p.slot),
          categories: toList(p.categories).map((x) => str(x)),
          values: toNumbers(p.values),
          selected: p.selected === undefined || p.selected === false || p.selected === null ? null : str(p.selected),
        });
        break;
    }
  }
  return marks;
}

export interface Extent {
  x0: number;
  x1: number;
  y0: number;
  y1: number;
}

export const EMPTY_EXTENT: Extent = { x0: Infinity, x1: -Infinity, y0: Infinity, y1: -Infinity };

function widen(e: Extent, x: number, y: number): void {
  if (Number.isFinite(x)) {
    if (x < e.x0) e.x0 = x;
    if (x > e.x1) e.x1 = x;
  }
  if (Number.isFinite(y)) {
    if (y < e.y0) e.y0 = y;
    if (y > e.y1) e.y1 = y;
  }
}

/** Data extent of one mark. Bars report category indices on y. */
export function markExtent(m: Mark): Extent {
  const e = { ...EMPTY_EXTENT };
  switch (m.kind) {
    case 'points':
    case 'line':
      for (let i = 0; i < Math.min(m.xs.length, m.ys.length); i++) widen(e, m.xs[i], m.ys[i]);
      break;
    case 'area':
      for (let i = 0; i < Math.min(m.xs.length, m.y1s.length); i++) {
        widen(e, m.xs[i], m.y1s[i]);
        widen(e, m.xs[i], m.y0s ? m.y0s[i] : m.baseline);
      }
      break;
    case 'bars':
      widen(e, 0, -0.5);
      widen(e, 0, m.categories.length - 0.5);
      for (let i = 0; i < m.values.length; i++) widen(e, m.values[i], NaN);
      break;
  }
  return e;
}

export function unionExtent(marks: Mark[]): Extent {
  const e = { ...EMPTY_EXTENT };
  for (const m of marks) {
    const me = markExtent(m);
    widen(e, me.x0, me.y0);
    widen(e, me.x1, me.y1);
  }
  return e;
}

/** A plot is categorical (band y axis) when it contains bars. */
export function bandCategories(marks: Mark[]): string[] | null {
  const bars = marks.find((m): m is BarsMark => m.kind === 'bars');
  return bars ? bars.categories : null;
}

/** Series that should appear in the legend (labelled lines/points/bars). */
export function legendEntries(marks: Mark[]): { label: string; slot: number }[] {
  const seen = new Set<string>();
  const out: { label: string; slot: number }[] = [];
  for (const m of marks) {
    if (m.kind === 'area' || !m.label || seen.has(m.label)) continue;
    seen.add(m.label);
    out.push({ label: m.label, slot: m.slot });
  }
  return out;
}
