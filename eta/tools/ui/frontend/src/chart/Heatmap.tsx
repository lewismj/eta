import { For, Index, Show, createMemo, createSignal } from 'solid-js';
import type { UiNode } from '../model';
import { emitHandler, isHandler } from '../model';
import { formatNumber } from '../format';
import { currentTheme } from './color';
import {
  cellColor, cellInk, heatmapScale, legendGradient, legendTicks, parseHeatmap, validateHeatmap, valueAt,
} from './heatmapModel';

interface Hover {
  row: number;
  col: number;
  x: number;
  y: number;
}

/**
 * Heatmap as a CSS grid: crisp labels, real hit targets, accessible cells,
 * and colour changes that animate via CSS transitions. Rows are selectable
 * (linked to other charts through Eta state).
 */
export function Heatmap(props: { node: UiNode }) {
  let grid!: HTMLDivElement;
  const spec = createMemo(() => parseHeatmap(props.node.p as Record<string, unknown>));
  const problem = createMemo(() => validateHeatmap(spec()));
  const scale = createMemo(() => heatmapScale(spec(), currentTheme()));
  const [hover, setHover] = createSignal<Hover | null>(null);
  const handler = () => {
    const h = props.node.p['on-select'];
    return isHandler(h) ? h : null;
  };

  const select = (row: number, col: number) => {
    const h = handler();
    if (!h) return;
    const s = spec();
    emitHandler(h, { row, col, index: row, key: s.rows[row], column: s.cols[col] });
  };

  const onEnter = (row: number, col: number, ev: MouseEvent) => {
    const r = grid.getBoundingClientRect();
    const c = (ev.currentTarget as HTMLElement).getBoundingClientRect();
    setHover({ row, col, x: c.left - r.left + c.width / 2, y: c.top - r.top });
  };

  const title = () => (typeof props.node.p.title === 'string' ? props.node.p.title : '');
  const subtitle = () => (typeof props.node.p.subtitle === 'string' ? props.node.p.subtitle : '');

  return (
    <section class="plot heatmap" style={{ flex: 'none' }} data-testid="heatmap">
      <Show when={title() || subtitle()}>
        <header class="plot-head">
          <span class="plot-title">{title()}</span>
          <span class="plot-subtitle">{subtitle()}</span>
        </header>
      </Show>
      <Show when={!problem()} fallback={<div class="plot-fallback" role="alert">{problem()}</div>}>
        <div
          class="heatmap-grid"
          ref={grid}
          role="grid"
          aria-rowcount={spec().rows.length}
          aria-colcount={spec().cols.length}
          style={{ 'grid-template-columns': `max-content repeat(${spec().cols.length}, minmax(44px, 1fr))` }}
          onMouseLeave={() => setHover(null)}
        >
          <span class="heatmap-corner" />
          <For each={spec().cols}>{(c) => <span class="heatmap-col" role="columnheader">{c}</span>}</For>
          <Index each={spec().rows}>
            {(rowLabel, r) => (
              <>
                <span
                  class={spec().selectedRow === r ? 'heatmap-row selected' : 'heatmap-row'}
                  role="rowheader"
                  onClick={() => select(r, 0)}
                >
                  {rowLabel()}
                </span>
                <Index each={spec().cols}>
                  {(_, c) => {
                    const v = () => valueAt(spec(), r, c);
                    const fill = () => cellColor(scale(), v());
                    return (
                      <button
                        type="button"
                        class={spec().selectedRow === r ? 'heatmap-cell selected' : 'heatmap-cell'}
                        role="gridcell"
                        aria-label={`${rowLabel()} × ${spec().cols[c]}: ${formatNumber(v(), spec().format)}`}
                        data-row={r}
                        data-col={c}
                        style={{ background: fill(), color: cellInk(fill()) }}
                        onMouseEnter={(ev) => onEnter(r, c, ev)}
                        onClick={() => select(r, c)}
                      >
                        <Show when={spec().showValues}>{formatNumber(v(), spec().format)}</Show>
                      </button>
                    );
                  }}
                </Index>
              </>
            )}
          </Index>
          <Show when={hover()}>
            {(h) => (
              <div class="tooltip heatmap-tip" role="tooltip"
                style={{ left: `${h().x}px`, top: `${h().y}px`, transform: 'translate(-50%, calc(-100% - 8px))' }}>
                <div class="tooltip-title">{spec().rows[h().row]}</div>
                <div class="tooltip-row">
                  <span>{spec().cols[h().col]}</span>
                  <span class="num">{formatNumber(valueAt(spec(), h().row, h().col), spec().format)}</span>
                </div>
              </div>
            )}
          </Show>
        </div>
        <div class="heatmap-legend" aria-label="Colour scale">
          <div class="heatmap-legend-bar" style={{ background: legendGradient(scale()) }} />
          <div class="heatmap-legend-ticks">
            <For each={legendTicks(scale(), spec().format)}>
              {(t) => <span class="num" style={{ left: `${t.at * 100}%` }}>{t.label}</span>}
            </For>
          </div>
        </div>
      </Show>
    </section>
  );
}
