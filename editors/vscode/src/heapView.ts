import {
    window,
    debug,
    WebviewPanel,
    ViewColumn,
    ExtensionContext,
} from 'vscode';

// ── Heap snapshot types (mirror DAP response) ────────────────────────────────

interface KindStat {
    kind: string;
    count: number;
    bytes: number;
}

interface GCRoot {
    name: string;
    objectIds: number[];
}

interface HeapSnapshot {
    totalBytes: number;
    softLimit: number;
    kinds: KindStat[];
    roots: GCRoot[];
}

interface ObjectChild {
    objectId: number;
    kind: string;
    size: number;
    preview: string;
}

interface ObjectInspection {
    objectId: number;
    kind: string;
    size: number;
    preview: string;
    children: ObjectChild[];
}

// ── HeapInspectorPanel ───────────────────────────────────────────────────────

export class HeapInspectorPanel {
    public static readonly viewType = 'etaHeapInspector';

    private static instance: HeapInspectorPanel | undefined;
    private panel: WebviewPanel;
    private snapshot: HeapSnapshot | undefined;

    private constructor(panel: WebviewPanel) {
        this.panel = panel;

        panel.webview.onDidReceiveMessage(async (msg) => {
            switch (msg.command) {
                case 'refresh':
                    await this.refresh();
                    break;
                case 'inspectObject':
                    await this.inspectObject(msg.objectId);
                    break;
            }
        });

        panel.onDidDispose(() => {
            HeapInspectorPanel.instance = undefined;
        });
    }

    /** Show the panel (or reveal if already open). */
    public static createOrShow(_context: ExtensionContext): HeapInspectorPanel {
        if (HeapInspectorPanel.instance) {
            HeapInspectorPanel.instance.panel.reveal(ViewColumn.Beside);
            return HeapInspectorPanel.instance;
        }

        const panel = window.createWebviewPanel(
            HeapInspectorPanel.viewType,
            'Eta Heap Inspector',
            ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true },
        );

        HeapInspectorPanel.instance = new HeapInspectorPanel(panel);
        return HeapInspectorPanel.instance;
    }

    /** Called when the VM stops (breakpoint / step). */
    public async notifyStopped(): Promise<void> {
        await this.refresh();
    }

    /** Request a fresh heap snapshot from the debug adapter. */
    public async refresh(): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.panel.webview.postMessage({ command: 'error', text: 'No active Eta debug session.' });
            return;
        }

        try {
            const snap = await session.customRequest('eta/heapSnapshot') as HeapSnapshot;
            this.snapshot = snap;
            this.panel.webview.postMessage({ command: 'snapshot', data: snap });
        } catch (err: any) {
            this.panel.webview.postMessage({ command: 'error', text: err?.message ?? String(err) });
        }
    }

    /** Drill into a specific heap object. */
    public async inspectObject(objectId: number): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') { return; }

        try {
            const obj = await session.customRequest('eta/inspectObject', { objectId }) as ObjectInspection;
            this.panel.webview.postMessage({ command: 'inspectResult', data: obj });
        } catch (err: any) {
            this.panel.webview.postMessage({ command: 'error', text: err?.message ?? String(err) });
        }
    }

    /** Update the HTML when the panel is first shown. */
    public setInitialHtml(): void {
        this.panel.webview.html = getWebviewHtml();
    }

    /** Expose the underlying panel for subscription cleanup. */
    public get webviewPanel(): WebviewPanel {
        return this.panel;
    }

    /** Get the singleton (if any). */
    public static current(): HeapInspectorPanel | undefined {
        return HeapInspectorPanel.instance;
    }
}

// ── Static HTML ──────────────────────────────────────────────────────────────

function getWebviewHtml(): string {
    return /*html*/ `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Eta Heap Inspector</title>
<style>
    :root {
        --bg: var(--vscode-editor-background);
        --fg: var(--vscode-editor-foreground);
        --border: var(--vscode-panel-border, #444);
        --accent: var(--vscode-focusBorder, #007acc);
        --badge: var(--vscode-badge-background, #4d4d4d);
        --badge-fg: var(--vscode-badge-foreground, #fff);
        --bar-bg: var(--vscode-progressBar-background, #0e70c0);
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
        font-family: var(--vscode-font-family, 'Segoe UI', sans-serif);
        font-size: var(--vscode-font-size, 13px);
        color: var(--fg);
        background: var(--bg);
        padding: 12px;
    }
    h2 { margin-bottom: 8px; font-size: 1.1em; }
    .section { margin-bottom: 16px; }

    /* Memory gauge */
    .gauge-container {
        display: flex; align-items: center; gap: 8px;
        margin-bottom: 4px;
    }
    .gauge-bar {
        flex: 1; height: 18px; background: var(--border);
        border-radius: 4px; overflow: hidden;
    }
    .gauge-fill {
        height: 100%; background: var(--bar-bg);
        transition: width 0.3s;
    }
    .gauge-label { white-space: nowrap; min-width: 100px; text-align: right; }

    /* Kind stats table */
    table { width: 100%; border-collapse: collapse; }
    th, td { padding: 4px 8px; text-align: left; border-bottom: 1px solid var(--border); }
    th { opacity: 0.7; font-weight: 600; }
    .num { text-align: right; font-variant-numeric: tabular-nums; }

    /* Roots / object tree */
    .tree { list-style: none; padding-left: 0; }
    .tree li { padding: 2px 0; }
    .tree ul { padding-left: 16px; list-style: none; }
    .toggle {
        cursor: pointer; user-select: none;
        color: var(--accent);
    }
    .toggle::before { content: '▶ '; font-size: 0.8em; }
    .toggle.open::before { content: '▼ '; }
    .obj-link {
        cursor: pointer; color: var(--accent);
        text-decoration: underline;
    }
    .badge {
        display: inline-block; padding: 0 6px;
        border-radius: 8px; font-size: 0.85em;
        background: var(--badge); color: var(--badge-fg);
    }

    /* Object detail pane */
    #detail { border-top: 1px solid var(--border); padding-top: 8px; margin-top: 8px; }
    #detail h3 { margin-bottom: 4px; }
    .detail-row { margin: 2px 0; }
    .detail-label { opacity: 0.7; }

    .btn {
        padding: 4px 12px; cursor: pointer;
        background: var(--accent); color: #fff;
        border: none; border-radius: 4px;
        font-size: 0.9em;
    }
    .btn:hover { opacity: 0.85; }

    .error { color: var(--vscode-errorForeground, #f44); margin: 8px 0; }

    #placeholder { opacity: 0.6; margin-top: 32px; text-align: center; }
</style>
</head>
<body>
    <div style="display:flex; align-items:center; justify-content:space-between; margin-bottom:12px;">
        <h2>🔍 Eta Heap Inspector</h2>
        <button class="btn" id="refreshBtn">Refresh</button>
    </div>
    <div id="content">
        <div id="placeholder">Pause the VM (breakpoint / step) to inspect the heap.</div>
    </div>
    <div id="detail" style="display:none;"></div>

<script>
    const vscode = acquireVsCodeApi();

    document.getElementById('refreshBtn').addEventListener('click', () => {
        vscode.postMessage({ command: 'refresh' });
    });

    function fmt(bytes) {
        if (bytes < 1024) return bytes + ' B';
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
        return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }

    function renderSnapshot(snap) {
        const pct = snap.softLimit > 0
            ? Math.min(100, (snap.totalBytes / snap.softLimit) * 100).toFixed(1)
            : 0;

        let html = '';

        // ── Memory gauge ──────────────────────────────────────────────────
        html += '<div class="section">';
        html += '<div class="gauge-container">';
        html += '  <span>Memory</span>';
        html += '  <div class="gauge-bar"><div class="gauge-fill" style="width:' + pct + '%"></div></div>';
        html += '  <span class="gauge-label">' + fmt(snap.totalBytes) + ' / ' + fmt(snap.softLimit) + ' (' + pct + '%)</span>';
        html += '</div></div>';

        // ── Per-kind table ────────────────────────────────────────────────
        const sorted = [...snap.kinds].sort((a, b) => b.bytes - a.bytes);
        html += '<div class="section"><h2>Object Kinds</h2><table>';
        html += '<tr><th>Kind</th><th class="num">Count</th><th class="num">Bytes</th></tr>';
        for (const k of sorted) {
            html += '<tr><td>' + esc(k.kind) + '</td>';
            html += '<td class="num">' + k.count.toLocaleString() + '</td>';
            html += '<td class="num">' + fmt(k.bytes) + '</td></tr>';
        }
        html += '</table></div>';

        // ── GC Roots ─────────────────────────────────────────────────────
        html += '<div class="section"><h2>GC Roots</h2><ul class="tree">';
        for (const root of snap.roots) {
            if (root.objectIds.length === 0) continue;
            html += '<li>';
            html += '<span class="toggle" data-root="' + esc(root.name) + '">' + esc(root.name);
            html += ' <span class="badge">' + root.objectIds.length + '</span></span>';
            html += '<ul class="children" style="display:none">';
            const cap = Math.min(root.objectIds.length, 50);
            for (let i = 0; i < cap; i++) {
                const oid = root.objectIds[i];
                html += '<li><span class="obj-link" data-oid="' + oid + '">Object #' + oid + '</span></li>';
            }
            if (root.objectIds.length > 50) {
                html += '<li><em>… and ' + (root.objectIds.length - 50) + ' more</em></li>';
            }
            html += '</ul></li>';
        }
        html += '</ul></div>';

        document.getElementById('content').innerHTML = html;
        document.getElementById('detail').style.display = 'none';

        // Toggle listeners
        document.querySelectorAll('.toggle').forEach(el => {
            el.addEventListener('click', () => {
                el.classList.toggle('open');
                const ul = el.nextElementSibling;
                if (ul) ul.style.display = ul.style.display === 'none' ? '' : 'none';
            });
        });

        // Object link listeners
        document.querySelectorAll('.obj-link').forEach(el => {
            el.addEventListener('click', () => {
                const oid = parseInt(el.getAttribute('data-oid'), 10);
                vscode.postMessage({ command: 'inspectObject', objectId: oid });
            });
        });
    }

    function renderInspect(obj) {
        let html = '<h3>Object #' + obj.objectId + '</h3>';
        html += '<div class="detail-row"><span class="detail-label">Kind:</span> ' + esc(obj.kind) + '</div>';
        html += '<div class="detail-row"><span class="detail-label">Size:</span> ' + fmt(obj.size) + '</div>';
        html += '<div class="detail-row"><span class="detail-label">Preview:</span> <code>' + esc(obj.preview) + '</code></div>';

        if (obj.children && obj.children.length > 0) {
            html += '<h3 style="margin-top:8px;">Children (' + obj.children.length + ')</h3>';
            html += '<ul class="tree">';
            for (const c of obj.children) {
                html += '<li><span class="obj-link" data-oid="' + c.objectId + '">';
                html += '#' + c.objectId + ' <span class="badge">' + esc(c.kind) + '</span> ';
                html += esc(c.preview);
                html += '</span></li>';
            }
            html += '</ul>';
        } else {
            html += '<div style="margin-top:4px; opacity:0.6;">No heap children.</div>';
        }

        const detail = document.getElementById('detail');
        detail.innerHTML = html;
        detail.style.display = '';

        // Child links
        detail.querySelectorAll('.obj-link').forEach(el => {
            el.addEventListener('click', () => {
                const oid = parseInt(el.getAttribute('data-oid'), 10);
                vscode.postMessage({ command: 'inspectObject', objectId: oid });
            });
        });
    }

    function esc(s) {
        const d = document.createElement('div');
        d.textContent = String(s);
        return d.innerHTML;
    }

    window.addEventListener('message', e => {
        const msg = e.data;
        switch (msg.command) {
            case 'snapshot':
                renderSnapshot(msg.data);
                break;
            case 'inspectResult':
                renderInspect(msg.data);
                break;
            case 'error':
                document.getElementById('content').innerHTML =
                    '<div class="error">' + esc(msg.text) + '</div>';
                break;
        }
    });
</script>
</body>
</html>`;
}

