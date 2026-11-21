// @vitest-environment jsdom
import { cleanup, fireEvent, render, screen } from '@solidjs/testing-library';
import { afterEach, describe, expect, it } from 'vitest';
import { captureSent, n } from '../testing';
import { Heatmap } from './Heatmap';

afterEach(cleanup);

const node = (extra: Record<string, unknown> = {}) =>
  n('heatmap', {
    title: 'WWR elasticity',
    rows: ['CP-01', 'CP-02'],
    cols: ['oil', 'usd-10y', 'eur-usd'],
    values: [0.5, -0.2, 0, 2.9, 0.1, -1.5],
    format: 'num',
    'on-select': { $h: 'r/h#on-select' },
    ...extra,
  });

const rgb = (hex: string) => {
  const v = parseInt(hex.slice(1), 16);
  return `rgb(${(v >> 16) & 255}, ${(v >> 8) & 255}, ${v & 255})`;
};

describe('Heatmap', () => {
  it('renders one cell per value with row and column headers', () => {
    render(() => <Heatmap node={node()} />);
    expect(screen.getAllByRole('gridcell')).toHaveLength(6);
    expect(screen.getAllByRole('columnheader').map((e) => e.textContent)).toEqual(['oil', 'usd-10y', 'eur-usd']);
    expect(screen.getAllByRole('rowheader').map((e) => e.textContent)).toEqual(['CP-01', 'CP-02']);
  });

  it('colours zero neutral and the extremes with the diverging poles', () => {
    render(() => <Heatmap node={node()} />);
    const cell = (r: number, c: number) => document.querySelector(`[data-row="${r}"][data-col="${c}"]`) as HTMLElement;
    expect(cell(0, 2).style.background).toBe(rgb('#383835'));
    expect(cell(1, 0).style.background).toBe(rgb('#e66767'));
  });

  it('labels cells for screen readers', () => {
    render(() => <Heatmap node={node()} />);
    expect(screen.getByLabelText('CP-02 × oil: 2.90')).toBeTruthy();
  });

  it('clicking a cell selects its row and reports the column', () => {
    const sent = captureSent();
    render(() => <Heatmap node={node()} />);
    fireEvent.click(screen.getByLabelText('CP-02 × eur-usd: −1.50'));
    expect(sent.at(-1)).toEqual({
      type: 'event', h: 'r/h#on-select', data: { row: 1, col: 2, index: 1, key: 'CP-02', column: 'eur-usd' },
    });
  });

  it('clicking a row header selects the row', () => {
    const sent = captureSent();
    render(() => <Heatmap node={node()} />);
    fireEvent.click(screen.getByText('CP-01'));
    expect(sent.at(-1)?.data).toMatchObject({ row: 0, key: 'CP-01' });
  });

  it('highlights the selected row', () => {
    render(() => <Heatmap node={node({ 'selected-row': 'CP-02' })} />);
    expect(screen.getByText('CP-02').classList.contains('selected')).toBe(true);
    expect(document.querySelectorAll('.heatmap-cell.selected')).toHaveLength(3);
  });

  it('shows a tooltip on hover', () => {
    render(() => <Heatmap node={node()} />);
    fireEvent.mouseEnter(screen.getByLabelText('CP-01 × oil: 0.500'));
    expect(screen.getByRole('tooltip').textContent).toContain('CP-01');
    expect(screen.getByRole('tooltip').textContent).toContain('oil');
  });

  it('prints values only when asked', () => {
    render(() => <Heatmap node={node({ 'show-values': true })} />);
    expect(screen.getByLabelText('CP-02 × oil: 2.90').textContent).toBe('2.90');
  });

  it('reports a shape mismatch instead of drawing', () => {
    render(() => <Heatmap node={node({ values: [1, 2, 3] })} />);
    expect(screen.getByRole('alert').textContent).toMatch(/expects 2 × 3/);
    expect(screen.queryAllByRole('gridcell')).toHaveLength(0);
  });

  it('draws a legend with min, mid and max', () => {
    render(() => <Heatmap node={node()} />);
    const ticks = Array.from(document.querySelectorAll('.heatmap-legend-ticks span')).map((e) => e.textContent);
    expect(ticks).toEqual(['−2.90', '0', '2.90']);
  });
});
