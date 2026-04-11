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

export type GCRootNode = RootCategoryNode | RootObjectNode | ObjectFieldNode;

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

/** A child field of an inspected heap object (car/cdr, vector element, upvalue). */
export class ObjectFieldNode {
    constructor(
        public readonly fieldName: string,
        public readonly objectId: number,
        public readonly kind: string,
        public readonly preview: string,
    ) {}
}

// ── Inspection result type ────────────────────────────────────────────────────

interface InspectResult {
    objectId: number;
    kind: string;
    size: number;
    preview: string;
    children: Array<{
        objectId: number;
        kind: string;
        size: number;
        preview: string;
    }>;
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
        } else if (element instanceof RootObjectNode) {
            // Make root objects expandable — children loaded via eta/inspectObject
            const item = new TreeItem(element.label, TreeItemCollapsibleState.Collapsed);
            item.iconPath = new ThemeIcon('symbol-variable');
            item.description = `#${element.objectId}`;
            item.tooltip = `Object ID: ${element.objectId}\nRoot: ${element.parentName}\nClick to expand fields`;
            item.command = {
                command: 'eta.inspectObjectFromTree',
                title: 'Inspect Object',
                arguments: [element.objectId],
            };
            return item;
        } else {
            // ObjectFieldNode — expandable if it has a valid objectId
            const hasChildren = element.objectId > 0;
            const item = new TreeItem(
                element.fieldName,
                hasChildren ? TreeItemCollapsibleState.Collapsed : TreeItemCollapsibleState.None,
            );
            item.iconPath = new ThemeIcon('symbol-field');
            item.description = element.preview;
            item.tooltip = `${element.kind} #${element.objectId}: ${element.preview}`;
            if (hasChildren) {
                item.command = {
                    command: 'eta.inspectObjectFromTree',
                    title: 'Inspect Object',
                    arguments: [element.objectId],
                };
            }
            return item;
        }
    }

    getChildren(element?: GCRootNode): GCRootNode[] | Thenable<GCRootNode[]> {
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
        // RootObjectNode or ObjectFieldNode — drill down via eta/inspectObject
        if (element instanceof RootObjectNode || element instanceof ObjectFieldNode) {
            return this.inspectAndExpand(element.objectId);
        }
        return [];
    }

    private async inspectAndExpand(objectId: number): Promise<ObjectFieldNode[]> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') { return []; }
        try {
            const result = await session.customRequest('eta/inspectObject', { objectId }) as InspectResult;
            return (result.children ?? []).map((child, i) =>
                new ObjectFieldNode(
                    `[${i}]`,
                    child.objectId,
                    child.kind,
                    child.preview,
                ),
            );
        } catch {
            return [];
        }
    }
}
