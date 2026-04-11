import {
    TreeDataProvider,
    TreeItem,
    TreeItemCollapsibleState,
    EventEmitter,
    Event,
    debug,
    ThemeIcon,
} from 'vscode';

// ── Node types ────────────────────────────────────────────────────────────────

export type GCRootNode = RootCategoryNode | RootObjectNode;

export class RootCategoryNode {
    constructor(
        public readonly name: string,
        public readonly objectIds: number[],
        public readonly labels: string[],
    ) {}
}

export class RootObjectNode {
    constructor(
        public readonly objectId: number,
        public readonly label: string,
        public readonly parentName: string,
    ) {}
}

// ── Snapshot types ────────────────────────────────────────────────────────────

interface GCRoot {
    name: string;
    objectIds: number[];
    labels?: string[];
}

interface HeapSnapshot {
    totalBytes: number;
    softLimit: number;
    kinds: any[];
    roots: GCRoot[];
    consPool?: any;
}

// ── Tree data provider ───────────────────────────────────────────────────────

export class GCRootsTreeProvider implements TreeDataProvider<GCRootNode> {
    private _onDidChangeTreeData = new EventEmitter<GCRootNode | undefined | void>();
    readonly onDidChangeTreeData: Event<GCRootNode | undefined | void> = this._onDidChangeTreeData.event;

    private roots: GCRoot[] = [];

    refresh(): void {
        this.fetchSnapshot().then(() => {
            this._onDidChangeTreeData.fire();
        });
    }

    notifyStopped(): void {
        this.refresh();
    }

    private async fetchSnapshot(): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.roots = [];
            return;
        }
        try {
            const snap = await session.customRequest('eta/heapSnapshot') as HeapSnapshot;
            this.roots = snap.roots ?? [];
        } catch {
            this.roots = [];
        }
    }

    getTreeItem(element: GCRootNode): TreeItem {
        if (element instanceof RootCategoryNode) {
            const item = new TreeItem(
                `${element.name} (${element.objectIds.length})`,
                element.objectIds.length > 0
                    ? TreeItemCollapsibleState.Collapsed
                    : TreeItemCollapsibleState.None,
            );
            item.iconPath = new ThemeIcon('symbol-namespace');
            item.tooltip = `${element.name}: ${element.objectIds.length} heap object(s)`;
            return item;
        } else {
            const item = new TreeItem(element.label, TreeItemCollapsibleState.None);
            item.iconPath = new ThemeIcon('symbol-variable');
            item.description = `#${element.objectId}`;
            item.tooltip = `Object ID: ${element.objectId}\nRoot: ${element.parentName}`;
            item.command = {
                command: 'eta.inspectObjectFromTree',
                title: 'Inspect Object',
                arguments: [element.objectId],
            };
            return item;
        }
    }

    getChildren(element?: GCRootNode): GCRootNode[] {
        if (!element) {
            // Root level: one node per GC root category
            return this.roots
                .filter(r => r.objectIds.length > 0)
                .map(r => new RootCategoryNode(
                    r.name,
                    r.objectIds,
                    r.labels ?? [],
                ));
        }
        if (element instanceof RootCategoryNode) {
            const cap = Math.min(element.objectIds.length, 200);
            const children: RootObjectNode[] = [];
            for (let i = 0; i < cap; i++) {
                const oid = element.objectIds[i];
                const label = element.labels[i] ?? `Object #${oid}`;
                children.push(new RootObjectNode(oid, label, element.name));
            }
            return children;
        }
        return [];
    }
}

