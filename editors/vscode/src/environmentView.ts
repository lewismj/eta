import {
    commands,
    ConfigurationTarget,
    debug,
    ExtensionContext,
    Uri,
    ViewColumn,
    Webview,
    WebviewPanel,
    window,
    workspace,
} from 'vscode';
import * as crypto from 'crypto';
import * as fs from 'fs';
import * as path from 'path';
import { buildEnvironmentRequestArgs, getEnvironmentSettings } from './gcRootsTreeView';
import type { DebugVariable, EnvironmentSnapshot } from './dapTypes';

type FilterKey =
    | 'showLocals'
    | 'showClosures'
    | 'showGlobals'
    | 'showBuiltins'
    | 'showInternal'
    | 'showNil';

export class EnvironmentInspectorPanel {
    public static readonly viewType = 'etaEnvironmentInspector';

    private static instance: EnvironmentInspectorPanel | undefined;
    private readonly panel: WebviewPanel;
    private readonly extensionUri: Uri;
    private snapshot: EnvironmentSnapshot | undefined;
    private idleText = 'Start and pause an Eta debug session to inspect lexical environments.';

    private constructor(panel: WebviewPanel, extensionUri: Uri) {
        this.panel = panel;
        this.extensionUri = extensionUri;
        this.panel.webview.html = this.getWebviewHtml(panel.webview);

        this.panel.webview.onDidReceiveMessage(async (msg: any) => {
            switch (msg?.command) {
                case 'ready':
                    this.postSettings();
                    if (this.snapshot) {
                        this.postSnapshot();
                    } else {
                        this.showIdle(this.idleText);
                    }
                    break;
                case 'refresh':
                    await this.refresh();
                    break;
                case 'expandVariable':
                    await this.expandVariable(msg.requestId, msg.variablesReference);
                    break;
                case 'inspectObject':
                    if (typeof msg.objectId === 'number' && Number.isFinite(msg.objectId)) {
                        await commands.executeCommand('eta.inspectObjectFromTree', msg.objectId);
                    }
                    break;
                case 'showDisassembly':
                    await commands.executeCommand(
                        msg.scope === 'all' ? 'eta.showDisassemblyAll' : 'eta.showDisassembly',
                    );
                    break;
                case 'setFilter':
                    await this.updateFilterSetting(msg.key as FilterKey, !!msg.value);
                    this.postSettings();
                    await this.refresh();
                    break;
                case 'setFollowActiveFrame':
                    await this.updateFollowSetting(!!msg.value);
                    this.postSettings();
                    await this.refresh();
                    break;
            }
        });

        panel.onDidDispose(() => {
            EnvironmentInspectorPanel.instance = undefined;
        });
    }

    public static createOrShow(ctx: ExtensionContext): EnvironmentInspectorPanel {
        if (EnvironmentInspectorPanel.instance) {
            EnvironmentInspectorPanel.instance.panel.reveal(ViewColumn.Beside);
            return EnvironmentInspectorPanel.instance;
        }
        const mediaRoot = Uri.joinPath(ctx.extensionUri, 'media');
        const panel = window.createWebviewPanel(
            EnvironmentInspectorPanel.viewType,
            'Eta Environment Inspector',
            ViewColumn.Beside,
            {
                enableScripts: true,
                retainContextWhenHidden: false,
                localResourceRoots: [mediaRoot],
            },
        );
        EnvironmentInspectorPanel.instance = new EnvironmentInspectorPanel(panel, ctx.extensionUri);
        return EnvironmentInspectorPanel.instance;
    }

    public static current(): EnvironmentInspectorPanel | undefined {
        return EnvironmentInspectorPanel.instance;
    }

    public static disposeCurrent(): void {
        EnvironmentInspectorPanel.instance?.panel.dispose();
    }

    public isVisible(): boolean {
        return this.panel.visible;
    }

    public applyEnvironment(snapshot: EnvironmentSnapshot): void {
        this.snapshot = snapshot;
        this.postSnapshot();
    }

    public showIdle(text: string): void {
        this.idleText = text;
        this.panel.webview.postMessage({ command: 'idle', text });
    }

    public showError(text: string): void {
        this.panel.webview.postMessage({ command: 'error', text });
    }

    public async refresh(): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.showIdle('Start and pause an Eta debug session to inspect lexical environments.');
            return;
        }
        try {
            const snap = await session.customRequest(
                'eta/environment',
                buildEnvironmentRequestArgs(session),
            ) as EnvironmentSnapshot;
            this.applyEnvironment(snap);
        } catch (err: any) {
            const text = err?.message ?? String(err);
            if (/must be paused/i.test(text)) {
                this.showIdle('Pause the VM (breakpoint or step) to inspect lexical environments.');
                return;
            }
            this.showError(text);
        }
    }

    private postSnapshot(): void {
        if (!this.snapshot) {
            return;
        }
        this.panel.webview.postMessage({
            command: 'snapshot',
            data: this.snapshot,
        });
    }

    private postSettings(): void {
        this.panel.webview.postMessage({
            command: 'settings',
            data: getEnvironmentSettings(),
        });
    }

    private async updateFilterSetting(key: FilterKey, value: boolean): Promise<void> {
        const cfg = workspace.getConfiguration('eta.debug.environment');
        await cfg.update(key, value, this.configurationTarget());
    }

    private async updateFollowSetting(value: boolean): Promise<void> {
        const cfg = workspace.getConfiguration('eta.debug.environment');
        await cfg.update('followActiveFrame', value, this.configurationTarget());
    }

    private configurationTarget(): ConfigurationTarget {
        return workspace.workspaceFolders && workspace.workspaceFolders.length > 0
            ? ConfigurationTarget.Workspace
            : ConfigurationTarget.Global;
    }

    private async expandVariable(requestId: string, variablesReference: number): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.panel.webview.postMessage({
                command: 'expanded',
                requestId,
                variablesReference,
                variables: [],
                error: 'No active Eta debug session.',
            });
            return;
        }
        try {
            const resp = await session.customRequest('variables', {
                variablesReference,
                start: 0,
                count: 0,
            }) as { variables?: DebugVariable[] };
            this.panel.webview.postMessage({
                command: 'expanded',
                requestId,
                variablesReference,
                variables: Array.isArray(resp?.variables) ? resp.variables : [],
            });
        } catch (err: any) {
            this.panel.webview.postMessage({
                command: 'expanded',
                requestId,
                variablesReference,
                variables: [],
                error: err?.message ?? String(err),
            });
        }
    }

    private getWebviewHtml(webview: Webview): string {
        const mediaDir = Uri.joinPath(this.extensionUri, 'media', 'environment');
        const cssUri = webview.asWebviewUri(Uri.joinPath(mediaDir, 'environment.css'));
        const jsUri = webview.asWebviewUri(Uri.joinPath(mediaDir, 'environment.js'));
        const nonce = crypto.randomBytes(16).toString('base64');

        const htmlPath = path.join(mediaDir.fsPath, 'environment.html');
        let template: string;
        try {
            template = fs.readFileSync(htmlPath, 'utf8');
        } catch {
            return '<html lang="en"><body><pre>Failed to load environment.html bundle.</pre></body></html>';
        }
        return template
            .replace(/\{\{cspSource}}/g, webview.cspSource)
            .replace(/\{\{nonce}}/g, nonce)
            .replace(/\{\{cssUri}}/g, cssUri.toString())
            .replace(/\{\{jsUri}}/g, jsUri.toString());
    }
}
