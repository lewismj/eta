/** Zoom and pan maths for plots (pure, unit-tested). */

import type { Domain } from './scale';

/** Smallest zoom window as a fraction of the full data domain. */
export const MIN_ZOOM_FRACTION = 0.01;

/**
 * Zoom the x range about `anchor` (fraction 0..1 across the plot).
 * factor < 1 zooms in, > 1 zooms out. The result never exceeds `full` and
 * never gets narrower than MIN_ZOOM_FRACTION of it.
 */
export function zoomX(d: Domain, anchor: number, factor: number, full: Domain): Domain {
  const span = d.x1 - d.x0;
  const fullSpan = full.x1 - full.x0;
  if (!(span > 0) || !(fullSpan > 0)) return d;
  const a = Math.min(1, Math.max(0, anchor));
  const at = d.x0 + span * a;
  let nextSpan = Math.min(fullSpan, Math.max(fullSpan * MIN_ZOOM_FRACTION, span * factor));
  let x0 = at - nextSpan * a;
  let x1 = x0 + nextSpan;
  // Keep inside the full domain.
  if (x0 < full.x0) { x0 = full.x0; x1 = x0 + nextSpan; }
  if (x1 > full.x1) { x1 = full.x1; x0 = x1 - nextSpan; }
  if (nextSpan >= fullSpan) { x0 = full.x0; x1 = full.x1; nextSpan = fullSpan; }
  return { ...d, x0, x1 };
}

/** Pan the x range by `dxFraction` of its width, clamped to `full`. */
export function panX(d: Domain, dxFraction: number, full: Domain): Domain {
  const span = d.x1 - d.x0;
  let x0 = d.x0 - span * dxFraction;
  let x1 = x0 + span;
  if (x0 < full.x0) { x0 = full.x0; x1 = x0 + span; }
  if (x1 > full.x1) { x1 = full.x1; x0 = x1 - span; }
  return { ...d, x0, x1 };
}

/** True when `d` shows the whole of `full` on x (i.e. not zoomed). */
export function isFullX(d: Domain, full: Domain): boolean {
  const eps = (full.x1 - full.x0) * 1e-9;
  return Math.abs(d.x0 - full.x0) <= eps && Math.abs(d.x1 - full.x1) <= eps;
}

/** Wheel delta (pixels) to a zoom factor; one notch (~100px) = 15%. */
export function wheelFactor(deltaY: number): number {
  return Math.pow(1.15, Math.max(-5, Math.min(5, deltaY / 100)));
}
