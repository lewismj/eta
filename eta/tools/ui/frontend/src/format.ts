/** Number formatting shared by stats, tables and axes. */

export type FormatKind = 'num' | 'pct' | 'bp' | 'int' | 'usd' | string;

/** Decimals needed to show values spaced `step` apart (in display units). */
export function decimalsFor(step: number): number {
  if (!isFinite(step) || step <= 0) return 2;
  return Math.max(0, Math.min(6, -Math.floor(Math.log10(step) + 1e-9)));
}

function scaleFor(kind: FormatKind): number {
  if (kind === 'pct') return 100;
  if (kind === 'bp') return 10000;
  return 1;
}

function suffixFor(kind: FormatKind): string {
  if (kind === 'pct') return '%';
  if (kind === 'bp') return ' bp';
  return '';
}

const groupFormatters = new Map<number, Intl.NumberFormat>();
function grouped(decimals: number): Intl.NumberFormat {
  let f = groupFormatters.get(decimals);
  if (!f) {
    f = new Intl.NumberFormat('en-US', {
      minimumFractionDigits: decimals,
      maximumFractionDigits: decimals,
    });
    groupFormatters.set(decimals, f);
  }
  return f;
}

/**
 * Format `v`. `step` (in data units) picks the precision, e.g. the tick
 * spacing; when omitted a sensible precision for the magnitude is used.
 */
export function formatNumber(v: number, kind: FormatKind = 'num', step?: number): string {
  if (v === null || v === undefined || !isFinite(v)) return '–';
  const scale = scaleFor(kind);
  const x = v * scale;
  let decimals: number;
  if (kind === 'int') decimals = 0;
  else if (step !== undefined) decimals = decimalsFor(step * scale);
  else {
    const a = Math.abs(x);
    decimals = a >= 1000 ? 0 : a >= 100 ? 1 : a >= 1 ? 2 : a === 0 ? 0 : 3;
  }
  const prefix = kind === 'usd' ? '$' : '';
  const body = grouped(decimals).format(Math.abs(x) < 1e-12 ? 0 : x);
  return prefix + body.replace('-', '−') + suffixFor(kind);
}

export function formatValue(v: unknown, kind?: FormatKind): string {
  if (typeof v === 'number') return formatNumber(v, kind ?? 'num');
  if (v === null || v === undefined || v === false) return '–';
  return String(v);
}
