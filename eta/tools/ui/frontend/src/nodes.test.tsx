// @vitest-environment jsdom
import { cleanup, fireEvent, render, screen } from '@solidjs/testing-library';
import { afterEach, describe, expect, it } from 'vitest';
import { Children, NodeView } from './nodes';
import { captureSent, n } from './testing';

afterEach(cleanup);

describe('element registry', () => {
  it('renders layout containers with their children', () => {
    render(() => (
      <NodeView node={n('row', {}, [n('panel', { title: 'Assumptions', width: 280 }, [n('text', { text: 'hello' })], 'r/0')])} />
    ));
    expect(screen.getByText('Assumptions')).toBeTruthy();
    expect(screen.getByText('hello')).toBeTruthy();
    const panel = screen.getByText('Assumptions').closest('section')!;
    expect(panel.style.width).toBe('280px');
    expect(['none', '0 0 auto']).toContain(panel.style.flex);
  });

  it("'flex 0 sizes to content", () => {
    render(() => <NodeView node={n('row', { flex: 0 })} />);
    expect(['none', '0 0 auto']).toContain((document.querySelector('.row') as HTMLElement).style.flex);
  });

  it('stat formats numbers', () => {
    render(() => <NodeView node={n('stat', { label: 'CVA', value: 118302.4, format: 'int' })} />);
    expect(screen.getByText('118,302')).toBeTruthy();
  });

  it('kv lists label/value pairs', () => {
    render(() => <NodeView node={n('kv', { rows: [['Equities', 0.4], ['Credit', 0.6]], format: 'pct' })} />);
    expect(screen.getByText('Equities')).toBeTruthy();
    expect(screen.getByText('60.00%')).toBeTruthy();
  });

  it('button sends its click handler', () => {
    const sent = captureSent();
    render(() => <NodeView node={n('button', { label: 'Stress', 'on-click': { $h: 'r/b#on-click' } })} />);
    fireEvent.click(screen.getByText('Stress'));
    expect(sent.at(-1)).toEqual({ type: 'event', h: 'r/b#on-click', data: {} });
  });

  it('unknown element types render a visible placeholder', () => {
    render(() => <NodeView node={n('hologram')} />);
    expect(screen.getByText(/Unknown element: hologram/)).toBeTruthy();
  });

  it('Children renders every child in order', () => {
    render(() => (
      <Children node={n('column', {}, [n('text', { text: 'a' }, [], 'r/0'), n('text', { text: 'b' }, [], 'r/1')])} />
    ));
    expect(Array.from(document.querySelectorAll('.text')).map((e) => e.textContent)).toEqual(['a', 'b']);
  });
});
