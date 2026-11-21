/** Test helpers for component tests (jsdom). */

import type { UiNode } from './model';
import { setSender } from './model';

export type Sent = { type: string; h?: string; data?: Record<string, unknown> };

/** Capture every message the UI would send to Eta. */
export function captureSent(): Sent[] {
  const sent: Sent[] = [];
  setSender((m) => sent.push(m as Sent));
  return sent;
}

export const n = (t: string, p: Record<string, unknown> = {}, c: UiNode[] = [], id = `r/${t}`): UiNode => ({
  t,
  id,
  p,
  c,
});
