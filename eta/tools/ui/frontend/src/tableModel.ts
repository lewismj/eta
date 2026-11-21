/**
 * Table model (pure). Eta sends tables column-major:
 *   'columns  list of {key, label, format?, align?, width?}
 *   'data     {key: column vector}   (numeric columns arrive as Float64Array)
 *   'keys     row keys
 * Rows are rebuilt here for the table library.
 */

import { formatValue } from './format';

export interface ColumnSpec {
  key: string;
  label: string;
  format?: string;
  align: 'left' | 'right';
  width?: number;
}

export interface TableRow {
  __key: string;
  __index: number;
  [column: string]: unknown;
}

export interface TableSpec {
  columns: ColumnSpec[];
  rows: TableRow[];
  selected: string | null;
}

const asList = (v: unknown): unknown[] =>
  v instanceof Float64Array ? Array.from(v) : Array.isArray(v) ? v : [];

export function parseColumns(v: unknown): ColumnSpec[] {
  return asList(v).flatMap((c): ColumnSpec[] => {
    if (typeof c === 'string') return [{ key: c, label: c, align: 'left' as const }];
    if (!c || typeof c !== 'object') return [];
    const o = c as Record<string, unknown>;
    const key = typeof o.key === 'string' ? o.key : null;
    if (!key) return [];
    const format = typeof o.format === 'string' ? o.format : undefined;
    return [{
      key,
      label: typeof o.label === 'string' ? o.label : key,
      format,
      // Numbers right-align by default so digits line up.
      align: o.align === 'left' || o.align === 'right' ? o.align : format ? 'right' : 'left',
      width: typeof o.width === 'number' ? o.width : undefined,
    }];
  });
}

export function parseTable(p: Record<string, unknown>): TableSpec {
  const columns = parseColumns(p.columns);
  const data = (p.data && typeof p.data === 'object' ? p.data : {}) as Record<string, unknown>;
  const cols = new Map(columns.map((c) => [c.key, asList(data[c.key])]));
  const keys = asList(p.keys);
  const n = Math.max(keys.length, ...[...cols.values()].map((c) => c.length), 0);
  const rows: TableRow[] = [];
  for (let i = 0; i < n; i++) {
    const row: TableRow = { __key: i < keys.length ? String(keys[i]) : String(i), __index: i };
    for (const [k, c] of cols) row[k] = c[i];
    rows.push(row);
  }
  const sel = p.selected;
  return { columns, rows, selected: sel === undefined || sel === null || sel === false ? null : String(sel) };
}

export function formatCell(v: unknown, col: ColumnSpec): string {
  return formatValue(v, col.format);
}

/** Sort comparator: numbers numerically, everything else as text; blanks last. */
export function compareCells(a: unknown, b: unknown): number {
  const blank = (v: unknown) => v === undefined || v === null || v === false || (typeof v === 'number' && Number.isNaN(v));
  if (blank(a) && blank(b)) return 0;
  if (blank(a)) return 1;
  if (blank(b)) return -1;
  if (typeof a === 'number' && typeof b === 'number') return a - b;
  return String(a).localeCompare(String(b), undefined, { numeric: true });
}

/** Tables above this many rows are virtualised. */
export const VIRTUALIZE_ABOVE = 200;
export const TABLE_ROW_HEIGHT = 30;
