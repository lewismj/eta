import { describe, expect, it } from 'vitest';
import {
  boxX, boxZ, catmullRom, displayResolution, nearestCell, parseSurface, resample1d, resampleGrid, validateSurface,
  zRange,
} from './surfaceModel';

describe('parseSurface / validateSurface', () => {
  const p = {
    xs: [0, 1, 2],
    ys: [0.5, 0.9],
    zs: new Float64Array([0, 1, 2, 3, 4, 5]),
    x: { t: 'axis', p: { label: 'Years', format: 'num' } },
    z: { t: 'axis', p: { label: 'Exposure' } },
  };
  it('reads the grid and axis labels', () => {
    const s = parseSurface(p);
    expect([s.nx, s.ny]).toEqual([3, 2]);
    expect(s.x.label).toBe('Years');
    expect(s.z.label).toBe('Exposure');
    expect(s.y.label).toBeUndefined();
    expect(validateSurface(s)).toBeNull();
  });
  it('rejects a mismatched or degenerate grid', () => {
    expect(validateSurface(parseSurface({ ...p, zs: [1, 2] }))).toMatch(/2 × 3 = 6 values, got 2/);
    expect(validateSurface(parseSurface({ ...p, ys: [1], zs: [1, 2, 3] }))).toMatch(/2 × 2/);
  });
});

describe('zRange', () => {
  it('ignores non-finite values and widens flat data', () => {
    expect(zRange([3, NaN, 1, 2])).toEqual([1, 3]);
    expect(zRange([2, 2])).toEqual([1.5, 2.5]);
    expect(zRange([])).toEqual([0, 1]);
  });
});

describe('Catmull-Rom resampling', () => {
  it('interpolates through its control points', () => {
    expect(catmullRom(0, 1, 2, 3, 0)).toBe(1);
    expect(catmullRom(0, 1, 2, 3, 1)).toBe(2);
    expect(catmullRom(0, 1, 2, 3, 0.5)).toBeCloseTo(1.5);
  });
  it('resample1d keeps the end points and the original samples', () => {
    const v = [0, 4, 1, 3];
    const r = resample1d(v, 10); // 3 intervals -> every 3rd sample is original
    expect(r[0]).toBe(0);
    expect(r[9]).toBe(3);
    expect(r[3]).toBeCloseTo(4);
    expect(r[6]).toBeCloseTo(1);
  });
  it('reproduces linear data exactly', () => {
    const r = resample1d([0, 1, 2, 3], 13);
    r.forEach((x, k) => expect(x).toBeCloseTo((k / 12) * 3, 10));
  });
  it('handles tiny inputs', () => {
    expect(Array.from(resample1d([5], 3))).toEqual([5, 5, 5]);
    expect(resample1d([], 3).length).toBe(3);
  });
  it('resampleGrid preserves corners and is separable', () => {
    const z = [0, 1, 2, 10, 11, 12]; // ny=2 rows of nx=3
    const g = resampleGrid(z, 3, 2, 5, 3);
    expect(g.length).toBe(15);
    expect(g[0]).toBe(0);
    expect(g[4]).toBeCloseTo(2);
    expect(g[10]).toBeCloseTo(10);
    expect(g[14]).toBeCloseTo(12);
    expect(g[5]).toBeCloseTo(5); // middle row, first column: halfway 0 -> 10
  });
  it('display resolution upsamples small grids, capped', () => {
    expect(displayResolution(10)).toBe(37);
    expect(displayResolution(30)).toBe(64);
    expect(displayResolution(200)).toBe(200);
  });
});

describe('box mapping and picking', () => {
  it('maps indices into [-1, 1] and heights into [0, height]', () => {
    expect(boxX(0, 5)).toBe(-1);
    expect(boxX(4, 5)).toBe(1);
    expect(boxX(2, 5)).toBe(0);
    const b = { height: 0.9, zMin: 10, zMax: 20 };
    expect(boxZ(10, b)).toBe(0);
    expect(boxZ(20, b)).toBeCloseTo(0.9);
  });
  it('nearestCell inverts the box mapping and clamps', () => {
    expect(nearestCell(boxX(3, 10), boxX(7, 30), 10, 30)).toEqual({ i: 3, j: 7 });
    expect(nearestCell(-5, 5, 10, 30)).toEqual({ i: 0, j: 29 });
  });
});
