/**
 * Plot-level logic that does not need the GPU or the DOM: domains (including
 * the category axis used by bars), margins, ticks and tooltip contents.
 */

import { formatNumber } from '../format';
import { isSorted, nearestIndex } from './geometry';
import type { BarsMark, LineMark, Mark, PointsMark } from './marks';
import { bandCategories, unionExtent } from './marks';
import { AxisSpec, Domain, axisDomain, ticks } from './scale';

export interface Size {
  w: number;
  h: number;
}

export const ROW_HEIGHT = 24;

export function tickCountX(w: number): number {
  return Math.max(3, Math.round(w / 110));
}

export function tickCountY(h: number): number {
  return Math.max(3, Math.round(h / 60));
}

/** Target domain for the data. Bars put categories on y, top to bottom. */
export function plotDomain(marks: Mark[], xs: AxisSpec, ys: AxisSpec, size: Size): Domain {
  const e = unionExtent(marks);
  const cats = bandCategories(marks);
  if (cats) {
    const [x0, x1] = axisDomain(Math.min(0, e.x0), Math.max(0, e.x1), { ...xs, zero: true }, tickCountX(size.w));
    const n = Math.max(1, cats.length);
    return { x0, x1, y0: n - 0.5, y1: -0.5 };
  }
  const [x0, x1] = axisDomain(e.x0, e.x1, xs, tickCountX(size.w));
  const [y0, y1] = axisDomain(e.y0, e.y1, ys, tickCountY(size.h));
  return { x0, x1, y0, y1 };
}

/** Left margin wide enough for the longest category label (bars) or ticks. */
export function leftMargin(marks: Mark[], hasYTitle: boolean): number {
  const cats = bandCategories(marks);
  if (cats) {
    const longest = cats.reduce((m, c) => Math.max(m, c.length), 0);
    return Math.min(180, Math.max(64, Math.round(longest * 6.8) + 18));
  }
  return hasYTitle ? 68 : 52;
}

export interface Tick {
  value: number;
  label: string;
}

export function xTicks(target: Domain, size: Size, format: string): Tick[] {
  const t = ticks(Math.min(target.x0, target.x1), Math.max(target.x0, target.x1), tickCountX(size.w));
  return t.values.map((v) => ({ value: v, label: formatNumber(v, format, t.step) }));
}

export function yTicks(target: Domain, size: Size, format: string, categories: string[] | null): Tick[] {
  if (categories) return categories.map((c, i) => ({ value: i, label: c }));
  const t = ticks(Math.min(target.y0, target.y1), Math.max(target.y0, target.y1), tickCountY(size.h));
  return t.values.map((v) => ({ value: v, label: formatNumber(v, format, t.step) }));
}

export interface TooltipRow {
  label: string;
  value: string;
  slot?: number;
}

export interface TooltipContent {
  title: string;
  rows: TooltipRow[];
  slot?: number;
}

/** Marks that drive a crosshair: lines with a sorted x grid. */
export function crosshairLines(marks: Mark[]): LineMark[] {
  return marks.filter((m): m is LineMark => m.kind === 'line' && m.xs.length > 0 && isSorted(m.xs));
}

/**
 * Crosshair readout at data x: snaps to the nearest x of the first line and
 * reports every line's value there.
 */
export function crosshairAt(
  marks: Mark[], x: number, xs: AxisSpec, ys: AxisSpec,
): { x: number; index: number; content: TooltipContent; points: { y: number; slot: number }[] } | null {
  const lines = crosshairLines(marks);
  if (lines.length === 0) return null;
  const idx = nearestIndex(lines[0].xs, x);
  if (idx < 0) return null;
  const xv = lines[0].xs[idx];
  const rows: TooltipRow[] = [];
  const points: { y: number; slot: number }[] = [];
  for (const l of lines) {
    const j = l.xs === lines[0].xs ? idx : nearestIndex(l.xs, xv);
    const y = l.ys[j];
    if (!Number.isFinite(y)) continue;
    rows.push({ label: l.label || 'value', value: formatNumber(y, ys.format ?? 'num'), slot: l.slot });
    points.push({ y, slot: l.slot });
  }
  return {
    x: xv,
    index: idx,
    content: { title: `${xs.label ? xs.label + ' ' : ''}${formatNumber(xv, xs.format ?? 'num')}`, rows },
    points,
  };
}

/** Bar under data-space y (category index), or null. */
export function barAt(marks: Mark[], y: number): { mark: BarsMark; index: number } | null {
  const bars = marks.find((m): m is BarsMark => m.kind === 'bars');
  if (!bars) return null;
  const i = Math.round(y);
  if (i < 0 || i >= bars.categories.length || Math.abs(y - i) > 0.5) return null;
  return { mark: bars, index: i };
}

export function barTooltip(mark: BarsMark, index: number, xs: AxisSpec): TooltipContent {
  return {
    title: mark.categories[index],
    slot: mark.slot,
    rows: [{ label: xs.label ?? mark.label ?? 'value', value: formatNumber(mark.values[index], xs.format ?? 'num') }],
  };
}

export function pointTooltip(mark: PointsMark, dataIndex: number, key: string, xs: AxisSpec, ys: AxisSpec): TooltipContent {
  const labels = mark.labels;
  const title = labels && labels[dataIndex] != null ? String(labels[dataIndex]) : key;
  return {
    title,
    slot: mark.slot,
    rows: [
      { label: xs.label ?? 'x', value: formatNumber(mark.xs[dataIndex], xs.format ?? 'num') },
      { label: ys.label ?? 'y', value: formatNumber(mark.ys[dataIndex], ys.format ?? 'num') },
    ],
  };
}
