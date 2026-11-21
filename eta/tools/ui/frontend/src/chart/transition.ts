/**
 * Keyed transitions, independent of the GPU.
 *
 * Given where every instance is *now* and the incoming data, decide each
 * instance's (from, to) position, opacity and selection. Matching keys move,
 * new keys fade in at their target, missing keys fade out where they are.
 */

export interface InstanceNow {
  key: string;
  x: number;
  y: number;
  alpha: number;
  sel: number;
  /** Opacity the instance is heading to (0 for an exiting point). */
  targetAlpha: number;
}

export interface PlannedInstance {
  key: string;
  /** Index into the incoming data, or -1 for an exiting point. */
  dataIndex: number;
  fx: number;
  fy: number;
  tx: number;
  ty: number;
  a0: number;
  a1: number;
  s0: number;
  s1: number;
}

export interface TransitionInput {
  xs: ArrayLike<number>;
  ys: ArrayLike<number>;
  keys: ArrayLike<unknown>;
  selected?: unknown;
}

export const keyString = (k: unknown): string => (typeof k === 'string' ? k : String(k));

export function selectedKeyOf(selected: unknown): string | null {
  return selected === undefined || selected === null || selected === false ? null : keyString(selected);
}

/**
 * Plan the next transition. Output order is draw order: exiting points first
 * (beneath), then entering/updating points, with the selected point last.
 * Points with non-finite coordinates are dropped.
 */
export function planTransition(now: InstanceNow[], data: TransitionInput): PlannedInstance[] {
  const selectedKey = selectedKeyOf(data.selected);
  const prev = new Map<string, InstanceNow>();
  for (const s of now) {
    if (s.alpha > 0.001 || s.targetAlpha > 0) prev.set(s.key, s);
  }

  const entries: PlannedInstance[] = [];
  const seen = new Set<string>();
  const n = Math.min(data.xs.length, data.ys.length, data.keys.length);
  for (let i = 0; i < n; i++) {
    const x = data.xs[i], y = data.ys[i];
    if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
    const key = keyString(data.keys[i]);
    seen.add(key);
    const old = prev.get(key);
    const s1 = key === selectedKey ? 1 : 0;
    entries.push(
      old
        ? { key, dataIndex: i, fx: old.x, fy: old.y, tx: x, ty: y, a0: old.alpha, a1: 1, s0: old.sel, s1 }
        : { key, dataIndex: i, fx: x, fy: y, tx: x, ty: y, a0: 0, a1: 1, s0: 0, s1 },
    );
  }

  const exits: PlannedInstance[] = [];
  for (const [key, s] of prev) {
    if (seen.has(key) || s.alpha <= 0.001) continue;
    exits.push({ key, dataIndex: -1, fx: s.x, fy: s.y, tx: s.x, ty: s.y, a0: s.alpha, a1: 0, s0: s.sel, s1: 0 });
  }

  // Stable sort: selected points draw on top.
  const ordered = entries
    .map((e, i) => ({ e, i }))
    .sort((a, b) => a.e.s1 - b.e.s1 || a.i - b.i)
    .map((x) => x.e);
  return exits.concat(ordered);
}
