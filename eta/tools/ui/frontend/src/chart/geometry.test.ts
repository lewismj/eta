import { describe, expect, it } from 'vitest';
import {
  areaTriangles, barRects, interleave, isSorted, lerpArrays, lineSegments, morphFrom, nearestIndex, planArea, planBars,
  planLine,
} from './geometry';

describe('lineSegments', () => {
  it('joins consecutive points', () => {
    expect(Array.from(lineSegments([0, 1, 2], [0, 1, 0]))).toEqual([0, 0, 1, 1, 1, 1, 2, 0]);
  });
  it('breaks at non-finite values instead of bridging', () => {
    const s = lineSegments([0, 1, 2, 3], [0, NaN, 1, 2]);
    expect(Array.from(s)).toEqual([2, 1, 3, 2]);
  });
  it('is empty for fewer than two points', () => {
    expect(lineSegments([1], [1]).length).toBe(0);
  });
});

describe('areaTriangles', () => {
  it('emits two triangles per segment with v on the edges', () => {
    const t = areaTriangles([0, 1, 2], [0, 0, 0], [1, 2, 1]);
    expect(t.length).toBe(2 * 2 * 3 * 3); // 2 segments, 2 tris, 3 verts, (x,y,v)
    const vs = Array.from({ length: t.length / 3 }, (_, i) => t[3 * i + 2]);
    expect(new Set(vs)).toEqual(new Set([0, 1]));
  });
  it('uses the baseline when there is no lower edge', () => {
    const t = areaTriangles([0, 1], null, [2, 2], -1);
    expect(t[1]).toBe(-1); // first vertex sits on the baseline
  });
  it('skips segments with gaps', () => {
    expect(areaTriangles([0, 1, 2], null, [1, NaN, 1]).length).toBe(0);
  });
});

describe('nearestIndex', () => {
  const xs = [0, 1, 2, 4, 8];
  it('clamps to the ends', () => {
    expect(nearestIndex(xs, -5)).toBe(0);
    expect(nearestIndex(xs, 100)).toBe(4);
  });
  it('snaps to the closest x', () => {
    expect(nearestIndex(xs, 2.9)).toBe(2);
    expect(nearestIndex(xs, 3.1)).toBe(3);
    expect(nearestIndex(xs, 6.1)).toBe(4);
  });
  it('handles empty input and NaN', () => {
    expect(nearestIndex([], 1)).toBe(-1);
    expect(nearestIndex(xs, NaN)).toBe(-1);
  });
  it('agrees with a linear scan on random data', () => {
    const sorted = Array.from({ length: 200 }, (_, i) => i * 0.7 + (i % 3) * 0.1);
    for (let k = 0; k < 100; k++) {
      const x = Math.sin(k) * 80 + 70;
      let best = 0;
      for (let i = 1; i < sorted.length; i++) if (Math.abs(sorted[i] - x) < Math.abs(sorted[best] - x)) best = i;
      expect(Math.abs(sorted[nearestIndex(sorted, x)] - x)).toBeCloseTo(Math.abs(sorted[best] - x), 12);
    }
  });
});

describe('small helpers', () => {
  it('isSorted', () => {
    expect(isSorted([1, 1, 2])).toBe(true);
    expect(isSorted([2, 1])).toBe(false);
  });
  it('morphFrom only when lengths match', () => {
    expect(morphFrom([1, 2], [3, 4])).not.toBeNull();
    expect(morphFrom([1], [3, 4])).toBeNull();
    expect(morphFrom(null, [3])).toBeNull();
  });
  it('lerpArrays', () => {
    expect(Array.from(lerpArrays([0, 10], [10, 20], 0.5))).toEqual([5, 15]);
  });
  it('interleave', () => {
    expect(Array.from(interleave([1, 2], [3, 4]))).toEqual([1, 3, 2, 4]);
  });
  it('barRects anchor at the baseline and leave padding', () => {
    const r = barRects([3, -2, NaN], 0.3);
    expect(r).toHaveLength(2);
    expect(r[0]).toMatchObject({ v0: 0, v1: 3, center: 0 });
    expect(r[1]).toMatchObject({ v0: -2, v1: 0 });
    expect(r[0].halfHeight).toBeCloseTo(0.35);
  });
});

describe('planLine', () => {
  const v = (...xs: number[]) => Float32Array.from(xs);
  it('first render fades segments in', () => {
    const p = planLine(null, v(0, 0, 1, 1, 2, 0));
    expect(p.alpha).toEqual([0, 1, 0, 1]);
  });
  it('same vertex count morphs every vertex from where it is now', () => {
    const p = planLine(v(0, 0, 1, 1), v(0, 5, 1, 6));
    expect(p.a).toEqual([0, 0, 0, 5]);
    expect(p.b).toEqual([1, 1, 1, 6]);
    expect(p.alpha).toEqual([1, 1]);
  });
  it('different vertex count cross-fades old and new', () => {
    const p = planLine(v(0, 0, 1, 1), v(0, 0, 1, 1, 2, 2));
    // one old segment fading out, two new fading in
    expect(p.alpha).toEqual([1, 0, 0, 1, 0, 1]);
  });
  it('segments with a gap in the new data are dropped; a gap now fades the segment in', () => {
    const p = planLine(v(0, NaN, 1, 1, 2, 2), v(0, 0, 1, NaN, 2, 2));
    expect(p.alpha).toEqual([]);
    const q = planLine(v(0, NaN, 1, 1), v(0, 0, 1, 1));
    expect(q.alpha).toEqual([0, 1]);
  });
});

describe('planArea', () => {
  const tri = (...xs: number[]) => Float32Array.from(xs);
  it('morphs when the vertex count matches', () => {
    const now = tri(0, 0, 0, 1, 0, 0, 1, 1, 1);
    const next = tri(0, 1, 0, 1, 1, 0, 1, 3, 1);
    const p = planArea(now, next);
    expect(Array.from(p.from)).toEqual([0, 0, 1, 0, 1, 1]);
    expect(Array.from(p.to)).toEqual([0, 1, 1, 1, 1, 3]);
    expect(Array.from(p.alpha)).toEqual([1, 1, 1, 1, 1, 1]);
    expect(Array.from(p.v)).toEqual([0, 0, 1]);
  });
  it('cross-fades otherwise', () => {
    const p = planArea(tri(0, 0, 0, 1, 0, 0, 1, 1, 1), tri());
    expect(Array.from(p.alpha)).toEqual([1, 0, 1, 0, 1, 0]);
    const q = planArea(null, tri(0, 0, 0, 1, 0, 0, 1, 1, 1));
    expect(Array.from(q.alpha)).toEqual([0, 1, 0, 1, 0, 1]);
  });
});

describe('planBars', () => {
  it('new bars grow from the baseline', () => {
    const [b] = planBars([], ['A'], [5], null);
    expect([b.v00, b.v01, b.v10, b.v11]).toEqual([0, 0, 0, 5]);
    expect(b.dataIndex).toBe(0);
  });
  it('matching categories slide to their new row and resize', () => {
    const now = [{ category: 'A', center: 0, v0: 0, v1: 5, alpha: 1, sel: 0 }];
    const [b] = planBars(now, ['B', 'A'], [1, 7], null).filter((x) => x.category === 'A');
    expect([b.c0, b.c1]).toEqual([0, 1]);
    expect([b.v01, b.v11]).toEqual([5, 7]);
  });
  it('missing categories shrink to the baseline and fade, drawn first', () => {
    const now = [{ category: 'Z', center: 2, v0: 0, v1: 4, alpha: 1, sel: 0 }];
    const plan = planBars(now, ['A'], [1], null);
    expect(plan[0]).toMatchObject({ category: 'Z', dataIndex: -1, v10: 0, v11: 0, a1: 0 });
  });
  it('negative values extend left of the baseline', () => {
    const [b] = planBars([], ['A'], [-3], null);
    expect([b.v10, b.v11]).toEqual([-3, 0]);
  });
  it('selection targets only the selected category', () => {
    const plan = planBars([], ['A', 'B'], [1, 2], 'B');
    expect(plan.map((b) => b.s1)).toEqual([0, 1]);
  });
  it('duplicate categories are ignored after the first', () => {
    expect(planBars([], ['A', 'A'], [1, 2], null)).toHaveLength(1);
  });
});
