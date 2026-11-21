import { Component, For, JSX, Show } from 'solid-js';
import { Dynamic } from 'solid-js/web';
import type { UiNode } from './model';
import { emitHandler, isHandler, str } from './model';
import { formatValue } from './format';
import { Plot } from './chart/Plot';
import { Heatmap } from './chart/Heatmap';
import { Surface } from './chart/Surface';
import { Slider, Select, Toggle } from './inputs';
import { Table } from './Table';

type NodeProps = { node: UiNode };

export function Children(props: NodeProps) {
  return <For each={props.node.c}>{(child) => <NodeView node={child} />}</For>;
}

/** Common sizing props: 'width (px), 'flex (grow factor), 'height (px). */
function boxStyle(p: Record<string, unknown>): JSX.CSSProperties {
  const s: JSX.CSSProperties = {};
  if (typeof p.width === 'number') {
    s.width = `${p.width}px`;
    s.flex = 'none';
  }
  if (typeof p.height === 'number') s.height = `${p.height}px`;
  // 'flex 0 means "size to content".
  if (typeof p.flex === 'number') s.flex = p.flex === 0 ? 'none' : `${p.flex} 1 0`;
  return s;
}

const Row: Component<NodeProps> = (props) => (
  <div class="row" style={{ flex: '1 1 0', ...boxStyle(props.node.p) }}>
    <Children node={props.node} />
  </div>
);

const Column: Component<NodeProps> = (props) => (
  <div class="column" style={{ flex: '1 1 0', ...boxStyle(props.node.p) }}>
    <Children node={props.node} />
  </div>
);

const Panel: Component<NodeProps> = (props) => (
  <section class={props.node.t === 'card' ? 'card' : 'panel'} style={boxStyle(props.node.p)}>
    <Show when={str(props.node.p.title)}>
      <div class="panel-title">{str(props.node.p.title)}</div>
    </Show>
    <Children node={props.node} />
  </section>
);

const Heading: Component<NodeProps> = (props) => <h2 class="heading">{str(props.node.p.text)}</h2>;

const Text: Component<NodeProps> = (props) => (
  <p class={props.node.p.muted === true ? 'text muted' : 'text'}>{str(props.node.p.text)}</p>
);

const Stat: Component<NodeProps> = (props) => (
  <div class="stat">
    <span class="stat-label">{str(props.node.p.label)}</span>
    <span class="stat-value num">
      {formatValue(props.node.p.value, str(props.node.p.format, 'num'))}
    </span>
  </div>
);

const Kv: Component<NodeProps> = (props) => {
  const rows = () => (Array.isArray(props.node.p.rows) ? (props.node.p.rows as unknown[][]) : []);
  return (
    <dl class="kv">
      <For each={rows()}>
        {(r) => (
          <>
            <dt>{formatValue(r[0])}</dt>
            <dd>{formatValue(r[1], str(props.node.p.format, 'num'))}</dd>
          </>
        )}
      </For>
    </dl>
  );
};

const Button: Component<NodeProps> = (props) => (
  <button
    class={props.node.p.variant === 'primary' ? 'button primary' : 'button'}
    onClick={() => {
      const h = props.node.p['on-click'];
      if (isHandler(h)) emitHandler(h);
    }}
  >
    {str(props.node.p.label)}
  </button>
);

const Divider: Component<NodeProps> = () => <div class="divider" />;
const Spacer: Component<NodeProps> = () => <div class="spacer" />;

const Unknown: Component<NodeProps> = (props) => (
  <div class="text muted">Unknown element: {props.node.t}</div>
);

const registry: Record<string, Component<NodeProps>> = {
  row: Row,
  column: Column,
  panel: Panel,
  card: Panel,
  heading: Heading,
  text: Text,
  stat: Stat,
  kv: Kv,
  button: Button,
  divider: Divider,
  spacer: Spacer,
  plot: Plot,
  heatmap: Heatmap,
  surface: Surface,
  table: Table,
  slider: Slider,
  select: Select,
  toggle: Toggle,
};

export function NodeView(props: NodeProps) {
  return <Dynamic component={registry[props.node.t] ?? Unknown} node={props.node} />;
}
