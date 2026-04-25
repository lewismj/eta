/**
 * heapView.ts — Heap Inspector v2 (B3 of docs/dap_vs_plan.md).
 *
 * - Loads HTML/CSS/JS from `media/heap/` via webview.asWebviewUri.
 * - Emits a strict, nonce-based Content Security Policy.
 * - Supports baseline capture / snapshot diff (delegated to the webview).
 * - Implements "Find paths to root" by performing a BFS forward from every
 *   GC root listed in the most recent snapshot, calling `eta/inspectObject`
 *   on demand. Bounded by `MAX_BFS_NODES` to stay responsive on large heaps.
 */
import {
    window,
    debug,
    Uri,
    WebviewPanel,
    Webview,
    ViewColumn,
    ExtensionContext,
} from 'vscode';
import * as fs from 'fs';
import * as path from 'path';
import * as crypto from 'crypto';
import type {
    HeapSnapshot,
    ObjectInspection,
} from './dapTypes';

const MAX_BFS_NODES = 4000;
const MAX_PATHS = 5;
const MAX_UI_ROOT_OBJECTS = 750;

interface PathNode {
    objectId: number;
    kind: string;
    preview: string;
}
interface FoundPath {
    rootName: string;
    nodes: PathNode[];
}
interface PathsResult {
    paths: FoundPath[];
    visited: number;
    truncated: boolean;
    error?: string;
}

export class HeapInspectorPanel {
    public static readonly viewType = 'etaHeapInspector';

    private static instance: HeapInspectorPanel | undefined;
    private panel: WebviewPanel;
    private extensionUri: Uri;
    private snapshot: HeapSnapshot | undefined;

    private constructor(panel: WebviewPanel, extensionUri: Uri) {
        this.panel = panel;
        this.extensionUri = extensionUri;
        this.panel.webview.html = this.getWebviewHtml(panel.webview);

        panel.webview.onDidReceiveMessage(async (msg) => {
            switch (msg.command) {
                case 'refresh':
                    await this.refresh();
                    break;
                case 'inspectObject':
                    await this.inspectObject(msg.objectId);
                    break;
                case 'findPaths':
                    await this.findPaths(msg.objectId);
                    break;
            }
        });

        panel.onDidDispose(() => {
            HeapInspectorPanel.instance = undefined;
        });
    }

    public static createOrShow(ctx: ExtensionContext): HeapInspectorPanel {
        if (HeapInspectorPanel.instance) {
            HeapInspectorPanel.instance.panel.reveal(ViewColumn.Beside);
            return HeapInspectorPanel.instance;
        }
        const mediaRoot = Uri.joinPath(ctx.extensionUri, 'media');
        const panel = window.createWebviewPanel(
            HeapInspectorPanel.viewType,
            'Eta Heap Inspector',
            ViewColumn.Beside,
            {
                enableScripts: true,
                retainContextWhenHidden: false,
                localResourceRoots: [mediaRoot],
            },
        );
        HeapInspectorPanel.instance = new HeapInspectorPanel(panel, ctx.extensionUri);
        return HeapInspectorPanel.instance;
    }

    public async notifyStopped(): Promise<void> {
        await this.refresh();
    }

    public applySnapshot(snap: HeapSnapshot): void {
        this.snapshot = snap;
        this.panel.webview.postMessage({
            command: 'snapshot',
            data: this.trimSnapshotForUi(snap),
        });
    }

    public showIdle(text: string): void {
        this.panel.webview.postMessage({ command: 'idle', text });
    }

    public showError(text: string): void {
        this.panel.webview.postMessage({ command: 'error', text });
    }

    public async refresh(): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.showIdle('Start and pause an Eta debug session to inspect heap state.');
            return;
        }
        try {
            const snap = await session.customRequest('eta/heapSnapshot', {
                includeKinds: true,
                includeRoots: true,
                maxObjectsScanned: 120000,
                maxKindRows: 200,
                maxRootsPerCategory: 600,
            }) as HeapSnapshot;
            this.applySnapshot(snap);
        } catch (err: any) {
            const text = err?.message ?? String(err);
            if (/must be paused/i.test(text)) {
                this.showIdle('Pause the VM (breakpoint or step) to inspect the heap.');
                return;
            }
            this.showError(text);
        }
    }

    public async inspectObject(objectId: number): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') { return; }
        try {
            const obj = await session.customRequest(
                'eta/inspectObject', { objectId },
            ) as ObjectInspection;
            this.panel.webview.postMessage({ command: 'inspectResult', data: obj });
        } catch (err: any) {
            this.panel.webview.postMessage({
                command: 'error',
                text: err?.message ?? String(err),
            });
        }
    }

    /**
     * BFS forward from every GC root recorded in the latest snapshot,
     * memoising children via on-demand `eta/inspectObject` calls.
     * Returns up to MAX_PATHS shortest distinct paths whose terminus equals
     * `objectId`, or an empty list if none reachable within MAX_BFS_NODES.
     */
    public async findPaths(objectId: number): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta' || !this.snapshot) {
            this.panel.webview.postMessage({
                command: 'pathsResult',
                data: { paths: [], visited: 0, truncated: false,
                        error: 'No active snapshot — refresh first.' } satisfies PathsResult,
            });
            return;
        }

        const result: PathsResult = { paths: [], visited: 0, truncated: false };
        const childCache = new Map<number, ObjectInspection>();
        const fetchChildren = async (oid: number): Promise<ObjectInspection | undefined> => {
            const cached = childCache.get(oid);
            if (cached) return cached;
            try {
                const obj = await session.customRequest(
                    'eta/inspectObject', { objectId: oid },
                ) as ObjectInspection;
                childCache.set(oid, obj);
                return obj;
            } catch {
                return undefined;
            }
        };

        // ── Initialise BFS frontier from every (root, oid) pair ──────────
        type Step = { oid: number; rootName: string; parent?: Step };
        const frontier: Step[] = [];
        const visited = new Set<number>();
        for (const root of this.snapshot.roots) {
            for (const oid of root.objectIds) {
                if (visited.has(oid)) continue;
                visited.add(oid);
                frontier.push({ oid, rootName: root.name });
            }
        }

        const reconstruct = async (step: Step): Promise<FoundPath> => {
            const chain: Step[] = [];
            for (let cur: Step | undefined = step; cur; cur = cur.parent) {
                chain.push(cur);
            }
            chain.reverse();
            const nodes: PathNode[] = [];
            for (const c of chain) {
                const meta = await fetchChildren(c.oid);
                nodes.push({
                    objectId: c.oid,
                    kind: meta?.kind ?? 'unknown',
                    preview: meta?.preview ?? '',
                });
            }
            return { rootName: step.rootName, nodes };
        };

        // ── BFS ───────────────────────────────────────────────────────────
        while (frontier.length > 0 && result.paths.length < MAX_PATHS) {
            if (result.visited >= MAX_BFS_NODES) {
                result.truncated = true;
                break;
            }
            const step = frontier.shift()!;
            result.visited++;

            if (step.oid === objectId) {
                result.paths.push(await reconstruct(step));
                continue;
            }

            const meta = await fetchChildren(step.oid);
            if (!meta) continue;
            for (const child of meta.children) {
                if (visited.has(child.objectId)) continue;
                visited.add(child.objectId);
                frontier.push({
                    oid: child.objectId,
                    rootName: step.rootName,
                    parent: step,
                });
            }
        }

        this.panel.webview.postMessage({ command: 'pathsResult', data: result });
    }

    public static current(): HeapInspectorPanel | undefined {
        return HeapInspectorPanel.instance;
    }

    public isVisible(): boolean {
        return this.panel.visible;
    }

    public static disposeCurrent(): void {
        HeapInspectorPanel.instance?.panel.dispose();
    }

    private trimSnapshotForUi(snap: HeapSnapshot): HeapSnapshot {
        const roots = snap.roots.map((root) => {
            if (root.objectIds.length <= MAX_UI_ROOT_OBJECTS) {
                return root;
            }
            const trimmedIds = root.objectIds.slice(0, MAX_UI_ROOT_OBJECTS);
            const trimmedLabels = root.labels
                ? root.labels.slice(0, MAX_UI_ROOT_OBJECTS)
                : undefined;
            const uiRoot: any = {
                ...root,
                objectIds: trimmedIds,
                totalCount: root.objectIds.length,
                truncated: true,
            };
            if (trimmedLabels) {
                uiRoot.labels = trimmedLabels;
            }
            return uiRoot;
        });
        return {
            ...snap,
            roots,
        };
    }

    // ── HTML template loader (CSP + nonce) ──────────────────────────────
    private getWebviewHtml(webview: Webview): string {
        const mediaDir = Uri.joinPath(this.extensionUri, 'media', 'heap');
        const cssUri = webview.asWebviewUri(Uri.joinPath(mediaDir, 'heap.css'));
        const jsUri = webview.asWebviewUri(Uri.joinPath(mediaDir, 'heap.js'));
        const nonce = crypto.randomBytes(16).toString('base64');

        const htmlPath = path.join(mediaDir.fsPath, 'heap.html');
        let template: string;
        try {
            template = fs.readFileSync(htmlPath, 'utf8');
        } catch {
            return '<html lang="en"><body><pre>Failed to load heap.html bundle.</pre></body></html>';
        }
        return template
            .replace(/\{\{cspSource}}/g, webview.cspSource)
            .replace(/\{\{nonce}}/g, nonce)
            .replace(/\{\{cssUri}}/g, cssUri.toString())
            .replace(/\{\{jsUri}}/g, jsUri.toString());
    }
}
