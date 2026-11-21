/** Shared types and the event channel back to Eta. */

export interface UiNode {
  /** Node type, e.g. "window", "row", "plot", "points". */
  t: string;
  /** Stable path id built from keys/positions. */
  id: string;
  /** Properties. Handler props are `{ $h: id }`. */
  p: Record<string, unknown>;
  /** Children. */
  c: UiNode[];
}

export interface HandlerRef {
  $h: string;
}

export function isHandler(v: unknown): v is HandlerRef {
  return v !== null && typeof v === 'object' && typeof (v as HandlerRef).$h === 'string';
}

type Sender = (msg: { type: string; [k: string]: unknown }) => void;
let sender: Sender = () => {};

export function setSender(s: Sender): void {
  sender = s;
}

/** Invoke an Eta handler. */
export function emitHandler(h: HandlerRef, data: Record<string, unknown> = {}): void {
  sender({ type: 'event', h: h.$h, data });
}

export function str(v: unknown, fallback = ''): string {
  return typeof v === 'string' ? v : typeof v === 'number' ? String(v) : fallback;
}
