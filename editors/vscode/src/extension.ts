import * as path from 'path';
import * as fs from 'fs';
import {
    workspace,
    ExtensionContext,
    window,
    debug,
    commands,
    DebugAdapterExecutable,
    OutputChannel,
    WorkspaceFolder,
    CancellationToken,
} from 'vscode';
import type {
    DebugAdapterDescriptor,
    DebugAdapterDescriptorFactory,
    DebugAdapterTracker,
    DebugConfiguration,
    DebugConfigurationProvider,
    DebugSession,
} from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Executable,
} from 'vscode-languageclient/node';
import { HeapInspectorPanel } from './heapView';
import { GCRootsTreeProvider } from './gcRootsTreeView';
import { DisassemblyContentProvider, showDisassembly } from './disassemblyView';
import { DisassemblyTreeProvider } from './disassemblyTreeView';
import { ChildProcessTreeProvider } from './childProcessTreeView';

let client: LanguageClient | undefined;
let outputChannel: OutputChannel;
let programOutputChannel: OutputChannel;
let extensionCtx: ExtensionContext;

// Shared providers (accessible from tracker)
let gcRootsProvider: GCRootsTreeProvider;
let disasmProvider: DisassemblyContentProvider;
let disasmTreeProvider: DisassemblyTreeProvider;
let childProcProvider: ChildProcessTreeProvider;

function log(msg: string): void {
    outputChannel?.appendLine(msg);
}

function isFile(p: string): boolean {
    try { return fs.statSync(p).isFile(); } catch { return false; }
}

function checkPath(label: string, p: string): boolean {
    const found = isFile(p);
    log(`  - ${label}: ${p} (${found ? 'FOUND' : 'not found'})`);
    return found;
}

/** Resolve a configured path that may be either a direct binary or a directory. */
function resolveConfigBinary(configPath: string, names: string[]): string | undefined {
    if (isFile(configPath)) { return configPath; }
    for (const name of names) {
        const candidate = path.join(configPath, name);
        if (isFile(candidate)) { return candidate; }
    }
    return undefined;
}

function findServerBinary(context: ExtensionContext): string | undefined {
    log('[Checking LSP Binary]');
    log(`  Platform: ${process.platform}`);

    // 1. Check user configuration
    const config = workspace.getConfiguration('eta.lsp');
    const configPath = config.get<string>('serverPath', '').trim();
    if (configPath) {
        log(`  - Searching config: "${configPath}"`);
        const resolved = resolveConfigBinary(configPath, ['eta_lsp.exe', 'eta_lsp']);
        if (resolved) {
            log(`  - Found: ${resolved}`);
            return resolved;
        }
        const msg = `Eta LSP: configured serverPath "${configPath}" does not point to a valid executable.`;
        log(`  ERROR: ${msg}`);
        window.showWarningMessage(msg);
        return undefined;
    } else {
        log(`  - Searching config: "" (not set)`);
    }

    // 2. Check bundled binary inside the extension directory
    for (const name of ['eta_lsp', 'eta_lsp.exe']) {
        const candidate = path.join(context.extensionPath, 'bin', name);
        if (checkPath('bundled', candidate)) { return candidate; }
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
                path.join(folder.uri.fsPath, 'out', 'msvc-release', 'eta', 'lsp', 'eta_lsp.exe'),
                path.join(folder.uri.fsPath, 'build-release', 'eta', 'lsp', 'eta_lsp.exe'),
            ];
            for (const c of candidates) {
                if (checkPath('workspace', c)) { return c; }
            }
        }
    }

    // 4. Fall back to PATH
    log(`  - Falling back to PATH: eta_lsp`);
    return 'eta_lsp';
}

function findDapBinary(lspPath: string | undefined, context: ExtensionContext): string {
    const exe = process.platform === 'win32' ? 'eta_dap.exe' : 'eta_dap';
    log('[Checking DAP Binary]');
    log(`  Platform: ${process.platform}, looking for: ${exe}`);

    // 1. User configuration
    const configPath = workspace.getConfiguration('eta.dap').get<string>('executablePath', '').trim();
    if (configPath) {
        const resolved = resolveConfigBinary(configPath, [exe]);
        if (resolved) { log(`  - Found (config): ${resolved}`); return resolved; }
    }

    // 2. Next to the LSP binary (most common after an install)
    if (lspPath && lspPath !== 'eta_lsp' && lspPath !== 'eta_lsp.exe') {
        const candidate = path.join(path.dirname(lspPath), exe);
        if (checkPath('next to LSP binary', candidate)) { return candidate; }
    }

    // 3. Bundled alongside the extension
    const bundled = path.join(context.extensionPath, 'bin', exe);
    if (checkPath('bundled', bundled)) { return bundled; }

    // 4. Workspace build output
    const workspaceFolders = workspace.workspaceFolders;
    if (workspaceFolders) {
        for (const folder of workspaceFolders) {
            for (const rel of [
                path.join('out', 'wsl-clang-release', 'eta', 'dap', exe),
                path.join('out', 'build', 'eta', 'dap', exe),
                path.join('build', 'eta', 'dap', exe),
                path.join('build-release', 'eta', 'dap', exe),
                path.join('out', 'msvc-release', 'eta', 'dap', exe),
            ]) {
                const c = path.join(folder.uri.fsPath, rel);
                if (checkPath('workspace', c)) { return c; }
            }
        }
    }

    // 5. Fall back to PATH
    log(`  WARNING: eta_dap not found in any known location; falling back to PATH lookup.`);
    log(`  To fix: set "eta.dap.executablePath" in VS Code settings to the full path of ${exe}.`);
    return exe;
}

/** Resolve ETA_MODULE_PATH from the new shared setting, legacy lsp setting, or env. */
function resolveModulePath(): string {
    return workspace.getConfiguration('eta').get<string>('modulePath', '')
        || workspace.getConfiguration('eta.lsp').get<string>('modulePath', '')
        || process.env['ETA_MODULE_PATH']
        || '';
}

export function activate(context: ExtensionContext) {
    extensionCtx = context;
    outputChannel = window.createOutputChannel('Eta Language');
    programOutputChannel = window.createOutputChannel('Eta Output');
    context.subscriptions.push(outputChannel, programOutputChannel);
    log('Eta extension activating...');

    // ── GC Roots tree view ──────────────────────────────────────────
    gcRootsProvider = new GCRootsTreeProvider();
    context.subscriptions.push(
        window.createTreeView('etaGCRoots', {
            treeDataProvider: gcRootsProvider,
            showCollapseAll: true,
        }),
    );

    // ── Disassembly virtual document provider ───────────────────────
    disasmProvider = new DisassemblyContentProvider();
    context.subscriptions.push(
        workspace.registerTextDocumentContentProvider('eta-disasm', disasmProvider),
    );

    // ── Disassembly tree view (debug sidebar) ────────────────────────────
    disasmTreeProvider = new DisassemblyTreeProvider();
    context.subscriptions.push(
        window.createTreeView('etaDisassembly', {
            treeDataProvider: disasmTreeProvider,
            showCollapseAll: false,
        }),
    );

    // ── Child process tree view (debug sidebar) ──────────────────────────
    childProcProvider = new ChildProcessTreeProvider();
    context.subscriptions.push(
        window.createTreeView('etaChildProcesses', {
            treeDataProvider: childProcProvider,
            showCollapseAll: false,
        }),
    );

    // ── Always register the debug adapter ──────────────────────────
    const serverPath = findServerBinary(context);
    const dapPath    = findDapBinary(serverPath, context);

    log(`[Summary]`);
    log(`  LSP binary  : ${serverPath ?? '(none)'}`);
    log(`  DAP binary  : ${dapPath}`);

    const factory = new EtaDebugAdapterFactory(dapPath, outputChannel);
    context.subscriptions.push(
        debug.registerDebugAdapterDescriptorFactory('eta', factory),
        debug.registerDebugAdapterTrackerFactory('eta', {
            createDebugAdapterTracker(_session: DebugSession): DebugAdapterTracker {
                return new EtaDebugAdapterTracker(outputChannel, programOutputChannel);
            },
        }),
        debug.registerDebugConfigurationProvider('eta', new EtaDebugConfigurationProvider()),
        commands.registerCommand('eta.runFile', () => {
            const editor = window.activeTextEditor;
            if (!editor || editor.document.languageId !== 'eta') {
                window.showWarningMessage('Open an .eta file first.');
                return;
            }
            debug.startDebugging(undefined, {
                type: 'eta',
                request: 'launch',
                name: 'Run Eta file',
                program: editor.document.fileName,
            });
        }),
        commands.registerCommand('eta.showHeapInspector', () => {
            HeapInspectorPanel.createOrShow(extensionCtx).refresh();
        }),
        commands.registerCommand('eta.showDisassembly', () => {
            showDisassembly(disasmProvider, 'current');
        }),
        commands.registerCommand('eta.showDisassemblyAll', () => {
            showDisassembly(disasmProvider, 'all');
        }),
        commands.registerCommand('eta.refreshGCRoots', () => {
            gcRootsProvider.refresh();
        }),
        commands.registerCommand('eta.refreshDisassembly', () => {
            disasmTreeProvider.refresh();
        }),
        commands.registerCommand('eta.refreshChildProcesses', () => {
            childProcProvider.refresh();
        }),
        commands.registerCommand('eta.inspectObjectFromTree', (objectId: number) => {
            HeapInspectorPanel.createOrShow(extensionCtx).inspectObject(objectId);
        }),
    );

    // ── Watch for configuration changes ────────────────────────────
    context.subscriptions.push(
        workspace.onDidChangeConfiguration(e => {
            if (e.affectsConfiguration('eta.lsp.serverPath')) {
                const configPath = workspace.getConfiguration('eta.lsp').get<string>('serverPath', '').trim();
                if (configPath && !resolveConfigBinary(configPath, ['eta_lsp.exe', 'eta_lsp'])) {
                    window.showWarningMessage(
                        `Eta: eta.lsp.serverPath "${configPath}" is not a valid executable or directory.`
                    );
                }
            }
        })
    );

    // ── LSP setup ──────────────────────────────────────────────────
    const enabled = workspace.getConfiguration('eta.lsp').get<boolean>('enabled', true);
    if (!enabled) {
        log('LSP is disabled via eta.lsp.enabled = false. Skipping LSP startup.');
        return;
    }

    if (!serverPath) {
        const msg = 'Eta LSP server not found. Set eta.lsp.serverPath in settings or build the eta_lsp target.';
        log(`WARNING: ${msg}`);
        window.showWarningMessage(msg);
        return;
    }

    const modulePath = resolveModulePath();
    const serverEnv: NodeJS.ProcessEnv = { ...process.env };
    if (modulePath) {
        serverEnv['ETA_MODULE_PATH'] = modulePath;
        log(`  ETA_MODULE_PATH : ${modulePath}`);
    }

    log(`[Starting LSP]`);
    log(`  Command: ${serverPath}`);

    const exec: Executable = { command: serverPath, options: { env: serverEnv } };
    const serverOptions: ServerOptions = { run: exec, debug: exec };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'eta' }],
        synchronize: {
            fileEvents: workspace.createFileSystemWatcher('**/*.eta'),
        },
        outputChannel,
    };

    client = new LanguageClient(
        'etaLsp',
        'Eta Language Server',
        serverOptions,
        clientOptions
    );

    client.start();
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}

// ── Debug configuration provider ─────────────────────────────────────────────────

class EtaDebugConfigurationProvider implements DebugConfigurationProvider {
    resolveDebugConfiguration(
        _folder: WorkspaceFolder | undefined,
        config: DebugConfiguration,
        _token?: CancellationToken,
    ): DebugConfiguration | undefined {
        if (!config.type && !config.request && !config.name) {
            const editor = window.activeTextEditor;
            if (!editor || editor.document.languageId !== 'eta') {
                window.showWarningMessage('Open an .eta file to run it.');
                return undefined;
            }
            return {
                type: 'eta',
                request: 'launch',
                name: 'Run Eta file',
                program: editor.document.fileName,
            };
        }

        if (!config.program) {
            window.showWarningMessage(
                'Set "program" in your launch.json to the .eta file you want to run (e.g. "${file}").'
            );
            return undefined;
        }

        return config;
    }
}

// ── Debug adapter tracker ─────────────────────────────────────────────────────

class EtaDebugAdapterTracker implements DebugAdapterTracker {
    constructor(
        private readonly channel: OutputChannel,
        private readonly programChannel: OutputChannel,
    ) {}

    onWillStartSession(): void {
        this.channel.appendLine('[DAP] Debug session starting…');
        this.programChannel.clear();
        this.programChannel.show(true);

        const autoShow = workspace.getConfiguration('eta.debug').get<boolean>('autoShowHeap', true);
        if (autoShow && extensionCtx) {
            HeapInspectorPanel.createOrShow(extensionCtx);
        }
    }

    onWillStopSession(): void {
        this.channel.appendLine('[DAP] Debug session stopping…');
        childProcProvider?.notifySessionEnded();
    }

    onWillReceiveMessage(message: any): void {
        const cmd: string = message?.command ?? '';
        const args = message?.arguments;
        switch (cmd) {
            case 'initialize':
                this.channel.appendLine('[DAP→] initialize');
                break;
            case 'launch':
                this.channel.appendLine(`[DAP→] launch: program="${args?.program ?? '?'}"`);
                break;
            case 'setBreakpoints': {
                const srcPath: string = args?.source?.path ?? '?';
                const lines: number[] = (args?.breakpoints ?? []).map((b: any) => b.line as number);
                this.channel.appendLine(
                    `[DAP→] setBreakpoints: ${lines.length} bp(s) in "${srcPath}" lines=[${lines.join(',')}]`
                );
                break;
            }
            case 'configurationDone':
                this.channel.appendLine('[DAP→] configurationDone');
                break;
            case 'continue':
            case 'next':
            case 'stepIn':
            case 'stepOut':
            case 'pause':
            case 'disconnect':
                this.channel.appendLine(`[DAP→] ${cmd}`);
                break;
            default:
                if (cmd) { this.channel.appendLine(`[DAP→] ${cmd}`); }
        }
    }

    onDidSendMessage(message: any): void {
        const type: string  = message?.type  ?? '';
        const event: string = message?.event ?? '';

        if (type === 'event') {
            if (event === 'eta-output') {
                const text: string = message?.body?.text ?? '';
                if (text) { this.programChannel.append(text); }
            } else if (event === 'output') {
                const output: string = message?.body?.output ?? '';
                if (output) { this.channel.append(output); }
            } else if (event === 'initialized') {
                this.channel.appendLine('[DAP←] initialized (adapter ready; VS Code will now send setBreakpoints)');
            } else if (event === 'stopped') {
                const reason: string = message?.body?.reason ?? '?';
                const tid: number    = message?.body?.threadId ?? 0;
                this.channel.appendLine(`[DAP←] stopped: reason="${reason}" threadId=${tid}`);
                HeapInspectorPanel.current()?.notifyStopped();
                gcRootsProvider?.notifyStopped();
                disasmTreeProvider?.notifyStopped();
                disasmProvider?.refresh();
                childProcProvider?.notifyStopped();
            } else if (event === 'continued') {
                this.channel.appendLine('[DAP←] continued');
            } else if (event === 'breakpoint') {
                const bp  = message?.body?.breakpoint ?? {};
                const why = message?.body?.reason ?? '?';
                this.channel.appendLine(
                    `[DAP←] breakpoint ${why}: id=${bp.id} verified=${bp.verified} line=${bp.line}`
                );
            } else if (event === 'terminated') {
                this.channel.appendLine('[DAP←] terminated');
            } else if (event === 'exited') {
                this.channel.appendLine(`[DAP←] exited: code=${message?.body?.exitCode ?? '?'}`);
            }
        } else if (type === 'response') {
            const cmd     = message?.command ?? '';
            const success = message?.success ?? false;
            if (!success) {
                this.channel.appendLine(
                    `[DAP←] ERROR response to "${cmd}": ${JSON.stringify(message?.body ?? {})}`
                );
            } else if (cmd === 'initialize') {
                const caps = message?.body ?? {};
                this.channel.appendLine(
                    `[DAP←] initialize OK — supportsConfigurationDone=${caps.supportsConfigurationDoneRequest}`
                );
            }
        }
    }

    onError(error: Error): void {
        this.channel.appendLine(`[DAP] Adapter error: ${error.message}`);
    }

    onExit(code: number | undefined, signal: string | undefined): void {
        this.channel.appendLine(
            `[DAP] Adapter exited (code=${code ?? 'null'}, signal=${signal ?? 'null'}).`
        );
    }
}

class EtaDebugAdapterFactory implements DebugAdapterDescriptorFactory {
    constructor(
        private readonly dapPath: string,
        private readonly channel: OutputChannel,
    ) {}

    createDebugAdapterDescriptor(_session: DebugSession): DebugAdapterDescriptor {
        const modulePath = resolveModulePath();
        const env: { [key: string]: string } = {};
        for (const [k, v] of Object.entries(process.env)) {
            if (v !== undefined) { env[k] = v; }
        }
        if (modulePath) {
            env['ETA_MODULE_PATH'] = modulePath;
        }

        this.channel.appendLine(`[DAP] Launching: ${this.dapPath}`);
        this.channel.appendLine(`[DAP] ETA_MODULE_PATH: ${modulePath || '(not set)'}`);

        return new DebugAdapterExecutable(this.dapPath, [], { env });
    }
}
