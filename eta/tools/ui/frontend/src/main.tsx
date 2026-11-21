import '@fontsource-variable/inter';
import './theme.css';

import { For, Show, createEffect } from 'solid-js';
import { createStore, reconcile } from 'solid-js/store';
import { render } from 'solid-js/web';
import { Connection, Message, Status } from './protocol';
import type { UiNode } from './model';
import { setSender, str } from './model';
import { Children } from './nodes';

interface Toast {
  id: number;
  title: string;
  message: string;
}

interface AppState {
  tree: UiNode | null;
  rev: number;
  status: Status;
  toasts: Toast[];
}

const [state, setState] = createStore<AppState>({
  tree: null,
  rev: 0,
  status: 'connecting',
  toasts: [],
});

let toastSeq = 0;
function pushToast(title: string, message: string): void {
  const id = ++toastSeq;
  setState('toasts', (ts) => [...ts, { id, title, message }].slice(-4));
  setTimeout(() => setState('toasts', (ts) => ts.filter((t) => t.id !== id)), 9000);
}

function onMessage(msg: Message): void {
  switch (msg.type) {
    case 'render': {
      const tree = msg.tree as UiNode;
      // Keyed by node id so unchanged nodes keep their components (and the
      // charts keep their GPU state to animate from).
      setState('tree', reconcile(tree, { key: 'id', merge: true }));
      setState('rev', Number(msg.rev) || 0);
      break;
    }
    case 'error':
      pushToast('Handler error', String(msg.message ?? ''));
      break;
    default:
      break;
  }
}

const connection = Connection.fromLocation({
  onMessage,
  onStatus: (s) => setState('status', s),
});
setSender((m) => connection.send(m));

const statusLabel: Record<Status, string> = {
  connecting: 'Connecting…',
  live: 'Live',
  reconnecting: 'Waiting for Eta…',
};

function App() {
  const title = () => str(state.tree?.p.title, 'Eta');
  createEffect(() => {
    document.title = title();
  });

  return (
    <div class="app">
      <header class="app-header">
        <span class="app-title">{title()}</span>
        <span class="app-brand">Eta</span>
        <span class="app-status" role="status">
          <span class={`status-dot ${state.status}`} />
          {statusLabel[state.status]}
        </span>
      </header>
      <main class="app-body">
        <Show when={state.tree} fallback={<div class="app-empty">Waiting for the Eta program…</div>}>
          {(tree) => <Children node={tree()} />}
        </Show>
      </main>
      <div class="toasts" aria-live="polite">
        <For each={state.toasts}>
          {(t) => (
            <div class="toast" role="alert">
              <div class="toast-title">{t.title}</div>
              {t.message}
            </div>
          )}
        </For>
      </div>
    </div>
  );
}

render(() => <App />, document.getElementById('root')!);
