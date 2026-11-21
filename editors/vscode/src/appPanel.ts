/**
 * "Eta: Open App" — show a running std.ui application in a VS Code panel.
 *
 * The Eta process serves its own front end and writes a discovery file
 * `<config>/eta/ui/<app-id>.url` containing the page URL. This command lists
 * those files (most recent first), and opens the chosen app in a webview
 * panel that hosts the page in an iframe. `env.asExternalUri` maps the local
 * port, so it also works in remote / devcontainer sessions.
 *
 * The extension contains no protocol code: the page talks to the Eta
 * process directly over its WebSocket.
 */

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { Disposable, Uri, ViewColumn, WebviewPanel, env, window } from 'vscode';

interface AppEntry {
    id: string;
    url: string;
    modified: number;
}

function uiConfigDir(): string {
    if (process.platform === 'win32' && process.env.APPDATA) {
        return path.join(process.env.APPDATA, 'eta', 'ui');
    }
    const xdg = process.env.XDG_CONFIG_HOME;
    if (xdg) return path.join(xdg, 'eta', 'ui');
    return path.join(os.homedir(), '.config', 'eta', 'ui');
}

function listApps(): AppEntry[] {
    const dir = uiConfigDir();
    let names: string[];
    try {
        names = fs.readdirSync(dir);
    } catch {
        return [];
    }
    const apps: AppEntry[] = [];
    for (const name of names) {
        if (!name.endsWith('.url')) continue;
        const file = path.join(dir, name);
        try {
            const url = fs.readFileSync(file, 'utf8').trim();
            if (!url.startsWith('http://127.0.0.1:')) continue;
            apps.push({ id: name.slice(0, -4), url, modified: fs.statSync(file).mtimeMs });
        } catch {
            // ignore unreadable entries
        }
    }
    return apps.sort((a, b) => b.modified - a.modified);
}

export class EtaAppPanel {
    private static panels = new Map<string, EtaAppPanel>();
    private readonly disposables: Disposable[] = [];

    private constructor(private readonly panel: WebviewPanel, private readonly appId: string) {
        this.panel.onDidDispose(() => this.dispose(), null, this.disposables);
    }

    static async show(app: AppEntry): Promise<void> {
        const existing = EtaAppPanel.panels.get(app.id);
        const external = await env.asExternalUri(Uri.parse(app.url));
        if (existing) {
            existing.panel.webview.html = EtaAppPanel.html(external.toString(true));
            existing.panel.reveal(ViewColumn.Beside);
            return;
        }
        const panel = window.createWebviewPanel('etaApp', `Eta: ${app.id}`, ViewColumn.Beside, {
            enableScripts: true,
            retainContextWhenHidden: true,
        });
        panel.webview.html = EtaAppPanel.html(external.toString(true));
        EtaAppPanel.panels.set(app.id, new EtaAppPanel(panel, app.id));
    }

    private static html(src: string): string {
        const origin = new URL(src).origin;
        const escaped = src.replace(/&/g, '&amp;').replace(/"/g, '&quot;');
        return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; frame-src ${origin}; style-src 'unsafe-inline';">
<style>
  html, body { margin: 0; padding: 0; height: 100%; overflow: hidden; background: #0d0d0d; }
  iframe { border: 0; width: 100%; height: 100%; display: block; }
</style>
</head>
<body>
<iframe src="${escaped}" allow="clipboard-read; clipboard-write"></iframe>
</body>
</html>`;
    }

    private dispose(): void {
        EtaAppPanel.panels.delete(this.appId);
        for (const d of this.disposables) d.dispose();
    }
}

/** Command handler for `eta.openApp`. */
export async function openEtaApp(): Promise<void> {
    const apps = listApps();
    if (apps.length === 0) {
        window.showInformationMessage(
            'No Eta app has been started yet. Run a program that calls ui:run (set ETA_UI=none to skip the browser).',
        );
        return;
    }
    let chosen = apps[0];
    if (apps.length > 1) {
        const pick = await window.showQuickPick(
            apps.map((a) => ({
                label: a.id,
                description: new Date(a.modified).toLocaleString(),
                app: a,
            })),
            { placeHolder: 'Open which Eta app?' },
        );
        if (!pick) return;
        chosen = pick.app;
    }
    await EtaAppPanel.show(chosen);
}
