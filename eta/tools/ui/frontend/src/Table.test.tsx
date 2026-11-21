// @vitest-environment jsdom
import { cleanup, fireEvent, render, screen } from '@solidjs/testing-library';
import { afterEach, describe, expect, it } from 'vitest';
import { Table } from './Table';
import { captureSent, n } from './testing';

afterEach(cleanup);

const node = (extra: Record<string, unknown> = {}) =>
  n('table', {
    title: 'Netting sets',
    columns: [
      { key: 'cp', label: 'Counterparty' },
      { key: 'cva', label: 'CVA', format: 'int' },
    ],
    data: { cp: ['CP-10', 'CP-2', 'CP-1'], cva: [300, 1200, 50] },
    keys: ['CP-10', 'CP-2', 'CP-1'],
    'on-select': { $h: 'r/t#on-select' },
    ...extra,
  });

const bodyColumn = (col: number) =>
  Array.from(document.querySelectorAll('tbody tr')).map((tr) => tr.querySelectorAll('td')[col].textContent);

describe('Table', () => {
  it('renders headers, formatted cells and a row count', () => {
    render(() => <Table node={node()} />);
    expect(screen.getByText('Counterparty')).toBeTruthy();
    expect(screen.getByText('3 rows')).toBeTruthy();
    expect(bodyColumn(1)).toEqual(['300', '1,200', '50']);
  });

  it('right-aligns numeric columns', () => {
    render(() => <Table node={node()} />);
    const td = document.querySelector('tbody tr td:nth-child(2)')!;
    expect(td.classList.contains('right')).toBe(true);
  });

  it('sorts ascending then descending on header clicks', () => {
    render(() => <Table node={node()} />);
    const header = screen.getByText('CVA').closest('button')!;
    fireEvent.click(header);
    const first = bodyColumn(1);
    fireEvent.click(header);
    const second = bodyColumn(1);
    // One click sorts one way, the next reverses it.
    expect(first).toEqual([...second].reverse());
    expect(new Set([first.join(), second.join()])).toEqual(new Set(['50,300,1,200', '1,200,300,50']));
  });

  it('sorts text naturally', () => {
    render(() => <Table node={node()} />);
    fireEvent.click(screen.getByText('Counterparty').closest('button')!);
    const order = bodyColumn(0);
    expect([order, [...order].reverse()]).toContainEqual(['CP-1', 'CP-2', 'CP-10']);
  });

  it('marks the sorted column for assistive tech', () => {
    render(() => <Table node={node()} />);
    const th = screen.getByText('CVA').closest('th')!;
    expect(th.getAttribute('aria-sort')).toBe('none');
    fireEvent.click(th.querySelector('button')!);
    expect(['ascending', 'descending']).toContain(th.getAttribute('aria-sort'));
  });

  it('row click sends the original index and key, even after sorting', () => {
    const sent = captureSent();
    render(() => <Table node={node()} />);
    fireEvent.click(screen.getByText('CVA').closest('button')!);
    const row = document.querySelector('tbody tr[data-key="CP-2"]')!;
    fireEvent.click(row);
    expect(sent.at(-1)).toEqual({ type: 'event', h: 'r/t#on-select', data: { index: 1, key: 'CP-2' } });
  });

  it('highlights the selected row', () => {
    render(() => <Table node={node({ selected: 'CP-1' })} />);
    const row = document.querySelector('tbody tr[data-key="CP-1"]')!;
    expect(row.classList.contains('selected')).toBe(true);
    expect(row.getAttribute('aria-selected')).toBe('true');
  });

  it('accepts Float64Array numeric columns', () => {
    render(() => <Table node={node({ data: { cp: ['a', 'b', 'c'], cva: new Float64Array([1, 2, 3]) } })} />);
    expect(bodyColumn(1)).toEqual(['1', '2', '3']);
  });

  it('virtualises large tables instead of rendering every row', () => {
    const count = 1000;
    render(() => (
      <Table node={node({
        data: { cp: Array.from({ length: count }, (_, i) => `CP-${i}`), cva: Array.from({ length: count }, (_, i) => i) },
        keys: Array.from({ length: count }, (_, i) => `CP-${i}`),
      })} />
    ));
    expect(document.querySelector('tbody.virtual')).toBeTruthy();
    expect(document.querySelectorAll('tbody tr').length).toBeLessThan(count);
  });
});
