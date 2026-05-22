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
import type { ChildProcessInfo, ChildProcessKind } from './dapTypes';

// ── Node type ─────────────────────────────────────────────────────────────────

export class ChildProcessNode {
    constructor(
        public readonly kind: ChildProcessKind,
        public readonly alive: boolean,
        public readonly pid?: number,
        public readonly endpoint?: string,
        public readonly modulePath?: string,
        public readonly actorPid?: string,
        public readonly registeredName?: string,
        public readonly mailboxLength?: number,
        public readonly state?: string,
        public readonly lastYieldReason?: string,
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
        if (element.kind === 'actor') {
            const actorPid = element.actorPid ?? '?';
            const state = element.state ?? (element.alive ? 'running' : 'exited');
            const label = element.registeredName && element.registeredName.length > 0
                ? element.registeredName
                : `actor ${actorPid}`;
            const item = new TreeItem(label, TreeItemCollapsibleState.None);
            item.description = `pid ${actorPid} (${state})`;
            item.tooltip =
                `Actor PID: ${actorPid}\n` +
                `Registered: ${element.registeredName && element.registeredName.length > 0 ? element.registeredName : '-'}\n` +
                `Mailbox: ${typeof element.mailboxLength === 'number' ? element.mailboxLength : '-'}\n` +
                `State: ${state}\n` +
                `Last Yield: ${element.lastYieldReason ?? '-'}`;
            item.iconPath = new ThemeIcon(
                element.alive ? 'symbol-interface' : 'circle-slash',
            );
            item.contextValue = element.alive ? 'childProcess.actor.alive' : 'childProcess.actor.dead';
            return item;
        }

        const pid = typeof element.pid === 'number' ? element.pid : -1;
        const modulePath = element.modulePath ?? '';
        const endpoint = element.endpoint ?? '';
        const label = modulePath
            ? modulePath.replace(/.*[\\/]/, '')  // basename
            : `pid ${pid}`;

        const item = new TreeItem(label, TreeItemCollapsibleState.None);
        item.description = element.alive ? `pid ${pid}` : `pid ${pid} (exited)`;
        item.tooltip =
            `PID: ${pid}\n` +
            `Endpoint: ${endpoint || '-'}\n` +
            `Module: ${modulePath || '-'}\n` +
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
        return this.children.map((c) => {
            const kind: ChildProcessKind = c.kind === 'actor' ? 'actor' : 'process';
            return new ChildProcessNode(
                kind,
                c.alive,
                c.pid,
                c.endpoint,
                c.modulePath,
                c.actorPid,
                c.registeredName,
                c.mailboxLength,
                c.state,
                c.lastYieldReason,
            );
        });
    }
}

