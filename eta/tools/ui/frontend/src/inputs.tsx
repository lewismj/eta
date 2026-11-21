/**
 * Input elements: slider, select and toggle. Each sends its handler a
 * `{ value }` payload; std.ui unwraps it so Eta handlers receive the value.
 */

import { For, createEffect, createSignal, onCleanup } from 'solid-js';
import type { UiNode } from './model';
import { emitHandler, isHandler, str } from './model';
import { formatValue } from './format';
import { snapToStep, throttle } from './util';

type NodeProps = { node: UiNode };

const num = (v: unknown, d: number) => (typeof v === 'number' && Number.isFinite(v) ? v : d);

let inputSeq = 0;

function send(node: UiNode, prop: string, value: unknown): void {
  const h = node.p[prop];
  if (isHandler(h)) emitHandler(h, { value });
}

/** Live updates while dragging are throttled to this interval. */
export const SLIDER_THROTTLE_MS = 120;

export function Slider(props: NodeProps) {
  const id = `eta-slider-${++inputSeq}`;
  const min = () => num(props.node.p.min, 0);
  const max = () => num(props.node.p.max, 1);
  const step = () => num(props.node.p.step, (max() - min()) / 100);
  const [local, setLocal] = createSignal(num(props.node.p.value, min()));
  const [dragging, setDragging] = createSignal(false);

  // Follow the server value unless the user is mid-drag.
  createEffect(() => {
    const v = num(props.node.p.value, min());
    if (!dragging()) setLocal(v);
  });

  const live = throttle((v: number) => send(props.node, 'on-change', v), SLIDER_THROTTLE_MS);
  onCleanup(() => live.cancel());

  const onInput = (ev: InputEvent & { currentTarget: HTMLInputElement }) => {
    const v = snapToStep(Number(ev.currentTarget.value), min(), max(), step());
    setDragging(true);
    setLocal(v);
    if (props.node.p.live !== false) live(v);
  };
  const onCommit = (ev: Event & { currentTarget: HTMLInputElement }) => {
    const v = snapToStep(Number(ev.currentTarget.value), min(), max(), step());
    live.cancel();
    setLocal(v);
    setDragging(false);
    send(props.node, 'on-change', v);
  };

  const pct = () => ((local() - min()) / Math.max(1e-12, max() - min())) * 100;

  return (
    <div class="field slider">
      <div class="field-head">
        <label for={id} class="field-label">{str(props.node.p.label)}</label>
        <span class="field-value num" data-testid="slider-value">
          {formatValue(local(), str(props.node.p.format, 'num'))}
        </span>
      </div>
      <input
        id={id}
        type="range"
        min={min()}
        max={max()}
        step={step()}
        value={local()}
        style={{ '--fill': `${pct()}%` }}
        onInput={onInput}
        onChange={onCommit}
      />
    </div>
  );
}

/** Options are strings or [value, label] pairs. */
export function parseOptions(v: unknown): { value: string; label: string }[] {
  if (!Array.isArray(v)) return [];
  return v.map((o) => {
    if (Array.isArray(o)) return { value: String(o[0]), label: String(o[1] ?? o[0]) };
    return { value: String(o), label: String(o) };
  });
}

export function Select(props: NodeProps) {
  const id = `eta-select-${++inputSeq}`;
  const options = () => parseOptions(props.node.p.options);
  return (
    <div class="field select">
      <label for={id} class="field-label">{str(props.node.p.label)}</label>
      <select
        id={id}
        value={str(props.node.p.value)}
        onChange={(ev) => send(props.node, 'on-change', ev.currentTarget.value)}
      >
        <For each={options()}>{(o) => <option value={o.value}>{o.label}</option>}</For>
      </select>
    </div>
  );
}

export function Toggle(props: NodeProps) {
  const id = `eta-toggle-${++inputSeq}`;
  return (
    <div class="field toggle">
      <input
        id={id}
        type="checkbox"
        role="switch"
        checked={props.node.p.value === true}
        onChange={(ev) => send(props.node, 'on-change', ev.currentTarget.checked)}
      />
      <label for={id} class="field-label">{str(props.node.p.label)}</label>
    </div>
  );
}
