import { describe, expect, it } from 'vitest';
import { MIN_ZOOM_FRACTION, isFullX, panX, wheelFactor, zoomX } from './interact';

const full = { x0: 0, x1: 100, y0: 0, y1: 1 };

describe('zoomX', () => {
  it('keeps the data under the cursor fixed', () => {
    const anchor = 0.3;
    const before = full.x0 + (full.x1 - full.x0) * anchor;
    const z = zoomX(full, anchor, 0.5, full);
    const after = z.x0 + (z.x1 - z.x0) * anchor;
    expect(after).toBeCloseTo(before, 9);
    expect(z.x1 - z.x0).toBeCloseTo(50, 9);
  });
  it('never zooms out beyond the full domain', () => {
    const z = zoomX({ ...full, x0: 40, x1: 60 }, 0.5, 100, full);
    expect(z).toMatchObject({ x0: 0, x1: 100 });
    expect(isFullX(z, full)).toBe(true);
  });
  it('never zooms in past the minimum window', () => {
    let d = full;
    for (let i = 0; i < 100; i++) d = zoomX(d, 0.5, 0.5, full);
    expect(d.x1 - d.x0).toBeCloseTo(100 * MIN_ZOOM_FRACTION, 9);
  });
  it('stays inside the full domain near the edges', () => {
    const z = zoomX({ ...full, x0: 0, x1: 20 }, 0, 1.5, full);
    expect(z.x0).toBeGreaterThanOrEqual(0);
    expect(z.x1).toBeLessThanOrEqual(100);
  });
  it('leaves y untouched', () => {
    expect(zoomX(full, 0.5, 0.5, full)).toMatchObject({ y0: 0, y1: 1 });
  });
});

describe('panX', () => {
  it('moves the window opposite to the drag', () => {
    const p = panX({ ...full, x0: 40, x1: 60 }, 0.5, full);
    expect(p).toMatchObject({ x0: 30, x1: 50 });
  });
  it('clamps to the full domain and keeps the width', () => {
    const p = panX({ ...full, x0: 10, x1: 30 }, 5, full);
    expect(p).toMatchObject({ x0: 0, x1: 20 });
    const q = panX({ ...full, x0: 70, x1: 90 }, -5, full);
    expect(q).toMatchObject({ x0: 80, x1: 100 });
  });
});

describe('wheelFactor', () => {
  it('scrolling down zooms out, up zooms in, symmetric in log space', () => {
    expect(wheelFactor(100)).toBeCloseTo(1.15);
    expect(wheelFactor(-100)).toBeCloseTo(1 / 1.15);
    expect(wheelFactor(0)).toBe(1);
  });
  it('limits very large deltas', () => {
    expect(wheelFactor(100000)).toBeCloseTo(Math.pow(1.15, 5));
  });
});
