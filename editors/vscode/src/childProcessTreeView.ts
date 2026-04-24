// Tree data provider for spawned child Eta processes (Phase 4 actor model).
// Polls eta/childProcesses on each debug stop event and rebuilds the tree.

import {
    TreeDataProvider,
    TreeItem,
    TreeItemCollapsibleState,
    EventEmitter,
    Event,
    debug,
    ThemeIcon,
} from 'vscode';
import type { ChildProcessInfo } from './dapTypes';

// ── Node type ─────────────────────────────────────────────────────────────────

export class ChildProcessNode {
    constructor(
        public readonly pid: number,
        public readonly endpoint: string,
        public readonly modulePath: string,
        public readonly alive: boolean,
    ) {}
}

// ── Tree data provider ────────────────────────────────────────────────────────

export class ChildProcessTreeProvider implements TreeDataProvider<ChildProcessNode> {
    private _onDidChangeTreeData = new EventEmitter<ChildProcessNode | undefined | void>();
    readonly onDidChangeTreeData: Event<ChildProcessNode | undefined | void> =
        this._onDidChangeTreeData.event;

    private children: ChildProcessInfo[] = [];

    /** Called by EtaDebugAdapterTracker on every 'stopped' event. */
    notifyStopped(): void {
        this.fetchChildren().then(() => {
            this._onDidChangeTreeData.fire();
        });
    }

    /** Manual refresh (toolbar button). */
    refresh(): void {
        this.notifyStopped();
    }

    updateChildren(children: ChildProcessInfo[]): void {
        this.children = children ?? [];
        this._onDidChangeTreeData.fire();
    }

    /** Called when the debug session ends — clear the list. */
    notifySessionEnded(): void {
        this.children = [];
        this._onDidChangeTreeData.fire();
    }

    private async fetchChildren(): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.children = [];
            return;
        }
        try {
            const result = await session.customRequest('eta/childProcesses') as
                { children: ChildProcessInfo[] };
            this.children = result.children ?? [];
        } catch {
            this.children = [];
        }
    }

    getTreeItem(element: ChildProcessNode): TreeItem {
        const label = element.modulePath
            ? element.modulePath.replace(/.*[\\/]/, '')  // basename
            : `pid ${element.pid}`;

        const item = new TreeItem(label, TreeItemCollapsibleState.None);

        item.description = element.alive ? `pid ${element.pid}` : `pid ${element.pid} (exited)`;
        item.tooltip =
            `PID: ${element.pid}\n` +
            `Endpoint: ${element.endpoint}\n` +
            `Module: ${element.modulePath}\n` +
            `Status: ${element.alive ? 'running' : 'exited'}`;
        item.iconPath = new ThemeIcon(
            element.alive ? 'server-process' : 'circle-slash',
        );
        item.contextValue = element.alive ? 'childProcess.alive' : 'childProcess.dead';

        return item;
    }

    getChildren(element?: ChildProcessNode): ChildProcessNode[] {
        if (element) {
            return []; // leaf nodes only
        }
        if (this.children.length === 0) {
            return [];
        }
        return this.children.map(
            c => new ChildProcessNode(c.pid, c.endpoint, c.modulePath, c.alive),
        );
    }
}

