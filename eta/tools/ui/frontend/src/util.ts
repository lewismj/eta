/** Small shared helpers (pure, unit-tested). */

/**
 * Leading + trailing throttle: calls `fn` at most once per `ms`, always
 * delivering the latest arguments last. `flush()` sends any pending call now.
 */
export function throttle<A extends unknown[]>(fn: (...args: A) => void, ms: number) {
  let last = -Infinity;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let pending: A | null = null;

  const fire = () => {
    timer = null;
    if (!pending) return;
    const args = pending;
    pending = null;
    last = Date.now();
    fn(...args);
  };

  const call = (...args: A) => {
    pending = args;
    const wait = ms - (Date.now() - last);
    if (wait <= 0) {
      if (timer) {
        clearTimeout(timer);
        timer = null;
      }
      fire();
    } else if (!timer) {
      timer = setTimeout(fire, wait);
    }
  };
  call.flush = () => {
    if (timer) {
      clearTimeout(timer);
      timer = null;
    }
    fire();
  };
  call.cancel = () => {
    if (timer) clearTimeout(timer);
    timer = null;
    pending = null;
  };
  return call;
}

/** Snap `v` to the slider grid defined by min/step and clamp to [min, max]. */
export function snapToStep(v: number, min: number, max: number, step: number): number {
  if (!Number.isFinite(v)) return min;
  const clamped = Math.min(max, Math.max(min, v));
  if (!(step > 0)) return clamped;
  const n = Math.round((clamped - min) / step);
  // Avoid float noise such as 0.30000000000000004.
  const decimals = Math.max(0, -Math.floor(Math.log10(step)) + 1);
  return Math.min(max, Number((min + n * step).toFixed(Math.min(12, decimals + 2))));
}
