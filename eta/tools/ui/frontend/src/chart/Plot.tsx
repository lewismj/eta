import { For, Show, createEffect, createMemo, createSignal, on, onCleanup, onMount, untrack } from 'solid-js';
import type { UiNode } from '../model';
import { emitHandler } from '../model';
import { currentTheme, seriesColor } from './color';
import { PlotEngine } from './engine';
import { isFullX, panX, wheelFactor, zoomX } from './interact';
import { bandCategories, legendEntries, parseMarks } from './marks';
import {
  ROW_HEIGHT, TooltipContent, barAt, barTooltip, crosshairAt, leftMargin, plotDomain, pointTooltip, xTicks, yTicks,
} from './plotModel';
import { AxisSpec, Domain } from './scale';

interface TipState {
  x: number;
  y: number;
  content: TooltipContent;
}

interface Crosshair {
  px: number;
  dots: { py: number; slot: number }[];
}

function axisSpec(v: unknown): AxisSpec {
  const node = v as UiNode | undefined;
  if (!node || typeof node !== 'object' || !node.p) return {};
  const p = node.p as Record<string, unknown>;
  return {
    label: typeof p.label === 'string' ? p.label : undefined,
    format: typeof p.format === 'string' ? p.format : undefined,
    min: typeof p.min === 'number' ? p.min : undefined,
    max: typeof p.max === 'number' ? p.max : undefined,
    zero: p.zero === true,
  };
}

export function Plot(props: { node: UiNode }) {
  let area!: HTMLDivElement;
  let canvas!: HTMLCanvasElement;
  let engine: PlotEngine | null = null;

  const [size, setSize] = createSignal({ w: 1, h: 1 });
  const [domain, setDomain] = createSignal<Domain>({ x0: 0, x1: 1, y0: 0, y1: 1 });
  const [view, setView] = createSignal<Domain | null>(null);
  const [tip, setTip] = createSignal<TipState | null>(null);
  const [cross, setCross] = createSignal<Crosshair | null>(null);
  const [failed, setFailed] = createSignal<string | null>(null);

  const xSpec = createMemo(() => axisSpec(props.node.p.x));
  const ySpec = createMemo(() => axisSpec(props.node.p.y));
  const marks = createMemo(() => parseMarks(props.node.c ?? []));
  const categories = createMemo(() => bandCategories(marks()));
  const full = createMemo(() => plotDomain(marks(), xSpec(), ySpec(), size()));
  const target = () => view() ?? full();
  const margins = createMemo(() => ({
    left: leftMargin(marks(), !!ySpec().label),
    right: 16,
    top: 10,
    bottom: xSpec().label ? 40 : 28,
  }));
  const legend = createMemo(() => legendEntries(marks()));
  const zoomable = () => !categories();

  onMount(() => {
    try {
      engine = new PlotEngine(canvas);
    } catch (err) {
      setFailed(String(err));
      return;
    }
    engine.init.catch((err) => setFailed(`GPU rendering unavailable: ${err}`));
    engine.onFrame = (d) => setDomain(d);

    const ro = new ResizeObserver(() => {
      const r = area.getBoundingClientRect();
      setSize({ w: r.width, h: r.height });
      engine?.resize(r.width, r.height);
    });
    ro.observe(area);

    const scheme = window.matchMedia('(prefers-color-scheme: dark)');
    const onScheme = () => engine?.setTheme(currentTheme());
    scheme.addEventListener('change', onScheme);

    onCleanup(() => {
      ro.disconnect();
      scheme.removeEventListener('change', onScheme);
      engine?.dispose();
      engine = null;
    });
  });

  createEffect(() => engine?.setMargins(margins()));

  // New data (or a new full domain): retarget marks, keep any zoom window.
  createEffect(on([marks, full], ([ms, f]) => {
    const v = untrack(view);
    const next = v && !isFullX(v, f) ? zoomX({ ...f, x0: v.x0, x1: v.x1 }, 0.5, 1, f) : null;
    setView(next);
    engine?.setMarks(ms, next ?? f);
    setTip(null);
    setCross(null);
  }));

  const toPx = (x: number) => {
    const d = domain(), m = margins();
    return m.left + ((x - d.x0) / (d.x1 - d.x0)) * (size().w - m.left - m.right);
  };
  const toPy = (y: number) => {
    const d = domain(), m = margins();
    return size().h - (m.bottom + ((y - d.y0) / (d.y1 - d.y0)) * (size().h - m.top - m.bottom));
  };
  const inside = (v: number, a: number, b: number) => {
    const lo = Math.min(a, b), hi = Math.max(a, b), eps = (hi - lo) * 1e-6;
    return v >= lo - eps && v <= hi + eps;
  };
  const xTickList = createMemo(() => xTicks(target(), size(), xSpec().format ?? 'num'));
  const yTickList = createMemo(() => yTicks(target(), size(), ySpec().format ?? 'num', categories()));
  const visibleX = () => xTickList().filter((t) => inside(t.value, domain().x0, domain().x1));
  const visibleY = () => yTickList().filter((t) => inside(t.value, domain().y0, domain().y1));

  // ---------------------------------------------------------- interaction

  let drag: { x: number; view: Domain } | null = null;

  const local = (ev: MouseEvent) => {
    const r = area.getBoundingClientRect();
    return [ev.clientX - r.left, ev.clientY - r.top] as const;
  };

  const onMove = (ev: PointerEvent) => {
    if (!engine) return;
    const [mx, my] = local(ev);
    if (drag) {
      const m = margins();
      const frac = (mx - drag.x) / Math.max(1, size().w - m.left - m.right);
      const next = panX(drag.view, frac, full());
      setView(next);
      engine.setDomain(next, 0);
      return;
    }
    const [dx, dy] = engine.fromPixels(mx, my);
    const ms = marks();

    if (categories()) {
      const hit = barAt(ms, dy);
      engine.setBarHover(hit ? hit.mark.categories[hit.index] : null);
      area.style.cursor = hit && hit.mark.onSelect ? 'pointer' : 'default';
      setTip(hit ? { x: mx, y: toPy(hit.index), content: barTooltip(hit.mark, hit.index, xSpec()) } : null);
      return;
    }

    const point = engine.pickPoint(mx, my);
    engine.setPointHover(point);
    if (point) {
      const m = ms.find((k) => k.id === point.layerId);
      if (m && m.kind === 'points') {
        area.style.cursor = m.onSelect ? 'pointer' : 'crosshair';
        setCross(null);
        setTip({ x: point.px, y: point.py, content: pointTooltip(m, point.dataIndex, point.key, xSpec(), ySpec()) });
        return;
      }
    }
    area.style.cursor = 'crosshair';
    const ch = crosshairAt(ms, dx, xSpec(), ySpec());
    if (ch && inside(ch.x, domain().x0, domain().x1)) {
      const px = toPx(ch.x);
      setCross({ px, dots: ch.points.map((p) => ({ py: toPy(p.y), slot: p.slot })) });
      setTip({ x: px, y: Math.min(...ch.points.map((p) => toPy(p.y)), size().h / 2), content: ch.content });
    } else {
      setCross(null);
      setTip(null);
    }
  };

  const onLeave = () => {
    drag = null;
    engine?.setPointHover(null);
    engine?.setBarHover(null);
    setTip(null);
    setCross(null);
  };

  const onDown = (ev: PointerEvent) => {
    if (!zoomable() || ev.button !== 0 || !view()) return;
    const [mx] = local(ev);
    drag = { x: mx, view: view()! };
    area.setPointerCapture(ev.pointerId);
  };
  const onUp = (ev: PointerEvent) => {
    if (drag) area.releasePointerCapture(ev.pointerId);
    drag = null;
  };

  const onWheel = (ev: WheelEvent) => {
    if (!engine || !zoomable()) return;
    ev.preventDefault();
    const [mx] = local(ev);
    const m = margins();
    const anchor = (mx - m.left) / Math.max(1, size().w - m.left - m.right);
    const next = zoomX(target(), anchor, wheelFactor(ev.deltaY), full());
    const reset = isFullX(next, full());
    setView(reset ? null : next);
    engine.setDomain(reset ? full() : next);
  };

  const onDblClick = () => {
    if (!view()) return;
    setView(null);
    engine?.setDomain(full());
  };

  const onClick = (ev: MouseEvent) => {
    if (!engine) return;
    const [mx, my] = local(ev);
    if (categories()) {
      const hit = barAt(marks(), engine.fromPixels(mx, my)[1]);
      if (hit?.mark.onSelect) emitHandler(hit.mark.onSelect, { index: hit.index, key: hit.mark.categories[hit.index] });
      return;
    }
    const point = engine.pickPoint(mx, my);
    if (!point) return;
    const m = marks().find((k) => k.id === point.layerId);
    if (m?.onSelect) emitHandler(m.onSelect, { index: point.dataIndex, key: point.key });
  };

  // ---------------------------------------------------------------- render

  const tipStyle = () => {
    const t = tip();
    if (!t) return {};
    const flip = t.x > size().w * 0.6;
    return {
      left: `${flip ? t.x - 14 : t.x + 14}px`,
      top: `${Math.max(4, t.y - 12)}px`,
      transform: flip ? 'translateX(-100%)' : 'none',
    };
  };

  const title = () => (typeof props.node.p.title === 'string' ? props.node.p.title : '');
  const subtitle = () => (typeof props.node.p.subtitle === 'string' ? props.node.p.subtitle : '');
  const style = () => {
    const s: Record<string, string> = {};
    const cats = categories();
    if (typeof props.node.p.height === 'number') s.height = `${props.node.p.height}px`;
    else if (cats) s.height = `${cats.length * ROW_HEIGHT + 90}px`;
    if (typeof props.node.p.flex === 'number') s.flex = props.node.p.flex === 0 ? 'none' : `${props.node.p.flex} 1 0`;
    else if (!s.height) s.flex = '1 1 0';
    else s.flex = 'none';
    return s;
  };

  return (
    <section class="plot" style={style()} data-testid="plot">
      <Show when={title() || subtitle() || legend().length > 1}>
        <header class="plot-head">
          <span class="plot-title">{title()}</span>
          <span class="plot-subtitle">{subtitle()}</span>
          <Show when={legend().length > 1}>
            <ul class="legend" aria-label="Legend">
              <For each={legend()}>
                {(e) => (
                  <li class="legend-item">
                    <span class="legend-swatch" style={{ background: seriesColor(e.slot, currentTheme()) }} />
                    {e.label}
                  </li>
                )}
              </For>
            </ul>
          </Show>
        </header>
      </Show>
      <div
        class="plot-area"
        ref={area}
        onPointerMove={onMove}
        onPointerLeave={onLeave}
        onPointerDown={onDown}
        onPointerUp={onUp}
        onWheel={onWheel}
        onDblClick={onDblClick}
        onClick={onClick}
      >
        <svg aria-hidden="true">
          <Show when={!categories()}>
            <For each={visibleY()}>
              {(t) => (
                <line x1={margins().left} x2={size().w - margins().right} y1={toPy(t.value)} y2={toPy(t.value)}
                  stroke={t.value === 0 ? 'var(--axis)' : 'var(--grid)'} stroke-width="1" shape-rendering="crispEdges" />
              )}
            </For>
          </Show>
          <For each={visibleX()}>
            {(t) => (
              <line x1={toPx(t.value)} x2={toPx(t.value)} y1={margins().top} y2={size().h - margins().bottom}
                stroke={t.value === 0 ? 'var(--axis)' : 'var(--grid)'} stroke-width="1" shape-rendering="crispEdges" />
            )}
          </For>
          <line x1={margins().left} x2={size().w - margins().right} y1={size().h - margins().bottom}
            y2={size().h - margins().bottom} stroke="var(--axis)" stroke-width="1" shape-rendering="crispEdges" />
          <Show when={cross()}>
            {(c) => (
              <line class="crosshair" x1={c().px} x2={c().px} y1={margins().top} y2={size().h - margins().bottom}
                stroke="var(--text-muted)" stroke-width="1" stroke-dasharray="3 3" shape-rendering="crispEdges" />
            )}
          </Show>
        </svg>
        <canvas ref={canvas} />
        <div class="plot-overlay">
          <For each={visibleX()}>
            {(t) => (
              <span class="tick x" style={{ left: `${toPx(t.value)}px`, top: `${size().h - margins().bottom + 6}px` }}>
                {t.label}
              </span>
            )}
          </For>
          <For each={visibleY()}>
            {(t) => (
              <span class={categories() ? 'tick y category' : 'tick y'} style={{ left: `${margins().left}px`, top: `${toPy(t.value)}px` }}>
                {t.label}
              </span>
            )}
          </For>
          <Show when={xSpec().label}>
            <span class="axis-title x" style={{ left: `${margins().left + (size().w - margins().left - margins().right) / 2}px`, top: `${size().h - 16}px` }}>
              {xSpec().label}
            </span>
          </Show>
          <Show when={ySpec().label && !categories()}>
            <span class="axis-title y" style={{ left: '0px', top: `${margins().top + (size().h - margins().top - margins().bottom) / 2}px` }}>
              {ySpec().label}
            </span>
          </Show>
          <Show when={cross()}>
            {(c) => (
              <For each={c().dots}>
                {(d) => (
                  <span class="crosshair-dot" style={{ left: `${c().px}px`, top: `${d.py}px`, background: seriesColor(d.slot, currentTheme()) }} />
                )}
              </For>
            )}
          </Show>
          <Show when={view()}>
            <span class="zoom-hint">Double-click to reset zoom</span>
          </Show>
        </div>
        <Show when={tip()}>
          {(t) => (
            <div class="tooltip" style={tipStyle()} role="tooltip">
              <div class="tooltip-title">
                <Show when={t().content.slot !== undefined}>
                  <span class="tooltip-swatch" style={{ background: seriesColor(t().content.slot ?? 0, currentTheme()) }} />
                </Show>
                {t().content.title}
              </div>
              <For each={t().content.rows}>
                {(r) => (
                  <div class="tooltip-row">
                    <span>
                      <Show when={r.slot !== undefined}>
                        <span class="tooltip-swatch inline" style={{ background: seriesColor(r.slot ?? 0, currentTheme()) }} />
                      </Show>
                      {r.label}
                    </span>
                    <span class="num">{r.value}</span>
                  </div>
                )}
              </For>
            </div>
          )}
        </Show>
        <Show when={failed()}>
          <div class="plot-fallback">{failed()}</div>
        </Show>
      </div>
    </section>
  );
}
