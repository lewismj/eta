import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { snapToStep, throttle } from './util';

describe('throttle', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('calls immediately, then at most once per interval with the latest value', () => {
    const calls: number[] = [];
    const f = throttle((v: number) => calls.push(v), 100);
    f(1);
    f(2);
    f(3);
    expect(calls).toEqual([1]);
    vi.advanceTimersByTime(100);
    expect(calls).toEqual([1, 3]);
    vi.advanceTimersByTime(500);
    expect(calls).toEqual([1, 3]);
  });

  it('flush delivers the pending call now', () => {
    const calls: number[] = [];
    const f = throttle((v: number) => calls.push(v), 100);
    f(1);
    f(2);
    f.flush();
    expect(calls).toEqual([1, 2]);
    vi.advanceTimersByTime(200);
    expect(calls).toEqual([1, 2]);
  });

  it('cancel drops the pending call', () => {
    const calls: number[] = [];
    const f = throttle((v: number) => calls.push(v), 100);
    f(1);
    f(2);
    f.cancel();
    vi.advanceTimersByTime(200);
    expect(calls).toEqual([1]);
  });
});

describe('snapToStep', () => {
  it('snaps to the step grid from min', () => {
    expect(snapToStep(0.33, 0, 1, 0.05)).toBe(0.35);
    expect(snapToStep(7, 1, 10, 3)).toBe(7);
    expect(snapToStep(8.4, 1, 10, 3)).toBe(7);
  });
  it('clamps and avoids float noise', () => {
    expect(snapToStep(2, 0, 1, 0.1)).toBe(1);
    expect(snapToStep(-1, 0, 1, 0.1)).toBe(0);
    expect(snapToStep(0.3, 0, 1, 0.1)).toBe(0.3);
  });
  it('non-finite values fall back to min; zero step only clamps', () => {
    expect(snapToStep(NaN, 2, 5, 1)).toBe(2);
    expect(snapToStep(3.3, 2, 5, 0)).toBe(3.3);
  });
});
