import { describe, expect, it } from 'vitest';
import { axisDomain, easeInOutCubic, lerpDomain, niceStep, ticks } from './scale';

describe('niceStep', () => {
  it('picks 1-2-5 steps', () => {
    expect(niceStep(1, 5)).toBeCloseTo(0.2);
    expect(niceStep(0.3, 6)).toBeCloseTo(0.05);
    expect(niceStep(1000, 4)).toBeCloseTo(200);
    expect(niceStep(7, 7)).toBeCloseTo(1);
  });
  it('is safe for degenerate spans', () => {
    expect(niceStep(0, 5)).toBe(1);
    expect(niceStep(NaN, 5)).toBe(1);
  });
});

describe('ticks', () => {
  it('covers the range on step multiples', () => {
    const t = ticks(0.03, 0.27, 5);
    expect(t.step).toBeCloseTo(0.05);
    expect(t.values.map((v) => +v.toFixed(4))).toEqual([0.05, 0.1, 0.15, 0.2, 0.25]);
  });
  it('snaps values near zero to exactly zero', () => {
    const t = ticks(-0.3, 0.3, 6);
    expect(t.values).toContain(0);
    expect(Object.is(t.values.find((v) => Math.abs(v) < 1e-12), -0)).toBe(false);
  });
});

describe('axisDomain', () => {
  it('pads and expands to the tick grid', () => {
    const [a, b] = axisDomain(0.031, 0.072, undefined, 5);
    expect(a).toBeLessThan(0.031);
    expect(b).toBeGreaterThan(0.072);
    const step = niceStep(b - a, 5);
    expect(Math.abs(a / step - Math.round(a / step))).toBeLessThan(1e-6);
  });
  it('includes zero when asked', () => {
    const [a] = axisDomain(5, 10, { zero: true }, 5);
    expect(a).toBe(0);
  });
  it('honours explicit min and max', () => {
    expect(axisDomain(1, 2, { min: -1, max: 3 }, 5)).toEqual([-1, 3]);
  });
  it('handles a single value and empty data', () => {
    const [a, b] = axisDomain(2, 2, undefined, 5);
    expect(a).toBeLessThan(2);
    expect(b).toBeGreaterThan(2);
    const [c, d] = axisDomain(Infinity, -Infinity, undefined, 5);
    expect(Number.isFinite(c) && Number.isFinite(d)).toBe(true);
  });
});

describe('easing and interpolation', () => {
  it('easeInOutCubic is clamped and symmetric', () => {
    expect(easeInOutCubic(-1)).toBe(0);
    expect(easeInOutCubic(0)).toBe(0);
    expect(easeInOutCubic(0.5)).toBeCloseTo(0.5);
    expect(easeInOutCubic(1)).toBe(1);
    expect(easeInOutCubic(2)).toBe(1);
    expect(easeInOutCubic(0.25) + easeInOutCubic(0.75)).toBeCloseTo(1);
  });
  it('lerpDomain blends all four bounds', () => {
    const d = lerpDomain({ x0: 0, x1: 10, y0: 0, y1: 1 }, { x0: 10, x1: 20, y0: 1, y1: 3 }, 0.5);
    expect(d).toEqual({ x0: 5, x1: 15, y0: 0.5, y1: 2 });
  });
});
