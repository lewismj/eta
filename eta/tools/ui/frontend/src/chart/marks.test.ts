import { describe, expect, it } from 'vitest';
import type { UiNode } from '../model';
import { bandCategories, legendEntries, markExtent, parseMarks, toNumbers, unionExtent } from './marks';

const node = (t: string, id: string, p: Record<string, unknown>): UiNode => ({ t, id, p, c: [] });

describe('toNumbers', () => {
  it('keeps Float64Array and converts lists, mapping non-numbers to NaN', () => {
    const f = new Float64Array([1, 2]);
    expect(toNumbers(f)).toBe(f);
    expect(Array.from(toNumbers([1, 'x', 3]))).toEqual([1, NaN, 3]);
    expect(toNumbers(undefined).length).toBe(0);
  });
});

describe('parseMarks', () => {
  it('parses every mark kind and skips other children', () => {
    const marks = parseMarks([
      node('line', 'l', { xs: [0, 1], ys: [1, 2], label: 'EPE' }),
      node('area', 'a', { xs: [0, 1], y0s: [0, 0], y1s: [1, 2], fill: 'band', series: 'EPE' }),
      node('points', 'p', { xs: [1], ys: [1], keys: ['k'], label: 'P' }),
      node('bars', 'b', { categories: ['A', 'B'], values: [3, 4], selected: 'B' }),
      node('axis', 'x', { label: 'ignored' }),
    ]);
    expect(marks.map((m) => m.kind)).toEqual(['line', 'area', 'points', 'bars']);
  });

  it('assigns colour slots in order and shares a slot between a line and its band', () => {
    const [l1, band, l2] = parseMarks([
      node('line', 'l1', { xs: [0], ys: [0], label: 'EPE' }),
      node('area', 'a', { xs: [0], y1s: [0], series: 'EPE' }),
      node('line', 'l2', { xs: [0], ys: [0], label: 'ENE' }),
    ]);
    expect(l1.slot).toBe(0);
    expect(band.slot).toBe(0);
    expect(l2.slot).toBe(1);
  });

  it('honours an explicit slot and caps slots at 8 colours', () => {
    const children = Array.from({ length: 10 }, (_, i) => node('line', `l${i}`, { xs: [0], ys: [0], label: `s${i}` }));
    const marks = parseMarks(children);
    expect(marks[9].slot).toBe(7);
    expect(parseMarks([node('line', 'x', { xs: [], ys: [], slot: 3 })])[0].slot).toBe(3);
  });

  it('area defaults: gradient fill, baseline 0, opacity 0.32, null lower edge', () => {
    const [a] = parseMarks([node('area', 'a', { xs: [0, 1], y1s: [1, 1] })]);
    expect(a.kind).toBe('area');
    if (a.kind !== 'area') return;
    expect(a.fill).toBe('gradient');
    expect(a.baseline).toBe(0);
    expect(a.opacity).toBeCloseTo(0.32);
    expect(a.y0s).toBeNull();
  });

  it('bars normalise categories and selection to strings', () => {
    const [b] = parseMarks([node('bars', 'b', { categories: [1, 2], values: [5, 6], selected: 2 })]);
    if (b.kind !== 'bars') throw new Error('expected bars');
    expect(b.categories).toEqual(['1', '2']);
    expect(b.selected).toBe('2');
  });

  it('picks up on-select handlers', () => {
    const [p] = parseMarks([node('points', 'p', { xs: [], ys: [], keys: [], 'on-select': { $h: 'r#on-select' } })]);
    expect(p.onSelect).toEqual({ $h: 'r#on-select' });
  });
});

describe('extents', () => {
  it('line/points cover their finite data', () => {
    const [l] = parseMarks([node('line', 'l', { xs: [0, 1, 2], ys: [5, NaN, -1] })]);
    expect(markExtent(l)).toEqual({ x0: 0, x1: 2, y0: -1, y1: 5 });
  });
  it('areas include the baseline when there is no lower edge', () => {
    const [a] = parseMarks([node('area', 'a', { xs: [0, 1], y1s: [2, 3], baseline: 0.5 })]);
    expect(markExtent(a)).toEqual({ x0: 0, x1: 1, y0: 0.5, y1: 3 });
  });
  it('bars span category rows and include zero on the value axis', () => {
    const [b] = parseMarks([node('bars', 'b', { categories: ['a', 'b', 'c'], values: [2, 5, 1] })]);
    expect(markExtent(b)).toEqual({ x0: 0, x1: 5, y0: -0.5, y1: 2.5 });
  });
  it('union covers all marks', () => {
    const marks = parseMarks([
      node('line', 'l', { xs: [0, 1], ys: [0, 1] }),
      node('line', 'm', { xs: [2, 3], ys: [-4, 1] }),
    ]);
    expect(unionExtent(marks)).toEqual({ x0: 0, x1: 3, y0: -4, y1: 1 });
  });
});

describe('legend and band axis', () => {
  it('legend lists labelled non-area series once, in order', () => {
    const marks = parseMarks([
      node('line', 'a', { xs: [], ys: [], label: 'EPE' }),
      node('area', 'b', { xs: [], y1s: [], label: 'band', series: 'EPE' }),
      node('line', 'c', { xs: [], ys: [], label: 'ENE' }),
      node('line', 'd', { xs: [], ys: [] }),
    ]);
    expect(legendEntries(marks)).toEqual([{ label: 'EPE', slot: 0 }, { label: 'ENE', slot: 1 }]);
  });
  it('bands come from bars only', () => {
    expect(bandCategories(parseMarks([node('line', 'l', { xs: [], ys: [] })]))).toBeNull();
    expect(bandCategories(parseMarks([node('bars', 'b', { categories: ['x'], values: [1] })]))).toEqual(['x']);
  });
});
