import { describe, expect, it } from 'vitest';
import { compareCells, formatCell, parseColumns, parseTable } from './tableModel';

describe('parseColumns', () => {
  it('accepts strings and objects, right-aligning formatted columns', () => {
    const cols = parseColumns([
      'name',
      { key: 'cva', label: 'CVA', format: 'int' },
      { key: 'note', align: 'right' },
      { label: 'no key' },
      42,
    ]);
    expect(cols).toEqual([
      { key: 'name', label: 'name', align: 'left' },
      { key: 'cva', label: 'CVA', format: 'int', align: 'right', width: undefined },
      { key: 'note', label: 'note', format: undefined, align: 'right', width: undefined },
    ]);
  });
});

describe('parseTable', () => {
  const p = {
    columns: [{ key: 'cp', label: 'Counterparty' }, { key: 'cva', label: 'CVA', format: 'int' }],
    data: { cp: ['CP-01', 'CP-02'], cva: new Float64Array([1200.4, 900]) },
    keys: ['CP-01', 'CP-02'],
    selected: 'CP-02',
  };
  it('rebuilds rows from columns with keys and indices', () => {
    const t = parseTable(p);
    expect(t.rows).toHaveLength(2);
    expect(t.rows[1]).toMatchObject({ __key: 'CP-02', __index: 1, cp: 'CP-02', cva: 900 });
    expect(t.selected).toBe('CP-02');
  });
  it('falls back to positional keys and tolerates ragged columns', () => {
    const t = parseTable({ columns: ['a', 'b'], data: { a: [1, 2, 3], b: [4] } });
    expect(t.rows.map((r) => r.__key)).toEqual(['0', '1', '2']);
    expect(t.rows[2].b).toBeUndefined();
    expect(t.selected).toBeNull();
  });
  it('handles a missing data map', () => {
    expect(parseTable({ columns: ['a'] }).rows).toEqual([]);
  });
});

describe('compareCells', () => {
  it('orders numbers numerically and text naturally', () => {
    expect([10, 2, 33].sort(compareCells)).toEqual([2, 10, 33]);
    expect(['CP-10', 'CP-2', 'CP-1'].sort(compareCells)).toEqual(['CP-1', 'CP-2', 'CP-10']);
  });
  it('puts blanks last', () => {
    expect([3, null, 1, NaN, undefined].sort(compareCells).slice(0, 2)).toEqual([1, 3]);
  });
});

describe('formatCell', () => {
  it('uses the column format', () => {
    expect(formatCell(1234.6, { key: 'x', label: 'x', format: 'int', align: 'right' })).toBe('1,235');
    expect(formatCell('CP-01', { key: 'x', label: 'x', align: 'left' })).toBe('CP-01');
  });
});
