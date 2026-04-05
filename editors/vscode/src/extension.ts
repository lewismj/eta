import * as path from 'path';
import * as fs from 'fs';
import * as cp from 'child_process';
import {
    workspace,
    ExtensionContext,
    window,
    debug,
    EventEmitter,
    DebugAdapterInlineImplementation,
} from 'vscode';
import type {
    DebugAdapterDescriptor,
    DebugAdapterDescriptorFactory,
    DebugProtocolMessage,
    DebugSession,
} from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Executable,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

function isFile(p: string): boolean {
    try { return fs.statSync(p).isFile(); } catch { return false; }
}

function findServerBinary(context: ExtensionContext): string | undefined {
    // 1. Check user configuration
    const config = workspace.getConfiguration('eta.lsp');
    const configPath = config.get<string>('serverPath', '').trim();
    if (configPath) {
        // Exact path to a file
        if (isFile(configPath)) {
            return configPath;
        }
        // User may have pointed to the bin/ directory — try common names inside it
        for (const name of ['eta_lsp.exe', 'eta_lsp']) {
            const candidate = path.join(configPath, name);
            if (isFile(candidate)) {
                return candidate;
            }
        }
        // Path was explicitly set but nothing was found — warn rather than silently falling through
        window.showWarningMessage(
            `Eta LSP: configured serverPath "${configPath}" does not point to a valid executable.`
        );
        return undefined;
    }

    // 2. Check bundled binary inside the extension directory
    //    (populated when packaging a release .vsix)
    const bundledCandidates = [
        path.join(context.extensionPath, 'bin', 'eta_lsp'),
        path.join(context.extensionPath, 'bin', 'eta_lsp.exe'),
    ];
    for (const c of bundledCandidates) {
        if (isFile(c)) {
            return c;
        }
    }

    // 3. Check workspace-relative build output
    const workspaceFolders = workspace.workspaceFolders;
    if (workspaceFolders) {
        for (const folder of workspaceFolders) {
            const candidates = [
                path.join(folder.uri.fsPath, 'out', 'wsl-clang-release', 'eta', 'lsp', 'eta_lsp'),
                path.join(folder.uri.fsPath, 'out', 'build', 'eta', 'lsp', 'eta_lsp'),
                path.join(folder.uri.fsPath, 'build', 'eta', 'lsp', 'eta_lsp'),
                path.join(folder.uri.fsPath, 'out', 'wsl-clang-release', 'eta', 'lsp', 'eta_lsp.exe'),
                path.join(folder.uri.fsPath, 'build', 'eta', 'lsp', 'eta_lsp.exe'),
            ];
            for (const c of candidates) {
                if (isFile(c)) {
                    return c;
                }
            }
        }
    }

    // 4. Fall back to PATH (hope it's installed)
    return 'eta_lsp';
}

export function activate(context: ExtensionContext) {
    const enabled = workspace.getConfiguration('eta.lsp').get<boolean>('enabled', true);
    if (!enabled) {
        return;
    }

    const serverPath = findServerBinary(context);
    if (!serverPath) {
        window.showWarningMessage(
            'Eta LSP server not found. Set eta.lsp.serverPath in settings or build the eta_lsp target.'
        );
        return;
    }

    // Build environment: inherit process env and inject ETA_MODULE_PATH if configured
    const config = workspace.getConfiguration('eta.lsp');
    const modulePath = config.get<string>('modulePath', '') || process.env['ETA_MODULE_PATH'] || '';
    const serverEnv: NodeJS.ProcessEnv = { ...process.env };
    if (modulePath) {
        serverEnv['ETA_MODULE_PATH'] = modulePath;
    }

    const run: Executable = { command: serverPath, options: { env: serverEnv } };
    const debug_exec: Executable = { command: serverPath, options: { env: serverEnv } };

    const serverOptions: ServerOptions = { run, debug: debug_exec };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'eta' }],
        synchronize: {
            fileEvents: workspace.createFileSystemWatcher('**/*.eta'),
        },
    };

    client = new LanguageClient(
        'etaLsp',
        'Eta Language Server',
        serverOptions,
        clientOptions
    );

    client.start();

    // Register the Eta debug adapter
    const factory = new EtaDebugAdapterFactory(serverPath);
    context.subscriptions.push(
        debug.registerDebugAdapterDescriptorFactory('eta', factory)
    );
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}

// ── Debug adapter ─────────────────────────────────────────────────────────

class EtaDebugAdapterFactory implements DebugAdapterDescriptorFactory {
    constructor(private readonly lspServerPath: string) {}

    createDebugAdapterDescriptor(_session: DebugSession): DebugAdapterDescriptor {
        return new DebugAdapterInlineImplementation(
            new EtaDebugSession(this.lspServerPath)
        );
    }
}

class EtaDebugSession {
    private readonly _sendMessage = new EventEmitter<DebugProtocolMessage>();
    readonly onDidSendMessage = this._sendMessage.event;

    private _proc: cp.ChildProcess | undefined;
    private _seq = 1;

    constructor(private readonly lspServerPath: string) {}

    handleMessage(msg: DebugProtocolMessage): void {
        const m = msg as any;
        if (m.type !== 'request') { return; }

        switch (m.command) {
            case 'initialize':
                this._respond(m, { supportsConfigurationDoneRequest: true, supportsTerminateRequest: true });
                this._event('initialized', {});
                break;
            case 'configurationDone':
                this._respond(m, {});
                break;
            case 'launch':
                this._launch(m);
                break;
            case 'terminate':
            case 'disconnect':
                this._proc?.kill();
                this._respond(m, {});
                break;
            default:
                this._respond(m, {});
        }
    }

    private _launch(m: any): void {
        const program = m.arguments?.program as string | undefined;
        if (!program) {
            this._respond(m, undefined, 'No program specified in launch configuration.');
            return;
        }

        // Locate etai next to the LSP binary, or fall back to PATH
        let etai = process.platform === 'win32' ? 'etai.exe' : 'etai';
        const serverDir = path.dirname(this.lspServerPath);
        const candidate = path.join(serverDir, etai);
        if (isFile(candidate)) {
            etai = candidate;
        }

        // Build environment
        const config = workspace.getConfiguration('eta.lsp');
        const modulePath = config.get<string>('modulePath', '') || process.env['ETA_MODULE_PATH'] || '';
        const env: NodeJS.ProcessEnv = { ...process.env };
        if (modulePath) { env['ETA_MODULE_PATH'] = modulePath; }

        const extraArgs: string[] = m.arguments?.args ?? [];
        this._respond(m, {});

        this._proc = cp.spawn(etai, [program, ...extraArgs], { env });

        this._proc.stdout?.on('data', (chunk: Buffer) => {
            this._event('output', { category: 'stdout', output: chunk.toString() });
        });
        this._proc.stderr?.on('data', (chunk: Buffer) => {
            this._event('output', { category: 'stderr', output: chunk.toString() });
        });
        this._proc.on('error', (err) => {
            this._event('output', {
                category: 'stderr',
                output: `Failed to launch etai: ${err.message}\n` +
                        `Make sure etai is built and on PATH, or set eta.lsp.serverPath.\n`,
            });
            this._event('exited', { exitCode: 1 });
            this._event('terminated', {});
        });
        this._proc.on('close', (code) => {
            this._event('output', { category: 'console', output: `\nProcess exited with code ${code ?? 0}\n` });
            this._event('exited', { exitCode: code ?? 0 });
            this._event('terminated', {});
        });
    }

    private _respond(req: any, body: any, errorMsg?: string): void {
        this._sendMessage.fire({
            type: 'response',
            seq: this._seq++,
            request_seq: req.seq,
            success: !errorMsg,
            command: req.command,
            body: body ?? {},
            ...(errorMsg ? { message: errorMsg } : {}),
        } as any);
    }

    private _event(event: string, body: any): void {
        this._sendMessage.fire({
            type: 'event',
            seq: this._seq++,
            event,
            body,
        } as any);
    }

    dispose(): void {
        this._proc?.kill();
        this._sendMessage.dispose();
    }
}
