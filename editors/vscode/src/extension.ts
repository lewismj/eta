import {
    workspace,
    ExtensionContext,
    window,
    debug,
    commands,
    DebugAdapterExecutable,
    LogOutputChannel,
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
import { registerTestController } from './testController';
import { discoverBinaries } from './binaries';

let client: LanguageClient | undefined;
let outputChannel: LogOutputChannel;
let programOutputChannel: LogOutputChannel;
let extensionCtx: ExtensionContext;

// Shared providers (accessible from tracker)
let gcRootsProvider: GCRootsTreeProvider;
let disasmProvider: DisassemblyContentProvider;
let disasmTreeProvider: DisassemblyTreeProvider;
let childProcProvider: ChildProcessTreeProvider;

function log(msg: string): void {
    outputChannel?.info(msg);
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
    outputChannel = window.createOutputChannel('Eta Language', { log: true });
    programOutputChannel = window.createOutputChannel('Eta Output', { log: true });
    context.subscriptions.push(outputChannel, programOutputChannel);
    log('Eta extension activating...');

    // â”€â”€ GC Roots tree view â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    gcRootsProvider = new GCRootsTreeProvider();
    context.subscriptions.push(
        window.createTreeView('etaGCRoots', {
            treeDataProvider: gcRootsProvider,
            showCollapseAll: true,
        }),
    );

    // â”€â”€ Disassembly virtual document provider â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    disasmProvider = new DisassemblyContentProvider();
    context.subscriptions.push(
        workspace.registerTextDocumentContentProvider('eta-disasm', disasmProvider),
    );

    // â”€â”€ Disassembly tree view (debug sidebar) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    disasmTreeProvider = new DisassemblyTreeProvider();
    context.subscriptions.push(
        window.createTreeView('etaDisassembly', {
            treeDataProvider: disasmTreeProvider,
            showCollapseAll: false,
        }),
    );

    // â”€â”€ Child process tree view (debug sidebar) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    childProcProvider = new ChildProcessTreeProvider();
    context.subscriptions.push(
        window.createTreeView('etaChildProcesses', {
            treeDataProvider: childProcProvider,
            showCollapseAll: false,
        }),
    );

    // â”€â”€ Always register the debug adapter â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    const binaries = discoverBinaries(context);
    const serverPath = binaries.lsp;
    const dapPath = binaries.dap ?? (process.platform === 'win32' ? 'eta_dap.exe' : 'eta_dap');

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

    // â”€â”€ Test Explorer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    registerTestController(context);

    // â”€â”€ Watch for configuration changes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    context.subscriptions.push(
        workspace.onDidChangeConfiguration(e => {
            if (
                e.affectsConfiguration('eta.lsp.serverPath')
                || e.affectsConfiguration('eta.dap.executablePath')
                || e.affectsConfiguration('eta.binaries.searchPaths')
            ) {
                const refreshed = discoverBinaries(context);
                outputChannel.info(`LSP binary resolved to: ${refreshed.lsp ?? '(none)'}`);
                outputChannel.info(`DAP binary resolved to: ${refreshed.dap ?? '(none)'}`);
            }
        })
    );

    // â”€â”€ LSP setup â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

// â”€â”€ Debug configuration provider â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

        if (!Array.isArray(config.args)) {
            config.args = [];
        }
        if (!config.cwd || typeof config.cwd !== 'string') {
            config.cwd = '${workspaceFolder}';
        }
        if (!config.env || typeof config.env !== 'object' || Array.isArray(config.env)) {
            config.env = {};
        }
        if (typeof config.stopOnEntry !== 'boolean') {
            config.stopOnEntry = false;
        }
        if (typeof config.etac !== 'boolean') {
            config.etac = false;
        }
        if (typeof config.trace !== 'boolean') {
            config.trace = false;
        }
        if (typeof config.console !== 'string') {
            config.console = 'debugConsole';
        }
        if (config.console !== 'debugConsole'
            && config.console !== 'integratedTerminal'
            && config.console !== 'externalTerminal') {
            window.showWarningMessage(
                'Eta debug: "console" must be one of debugConsole, integratedTerminal, or externalTerminal.'
            );
            return undefined;
        }

        if (!config.modulePath || typeof config.modulePath !== 'string') {
            config.modulePath = resolveModulePath();
        }

        return config;
    }
}

// â”€â”€ Debug adapter tracker â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

class EtaDebugAdapterTracker implements DebugAdapterTracker {
    constructor(
        private readonly channel: LogOutputChannel,
        private readonly programChannel: LogOutputChannel,
    ) {}

    onWillStartSession(): void {
        this.channel.appendLine('[DAP] Debug session startingâ€¦');
        this.programChannel.clear();
        this.programChannel.show(true);

        const autoShow = workspace.getConfiguration('eta.debug').get<boolean>('autoShowHeap', true);
        if (autoShow && extensionCtx) {
            HeapInspectorPanel.createOrShow(extensionCtx);
        }
    }

    onWillStopSession(): void {
        this.channel.appendLine('[DAP] Debug session stoppingâ€¦');
        childProcProvider?.notifySessionEnded();
    }

    onWillReceiveMessage(message: any): void {
        const cmd: string = message?.command ?? '';
        const args = message?.arguments;
        switch (cmd) {
            case 'initialize':
                this.channel.appendLine('[DAPâ†’] initialize');
                break;
            case 'launch':
                this.channel.appendLine(`[DAPâ†’] launch: program="${args?.program ?? '?'}"`);
                break;
            case 'setBreakpoints': {
                const srcPath: string = args?.source?.path ?? '?';
                const lines: number[] = (args?.breakpoints ?? []).map((b: any) => b.line as number);
                this.channel.appendLine(
                    `[DAPâ†’] setBreakpoints: ${lines.length} bp(s) in "${srcPath}" lines=[${lines.join(',')}]`
                );
                break;
            }
            case 'configurationDone':
                this.channel.appendLine('[DAPâ†’] configurationDone');
                break;
            case 'continue':
            case 'next':
            case 'stepIn':
            case 'stepOut':
            case 'pause':
            case 'disconnect':
                this.channel.appendLine(`[DAPâ†’] ${cmd}`);
                break;
            default:
                if (cmd) { this.channel.appendLine(`[DAPâ†’] ${cmd}`); }
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
                this.channel.appendLine('[DAPâ†] initialized (adapter ready; VS Code will now send setBreakpoints)');
            } else if (event === 'stopped') {
                const reason: string = message?.body?.reason ?? '?';
                const tid: number    = message?.body?.threadId ?? 0;
                this.channel.appendLine(`[DAPâ†] stopped: reason="${reason}" threadId=${tid}`);
                HeapInspectorPanel.current()?.notifyStopped();
                gcRootsProvider?.notifyStopped();
                disasmTreeProvider?.notifyStopped();
                disasmProvider?.refresh();
                childProcProvider?.notifyStopped();
            } else if (event === 'continued') {
                this.channel.appendLine('[DAPâ†] continued');
            } else if (event === 'breakpoint') {
                const bp  = message?.body?.breakpoint ?? {};
                const why = message?.body?.reason ?? '?';
                this.channel.appendLine(
                    `[DAPâ†] breakpoint ${why}: id=${bp.id} verified=${bp.verified} line=${bp.line}`
                );
            } else if (event === 'terminated') {
                this.channel.appendLine('[DAPâ†] terminated');
            } else if (event === 'exited') {
                this.channel.appendLine(`[DAPâ†] exited: code=${message?.body?.exitCode ?? '?'}`);
            }
        } else if (type === 'response') {
            const cmd     = message?.command ?? '';
            const success = message?.success ?? false;
            if (!success) {
                this.channel.appendLine(
                    `[DAPâ†] ERROR response to "${cmd}": ${JSON.stringify(message?.body ?? {})}`
                );
            } else if (cmd === 'initialize') {
                const caps = message?.body ?? {};
                this.channel.appendLine(
                    `[DAPâ†] initialize OK â€” supportsConfigurationDone=${caps.supportsConfigurationDoneRequest}`
                );
            } else if (cmd === 'eta/childProcesses') {
                const children = message?.body?.children;
                if (Array.isArray(children)) {
                    childProcProvider?.updateChildren(children);
                }
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
        private readonly channel: LogOutputChannel,
    ) {}

    createDebugAdapterDescriptor(session: DebugSession): DebugAdapterDescriptor {
        const launchConfig = session.configuration ?? {};
        const modulePath = typeof launchConfig.modulePath === 'string' && launchConfig.modulePath.length > 0
            ? launchConfig.modulePath
            : resolveModulePath();
        const env: { [key: string]: string } = {};
        for (const [k, v] of Object.entries(process.env)) {
            if (v !== undefined) { env[k] = v; }
        }
        const configEnv = launchConfig.env;
        if (configEnv && typeof configEnv === 'object' && !Array.isArray(configEnv)) {
            for (const [k, v] of Object.entries(configEnv)) {
                if (typeof v === 'string') env[k] = v;
            }
        }
        if (modulePath) {
            env['ETA_MODULE_PATH'] = modulePath;
        }

        const dapArgs: string[] = [];
        if (launchConfig.trace === true) {
            dapArgs.push('--trace-protocol');
        }

        const options: { env: { [key: string]: string }; cwd?: string } = { env };
        if (typeof launchConfig.cwd === 'string' && launchConfig.cwd.length > 0) {
            options.cwd = launchConfig.cwd;
        }

        this.channel.info(`[DAP] Launching: ${this.dapPath}`);
        this.channel.info(`[DAP] ETA_MODULE_PATH: ${modulePath || '(not set)'}`);

        return new DebugAdapterExecutable(this.dapPath, dapArgs, options);
    }
}
