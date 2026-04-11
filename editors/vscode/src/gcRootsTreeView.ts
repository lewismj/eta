import {
    TreeDataProvider,
    TreeItem,
    TreeItemCollapsibleState,
    EventEmitter,
    Event,
    debug,
    ThemeIcon,
} from 'vscode';
import type { GCRoot, HeapSnapshot, ObjectInspection } from './dapTypes';

// ── Node types ────────────────────────────────────────────────────────────────

export type GCRootNode = RootCategoryNode | ModuleGroupNode | RootObjectNode | ObjectFieldNode;

export class RootCategoryNode {
    constructor(
        public readonly name: string,
        public readonly objectIds: number[],
        public readonly labels: string[],
    ) {}
}

/** Intermediate grouping node: one module under the Globals root. */
export class ModuleGroupNode {
    constructor(
        public readonly moduleName: string,
        public readonly items: Array<{ oid: number; label: string }>,
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


// ── Helpers ───────────────────────────────────────────────────────────────────

/** Split "module.symbol" → ["module", "symbol"].
 *  Labels without a dot go into the "(top-level)" group. */
function splitLabel(label: string): { mod: string; sym: string } {
    const dot = label.lastIndexOf('.');
    if (dot > 0) {
        return { mod: label.substring(0, dot), sym: label.substring(dot + 1) };
    }
    return { mod: '(top-level)', sym: label };
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
        } else if (element instanceof ModuleGroupNode) {
            const item = new TreeItem(
                `${element.moduleName} (${element.items.length})`,
                TreeItemCollapsibleState.Collapsed,
            );
            item.iconPath = new ThemeIcon('symbol-module');
            item.tooltip = `Module: ${element.moduleName} — ${element.items.length} global(s)`;
            return item;
        } else if (element instanceof RootObjectNode) {
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
                .map(r => new RootCategoryNode(r.name, r.objectIds, r.labels ?? []));
        }

        if (element instanceof RootCategoryNode) {
            // If we have module-qualified labels (contain a dot), group by module.
            if (element.labels.length > 0 && element.labels.some(l => l.includes('.'))) {
                return this.buildModuleGroups(element);
            }
            // Flat list for non-Globals roots (no module prefix available).
            return element.objectIds.map((oid, i) => {
                const label = element.labels[i] ?? `Object #${oid}`;
                return new RootObjectNode(oid, label, element.name);
            });
        }

        if (element instanceof ModuleGroupNode) {
            // All items in this module group — no cap.
            return element.items.map(it =>
                new RootObjectNode(it.oid, it.label, element.moduleName),
            );
        }

        // RootObjectNode or ObjectFieldNode — drill down via eta/inspectObject
        return this.inspectAndExpand(element.objectId);
    }

    // ── Private ───────────────────────────────────────────────────────────────

    /** Group all labeled items into ModuleGroupNodes, sorted alphabetically
     *  with "(top-level)" last. */
    private buildModuleGroups(cat: RootCategoryNode): ModuleGroupNode[] {
        const groups = new Map<string, Array<{ oid: number; label: string; sym: string }>>();

        for (let i = 0; i < cat.objectIds.length; i++) {
            const oid = cat.objectIds[i];
            const rawLabel = cat.labels[i] ?? `Object #${oid}`;
            const { mod, sym } = splitLabel(rawLabel);
            if (!groups.has(mod)) { groups.set(mod, []); }
            groups.get(mod)!.push({ oid, label: rawLabel, sym });
        }

        return [...groups.entries()]
            .sort(([a], [b]) => {
                if (a === '(top-level)') { return 1; }
                if (b === '(top-level)') { return -1; }
                return a.localeCompare(b);
            })
            .map(([mod, items]) => {
                return new ModuleGroupNode(mod, items.map(it => ({ oid: it.oid, label: it.sym })));
            });
    }

    private async inspectAndExpand(objectId: number): Promise<ObjectFieldNode[]> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') { return []; }
        try {
            const result = await session.customRequest('eta/inspectObject', { objectId }) as ObjectInspection;
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
