import { describe, expect, it } from 'vitest';
import type { UiNode } from '../model';
import { parseMarks } from './marks';
import {
  barAt, barTooltip, crosshairAt, crosshairLines, leftMargin, plotDomain, pointTooltip, tickCountX, xTicks, yTicks,
} from './plotModel';

const node = (t: string, id: string, p: Record<string, unknown>): UiNode => ({ t, id, p, c: [] });
const size = { w: 660, h: 300 };

describe('plotDomain', () => {
  it('continuous plots get padded, nice domains around the data', () => {
    const marks = parseMarks([node('line', 'l', { xs: [0, 1, 2], ys: [0.031, 0.05, 0.072] })]);
    const d = plotDomain(marks, {}, {}, size);
    expect(d.x0).toBeLessThanOrEqual(0);
    expect(d.x1).toBeGreaterThanOrEqual(2);
    expect(d.y0).toBeLessThan(0.031);
    expect(d.y1).toBeGreaterThan(0.072);
  });
  it('bars put categories top to bottom and include zero', () => {
    const marks = parseMarks([node('bars', 'b', { categories: ['A', 'B', 'C'], values: [5, 7, 2] })]);
    const d = plotDomain(marks, {}, {}, size);
    expect(d.y0).toBe(2.5);
    expect(d.y1).toBe(-0.5);
    expect(d.x0).toBe(0);
    expect(d.x1).toBeGreaterThanOrEqual(7);
  });
  it('respects zero and explicit bounds on continuous axes', () => {
    const marks = parseMarks([node('line', 'l', { xs: [1, 2], ys: [5, 6] })]);
    const d = plotDomain(marks, { min: 0, max: 10 }, { zero: true }, size);
    expect(d.x0).toBe(0);
    expect(d.x1).toBe(10);
    expect(d.y0).toBe(0);
  });
});

describe('margins and ticks', () => {
  it('left margin grows with category label length, within limits', () => {
    const short = parseMarks([node('bars', 'b', { categories: ['A'], values: [1] })]);
    const long = parseMarks([node('bars', 'b', { categories: ['A very long counterparty name'], values: [1] })]);
    expect(leftMargin(short, false)).toBe(64);
    expect(leftMargin(long, false)).toBeGreaterThan(150);
    expect(leftMargin(parseMarks([node('bars', 'b', { categories: ['x'.repeat(200)], values: [1] })]), false)).toBe(180);
  });
  it('y title widens the margin of continuous plots', () => {
    expect(leftMargin([], true)).toBeGreaterThan(leftMargin([], false));
  });
  it('category ticks label every row', () => {
    expect(yTicks({ x0: 0, x1: 1, y0: 1.5, y1: -0.5 }, size, 'num', ['A', 'B'])).toEqual([
      { value: 0, label: 'A' },
      { value: 1, label: 'B' },
    ]);
  });
  it('x ticks are formatted with the axis format', () => {
    const t = xTicks({ x0: 0, x1: 0.2, y0: 0, y1: 1 }, size, 'pct');
    expect(t.length).toBeGreaterThanOrEqual(3);
    expect(t[t.length - 1].label.endsWith('%')).toBe(true);
  });
  it('tick density follows width', () => {
    expect(tickCountX(1200)).toBeGreaterThan(tickCountX(400));
    expect(tickCountX(10)).toBe(3);
  });
});

describe('crosshair', () => {
  const marks = parseMarks([
    node('line', 'a', { xs: [0, 1, 2, 3], ys: [10, 20, 30, 40], label: 'EPE' }),
    node('line', 'b', { xs: [0, 1, 2, 3], ys: [-1, -2, -3, -4], label: 'ENE' }),
    node('area', 'c', { xs: [0, 1, 2, 3], y1s: [1, 1, 1, 1] }),
  ]);
  it('snaps to the nearest x and reports every line', () => {
    const c = crosshairAt(marks, 1.6, { label: 'Year', format: 'num' }, { format: 'num' })!;
    expect(c.x).toBe(2);
    expect(c.content.rows.map((r) => [r.label, r.value])).toEqual([['EPE', '30.00'], ['ENE', '−3.00']]);
    expect(c.content.title.startsWith('Year')).toBe(true);
    expect(c.points.map((p) => p.slot)).toEqual([0, 1]);
  });
  it('ignores lines with unsorted x and returns null without lines', () => {
    const unsorted = parseMarks([node('line', 'u', { xs: [2, 1], ys: [0, 0] })]);
    expect(crosshairLines(unsorted)).toHaveLength(0);
    expect(crosshairAt(unsorted, 1, {}, {})).toBeNull();
  });
  it('skips series with no value at that x', () => {
    const gap = parseMarks([
      node('line', 'a', { xs: [0, 1], ys: [1, 2], label: 'A' }),
      node('line', 'b', { xs: [0, 1], ys: [NaN, 5], label: 'B' }),
    ]);
    expect(crosshairAt(gap, 0, {}, {})!.content.rows.map((r) => r.label)).toEqual(['A']);
  });
});

describe('bars and points tooltips', () => {
  const bars = parseMarks([node('bars', 'b', { categories: ['CP-01', 'CP-02'], values: [1200, 900], label: 'CVA' })]);
  it('barAt maps a row position to its category', () => {
    expect(barAt(bars, 0.2)?.index).toBe(0);
    expect(barAt(bars, 1.4)?.index).toBe(1);
    expect(barAt(bars, 2.6)).toBeNull();
    expect(barAt(bars, -0.7)).toBeNull();
  });
  it('bar tooltip names the category and the value', () => {
    const b = bars[0];
    if (b.kind !== 'bars') throw new Error();
    const t = barTooltip(b, 1, { label: 'CVA', format: 'int' });
    expect(t.title).toBe('CP-02');
    expect(t.rows).toEqual([{ label: 'CVA', value: '900' }]);
  });
  it('point tooltip prefers labels over keys', () => {
    const [p] = parseMarks([node('points', 'p', { xs: [0.1], ys: [0.2], keys: [7], labels: ['Portfolio #7'] })]);
    if (p.kind !== 'points') throw new Error();
    expect(pointTooltip(p, 0, '7', { format: 'pct' }, {}).title).toBe('Portfolio #7');
    const [q] = parseMarks([node('points', 'q', { xs: [0.1], ys: [0.2], keys: [7] })]);
    if (q.kind !== 'points') throw new Error();
    expect(pointTooltip(q, 0, '7', {}, {}).title).toBe('7');
  });
});
