import { describe, expect, it } from 'vitest';
import { InstanceNow, planTransition } from './transition';

const settled = (key: string, x: number, y: number, sel = 0): InstanceNow => ({
  key, x, y, alpha: 1, sel, targetAlpha: 1,
});

describe('planTransition', () => {
  it('first render: everything fades in at its target', () => {
    const plan = planTransition([], { xs: [1, 2], ys: [3, 4], keys: ['a', 'b'] });
    expect(plan).toHaveLength(2);
    for (const p of plan) {
      expect([p.fx, p.fy]).toEqual([p.tx, p.ty]);
      expect([p.a0, p.a1]).toEqual([0, 1]);
    }
    expect(plan.map((p) => p.dataIndex)).toEqual([0, 1]);
  });

  it('matching keys move from their current position', () => {
    const now = [settled('a', 1, 1), settled('b', 2, 2)];
    const plan = planTransition(now, { xs: [5, 6], ys: [7, 8], keys: ['b', 'a'] });
    const a = plan.find((p) => p.key === 'a')!;
    expect([a.fx, a.fy, a.tx, a.ty]).toEqual([1, 1, 6, 8]);
    expect([a.a0, a.a1]).toEqual([1, 1]);
    expect(a.dataIndex).toBe(1);
  });

  it('missing keys fade out in place and draw beneath', () => {
    const now = [settled('a', 1, 1), settled('gone', 9, 9)];
    const plan = planTransition(now, { xs: [1], ys: [1], keys: ['a'] });
    expect(plan[0].key).toBe('gone');
    expect(plan[0].dataIndex).toBe(-1);
    expect([plan[0].fx, plan[0].tx]).toEqual([9, 9]);
    expect([plan[0].a0, plan[0].a1]).toEqual([1, 0]);
  });

  it('already-invisible exits are dropped', () => {
    const now: InstanceNow[] = [{ key: 'ghost', x: 0, y: 0, alpha: 0, sel: 0, targetAlpha: 0 }];
    expect(planTransition(now, { xs: [], ys: [], keys: [] })).toHaveLength(0);
  });

  it('a point fading out that comes back resumes from its current opacity', () => {
    const now: InstanceNow[] = [{ key: 'a', x: 2, y: 2, alpha: 0.4, sel: 0, targetAlpha: 0 }];
    const [p] = planTransition(now, { xs: [3], ys: [3], keys: ['a'] });
    expect([p.a0, p.a1]).toEqual([0.4, 1]);
    expect([p.fx, p.tx]).toEqual([2, 3]);
  });

  it('numeric keys (from Float64 buffers) match string selections', () => {
    const plan = planTransition([], {
      xs: new Float64Array([1, 2, 3]),
      ys: new Float64Array([1, 2, 3]),
      keys: new Float64Array([10, 11, 12]),
      selected: 11,
    });
    const sel = plan.find((p) => p.s1 === 1)!;
    expect(sel.key).toBe('11');
    // Selected draws last; others keep data order.
    expect(plan.map((p) => p.key)).toEqual(['10', '12', '11']);
  });

  it('selection changes animate from the current selection level', () => {
    const now = [settled('a', 1, 1, 1), settled('b', 2, 2, 0)];
    const plan = planTransition(now, { xs: [1, 2], ys: [1, 2], keys: ['a', 'b'], selected: 'b' });
    const a = plan.find((p) => p.key === 'a')!;
    const b = plan.find((p) => p.key === 'b')!;
    expect([a.s0, a.s1]).toEqual([1, 0]);
    expect([b.s0, b.s1]).toEqual([0, 1]);
  });

  it('#f and missing selections select nothing', () => {
    for (const selected of [false, null, undefined]) {
      const plan = planTransition([], { xs: [1], ys: [1], keys: ['a'], selected });
      expect(plan[0].s1).toBe(0);
    }
  });

  it('non-finite coordinates are skipped', () => {
    const plan = planTransition([], { xs: [1, NaN, 3], ys: [1, 2, Infinity], keys: ['a', 'b', 'c'] });
    expect(plan.map((p) => p.key)).toEqual(['a']);
  });
});
