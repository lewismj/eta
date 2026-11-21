import { describe, expect, it } from 'vitest';
import {
  CATEGORICAL, divergingScale, hexToRgb, oklabToRgb, rampAt, rgbToHex, rgbToOklab, sequentialScale, seriesColor,
  symmetricExtent,
} from './color';

const lightness = (hex: string) => rgbToOklab(hexToRgb(hex))[0];

describe('hex and OKLab conversions', () => {
  it('round-trips hex', () => {
    for (const hex of ['#000000', '#ffffff', '#3987e5', '#e66767', '#1a1a19']) {
      expect(rgbToHex(hexToRgb(hex))).toBe(hex);
    }
    expect(hexToRgb('#abc')).toEqual(hexToRgb('#aabbcc'));
  });
  it('round-trips through OKLab', () => {
    for (const hex of CATEGORICAL.dark.concat(CATEGORICAL.light)) {
      expect(rgbToHex(oklabToRgb(rgbToOklab(hexToRgb(hex))))).toBe(hex);
    }
  });
  it('OKLab lightness of white is 1 and black is 0', () => {
    expect(rgbToOklab([1, 1, 1])[0]).toBeCloseTo(1, 3);
    expect(rgbToOklab([0, 0, 0])[0]).toBeCloseTo(0, 6);
  });
});

describe('rampAt', () => {
  it('hits the end stops exactly and clamps outside [0, 1]', () => {
    expect(rgbToHex(rampAt(['#000000', '#ffffff'], 0))).toBe('#000000');
    expect(rgbToHex(rampAt(['#000000', '#ffffff'], 1))).toBe('#ffffff');
    expect(rgbToHex(rampAt(['#000000', '#ffffff'], -3))).toBe('#000000');
    expect(rgbToHex(rampAt(['#000000', '#ffffff'], 9))).toBe('#ffffff');
  });
  it('passes through interior stops', () => {
    const stops = ['#000000', '#3987e5', '#ffffff'];
    expect(rgbToHex(rampAt(stops, 0.5))).toBe('#3987e5');
  });
});

describe('sequentialScale', () => {
  it('dark theme: magnitude increases lightness (low values recede)', () => {
    const s = sequentialScale(0, 10, 'dark');
    const ls = s.legend(7).map(lightness);
    for (let i = 1; i < ls.length; i++) expect(ls[i]).toBeGreaterThan(ls[i - 1]);
  });
  it('light theme: magnitude decreases lightness', () => {
    const s = sequentialScale(0, 10, 'light');
    const ls = s.legend(7).map(lightness);
    for (let i = 1; i < ls.length; i++) expect(ls[i]).toBeLessThan(ls[i - 1]);
  });
  it('handles a zero-width domain', () => {
    const s = sequentialScale(5, 5);
    expect(() => s.color(5)).not.toThrow();
  });
});

describe('divergingScale', () => {
  const d = divergingScale(-1, 0, 1, 'dark');
  it('midpoint is the neutral grey', () => {
    expect(rgbToHex(d.color(0))).toBe('#383835');
  });
  it('poles are the palette blue and red', () => {
    expect(rgbToHex(d.color(-1))).toBe('#3987e5');
    expect(rgbToHex(d.color(1))).toBe('#e66767');
  });
  it('saturates beyond the domain and is neutral for NaN', () => {
    expect(rgbToHex(d.color(-5))).toBe('#3987e5');
    expect(rgbToHex(d.color(5))).toBe('#e66767');
    expect(rgbToHex(d.color(NaN))).toBe('#383835');
  });
  it('equal distances from the midpoint get equally strong colours', () => {
    const mid = rgbToOklab(hexToRgb('#383835'));
    const dist = (v: number) => {
      const c = rgbToOklab(d.color(v));
      return Math.hypot(c[0] - mid[0], c[1] - mid[1], c[2] - mid[2]);
    };
    // Each arm is linear in OKLab from the midpoint toward its pole.
    expect(dist(0.5) / dist(1)).toBeCloseTo(0.5, 2);
    expect(dist(-0.5) / dist(-1)).toBeCloseTo(0.5, 2);
  });
  it('light theme uses the light poles and midpoint', () => {
    const l = divergingScale(-2, 0, 2, 'light');
    expect(rgbToHex(l.color(0))).toBe('#f0efec');
    expect(rgbToHex(l.color(2))).toBe('#e34948');
  });
  it('legend runs from negative pole to positive pole', () => {
    const stops = d.legend(5);
    expect(stops[0]).toBe('#3987e5');
    expect(stops[2]).toBe('#383835');
    expect(stops[4]).toBe('#e66767');
  });
});

describe('categorical slots', () => {
  it('are the validated palette in fixed order', () => {
    expect(seriesColor(0)).toBe('#3987e5');
    expect(seriesColor(1)).toBe('#d95926');
    expect(seriesColor(0, 'light')).toBe('#2a78d6');
  });
  it('never generate a ninth colour', () => {
    expect(seriesColor(8)).toBe(CATEGORICAL.dark[7]);
    expect(seriesColor(-1)).toBe(CATEGORICAL.dark[0]);
  });
});

describe('symmetricExtent', () => {
  it('is the largest absolute finite value', () => {
    expect(symmetricExtent([-3, 1, 2, NaN])).toBe(3);
    expect(symmetricExtent([])).toBe(1);
  });
});
