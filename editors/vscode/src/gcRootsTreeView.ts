import {
    debug,
    Event,
    EventEmitter,
    ThemeIcon,
    TreeDataProvider,
    TreeItem,
    TreeItemCollapsibleState,
} from 'vscode';
import type { DebugVariable, LocalMemorySnapshot } from './dapTypes';

export type MemoryNode = MemorySectionNode | MemoryVariableNode | MemoryMessageNode;

export class MemorySectionNode {
    constructor(
        public readonly title: string,
        public readonly total: number,
        public readonly truncated: boolean,
        public readonly variables: DebugVariable[],
    ) {}
}

export class MemoryVariableNode {
    constructor(public readonly variable: DebugVariable) {}
}

export class MemoryMessageNode {
    constructor(public readonly text: string) {}
}

export class GCRootsTreeProvider implements TreeDataProvider<MemoryNode> {
    private readonly _onDidChangeTreeData = new EventEmitter<MemoryNode | undefined | void>();
    readonly onDidChangeTreeData: Event<MemoryNode | undefined | void> = this._onDidChangeTreeData.event;

    private snapshot: LocalMemorySnapshot | undefined;
    private statusText = 'Pause an Eta debug session to inspect frame memory.';

    applyLocalMemory(snapshot: LocalMemorySnapshot | undefined): void {
        this.snapshot = snapshot;
        if (snapshot) {
            this.statusText = `Frame ${snapshot.frameIndex}: ${snapshot.frameName || '<anonymous>'}`;
        } else if (debug.activeDebugSession?.type === 'eta') {
            this.statusText = 'No paused Eta frame available.';
        } else {
            this.statusText = 'Pause an Eta debug session to inspect frame memory.';
        }
        this._onDidChangeTreeData.fire();
    }

    refresh(): void {
        this.fetchLocalMemory().then(() => this._onDidChangeTreeData.fire());
    }

    notifyStopped(): void {
        this.refresh();
    }

    private async fetchLocalMemory(): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.snapshot = undefined;
            this.statusText = 'Pause an Eta debug session to inspect frame memory.';
            return;
        }
        try {
            const snap = await session.customRequest('eta/localMemory', {
                frameIndex: 0,
                includeModuleGlobals: true,
                maxLocals: 200,
                maxUpvalues: 200,
                maxModuleGlobals: 200,
            }) as LocalMemorySnapshot;
            this.snapshot = snap;
            this.statusText = `Frame ${snap.frameIndex}: ${snap.frameName || '<anonymous>'}`;
        } catch (err: any) {
            this.snapshot = undefined;
            const text = err?.message ?? String(err);
            this.statusText = `Memory unavailable: ${text}`;
        }
    }

    getTreeItem(element: MemoryNode): TreeItem {
        if (element instanceof MemorySectionNode) {
            const item = new TreeItem(element.title, TreeItemCollapsibleState.Collapsed);
            item.iconPath = new ThemeIcon('symbol-namespace');
            item.description = element.truncated
                ? `${element.variables.length}/${element.total}`
                : `${element.total}`;
            item.tooltip = element.truncated
                ? `${element.title}: showing ${element.variables.length} of ${element.total}`
                : `${element.title}: ${element.total}`;
            return item;
        }

        if (element instanceof MemoryVariableNode) {
            const v = element.variable;
            const hasChildren = (v.variablesReference ?? 0) > 0;
            const item = new TreeItem(
                v.name,
                hasChildren ? TreeItemCollapsibleState.Collapsed : TreeItemCollapsibleState.None,
            );
            item.iconPath = new ThemeIcon(hasChildren ? 'symbol-object' : 'symbol-variable');
            item.description = v.value;
            item.tooltip = `${v.name} = ${v.value}`;
            return item;
        }

        const item = new TreeItem(element.text, TreeItemCollapsibleState.None);
        item.iconPath = new ThemeIcon('info');
        return item;
    }

    getChildren(element?: MemoryNode): MemoryNode[] | Thenable<MemoryNode[]> {
        if (!element) {
            return this.rootNodes();
        }
        if (element instanceof MemorySectionNode) {
            return element.variables.map(v => new MemoryVariableNode(v));
        }
        if (element instanceof MemoryVariableNode) {
            return this.expandVariable(element.variable);
        }
        return [];
    }

    private rootNodes(): MemoryNode[] {
        const snap = this.snapshot;
        if (!snap) {
            return [new MemoryMessageNode(this.statusText)];
        }

        const moduleTitle = snap.moduleName
            ? `Module Globals (${snap.moduleName})`
            : 'Module Globals';

        return [
            new MemorySectionNode('Locals', snap.localsTotal, snap.localsTruncated, snap.locals ?? []),
            new MemorySectionNode('Upvalues', snap.upvaluesTotal, snap.upvaluesTruncated, snap.upvalues ?? []),
            new MemorySectionNode(
                moduleTitle,
                snap.moduleGlobalsTotal,
                snap.moduleGlobalsTruncated,
                snap.moduleGlobals ?? [],
            ),
        ];
    }

    private async expandVariable(variable: DebugVariable): Promise<MemoryNode[]> {
        const ref = variable.variablesReference ?? 0;
        if (ref <= 0) {
            return [];
        }
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            return [new MemoryMessageNode('No active Eta debug session.')];
        }

        try {
            const resp = await session.customRequest('variables', {
                variablesReference: ref,
                start: 0,
                count: 0,
            }) as { variables?: DebugVariable[] };
            const children = Array.isArray(resp?.variables) ? resp.variables : [];
            return children.map(v => new MemoryVariableNode(v));
        } catch (err: any) {
            const text = err?.message ?? String(err);
            return [new MemoryMessageNode(`Failed to expand: ${text}`)];
        }
    }
}
