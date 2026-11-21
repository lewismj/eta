import { describe, expect, it } from 'vitest';
import {
  cellColor, cellInk, heatmapScale, legendGradient, legendTicks, parseHeatmap, validateHeatmap, valueAt,
} from './heatmapModel';

const p = {
  rows: ['CP-01', 'CP-02'],
  cols: ['oil', 'usd-10y', 'eur-usd'],
  values: [0.5, -0.2, 0, 2.9, 0.1, -1.5],
  format: 'num',
};

describe('parseHeatmap', () => {
  it('reads labels, values and defaults', () => {
    const h = parseHeatmap(p);
    expect(h.rows).toEqual(['CP-01', 'CP-02']);
    expect(h.scale).toBe('diverging');
    expect(h.selectedRow).toBeNull();
    expect(h.showValues).toBe(false);
    expect(valueAt(h, 1, 0)).toBe(2.9);
    expect(valueAt(h, 0, 2)).toBe(0);
  });
  it('selected row accepts an index or a label', () => {
    expect(parseHeatmap({ ...p, 'selected-row': 1 }).selectedRow).toBe(1);
    expect(parseHeatmap({ ...p, 'selected-row': 'CP-02' }).selectedRow).toBe(1);
    expect(parseHeatmap({ ...p, 'selected-row': 'missing' }).selectedRow).toBe(-1);
  });
  it('accepts Float64Array values and numeric labels', () => {
    const h = parseHeatmap({ ...p, values: new Float64Array(p.values), cols: [1, 2, 3] });
    expect(h.cols).toEqual(['1', '2', '3']);
    expect(h.values.length).toBe(6);
  });
});

describe('validateHeatmap', () => {
  it('accepts a consistent shape', () => {
    expect(validateHeatmap(parseHeatmap(p))).toBeNull();
  });
  it('reports a size mismatch with the expected count', () => {
    expect(validateHeatmap(parseHeatmap({ ...p, values: [1, 2] }))).toMatch(/2 × 3 = 6 values, got 2/);
  });
  it('requires labels', () => {
    expect(validateHeatmap(parseHeatmap({ ...p, rows: [] }))).toMatch(/labels/);
  });
});

describe('heatmapScale', () => {
  it('diverging is symmetric around zero over the data', () => {
    const s = heatmapScale(parseHeatmap(p), 'dark');
    expect(s.kind).toBe('diverging');
    expect(s.domain).toEqual([-2.9, 0, 2.9]);
  });
  it('explicit bounds override the data extent', () => {
    const s = heatmapScale(parseHeatmap({ ...p, max: 5 }), 'dark');
    expect(s.domain).toEqual([-5, 0, 5]);
  });
  it('sequential spans the data', () => {
    const s = heatmapScale(parseHeatmap({ ...p, scale: 'sequential' }), 'dark');
    expect(s.domain).toEqual([-1.5, 2.9]);
  });
  it('zero maps to the neutral midpoint', () => {
    const s = heatmapScale(parseHeatmap(p), 'dark');
    expect(cellColor(s, 0)).toBe('#383835');
  });
});

describe('legend and ink', () => {
  it('diverging legend shows min, mid and max', () => {
    const s = heatmapScale(parseHeatmap(p), 'dark');
    expect(legendTicks(s, 'num').map((t) => t.at)).toEqual([0, 0.5, 1]);
  });
  it('sequential legend shows the two ends', () => {
    const s = heatmapScale(parseHeatmap({ ...p, scale: 'sequential' }), 'dark');
    expect(legendTicks(s, 'num')).toHaveLength(2);
  });
  it('gradient lists every stop with a position', () => {
    const g = legendGradient(heatmapScale(parseHeatmap(p), 'dark'), 5);
    expect(g.startsWith('linear-gradient(90deg,')).toBe(true);
    expect(g).toContain('0%');
    expect(g).toContain('100%');
  });
  it('ink contrasts with the fill', () => {
    expect(cellInk('#ffffff')).toBe('#0b0b0b');
    expect(cellInk('#1a1a19')).toBe('#ffffff');
  });
});
