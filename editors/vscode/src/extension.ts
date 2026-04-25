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
    languages,
    Uri,
    ViewColumn,
    Range,
    Selection,
    TextEditorRevealType,
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
import * as fs from 'fs';
import * as path from 'path';
import { HeapInspectorPanel } from './heapView';
import { GCRootsTreeProvider } from './gcRootsTreeView';
import {
    DisassemblyContentProvider,
    EtaDisassemblyDefinitionProvider,
    showDisassembly,
    autoShowDisassemblyOnStop,
    type DisassemblyResult,
} from './disassemblyView';
import { DisassemblyTreeProvider } from './disassemblyTreeView';
import { ChildProcessTreeProvider } from './childProcessTreeView';
import type { ChildProcessInfo, HeapSnapshot } from './dapTypes';
import { registerTestController, runTestsForUri } from './testController';
import { discoverBinaries } from './binaries';
import { EtaInlineValuesProvider } from './inlineValues';
import { EtaEvaluatableExpressionProvider } from './evaluatableExpression';
import { EtaCodeLensProvider } from './codeLens';
import { EtaDocumentLinkProvider } from './documentLink';

let client: LanguageClient | undefined;
let outputChannel: LogOutputChannel;
let programOutputChannel: LogOutputChannel;
let extensionCtx: ExtensionContext;

// Shared providers (accessible from tracker)
let gcRootsProvider: GCRootsTreeProvider;
let disasmProvider: DisassemblyContentProvider;
let disasmTreeProvider: DisassemblyTreeProvider;
let childProcProvider: ChildProcessTreeProvider;
let gcRootsViewVisible = false;
let disasmViewVisible = false;
let childProcViewVisible = false;
const VSX_DEBUG_LOG_PATH = process.env['ETA_VSX_LOG_PATH']
    || (process.platform === 'win32'
        ? 'C:\\tmp\\eta_vsx.txt'
        : path.join(process.cwd(), 'eta_vsx.txt'));
let vsxDebugLogEnabled = true;

function log(msg: string): void {
    outputChannel?.info(msg);
}

function logToFile(msg: string): void {
    if (!vsxDebugLogEnabled) {
        return;
    }
    try {
        fs.mkdirSync(path.dirname(VSX_DEBUG_LOG_PATH), { recursive: true });
        fs.appendFileSync(
            VSX_DEBUG_LOG_PATH,
            `${Date.now()} [pid ${process.pid}] ${msg}\n`,
            'utf8',
        );
    } catch {
        vsxDebugLogEnabled = false;
    }
}

/** Resolve the .eta program path for a Run/Debug command, falling back to the active editor. */
function resolveEtaTarget(uri?: Uri): string | undefined {
    if (uri && uri.scheme === 'file') return uri.fsPath;
    const editor = window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'eta') {
        window.showWarningMessage('Open an .eta file first.');
        return undefined;
    }
    return editor.document.fileName;
}

/** Resolve ETA_MODULE_PATH from the shared setting or env. */
function resolveModulePath(): string {
    return workspace.getConfiguration('eta').get<string>('modulePath', '')
        || process.env['ETA_MODULE_PATH']
        || '';
}

export function activate(context: ExtensionContext) {
    extensionCtx = context;
    outputChannel = window.createOutputChannel('Eta Language', { log: true });
    programOutputChannel = window.createOutputChannel('Eta Output', { log: true });
    context.subscriptions.push(outputChannel, programOutputChannel);
    log('Eta extension activating...');
    log(`Eta extension version: ${context.extension.packageJSON.version}`);
    logToFile(`activate version=${context.extension.packageJSON.version} logPath=${VSX_DEBUG_LOG_PATH}`);

    // -- GC Roots tree view -------------------------------------------
    gcRootsProvider = new GCRootsTreeProvider();
    const gcRootsView = window.createTreeView('etaGCRoots', {
        treeDataProvider: gcRootsProvider,
        showCollapseAll: true,
    });
    gcRootsViewVisible = gcRootsView.visible;
    context.subscriptions.push(
        gcRootsView,
        gcRootsView.onDidChangeVisibility(e => {
            gcRootsViewVisible = e.visible;
        }),
    );

    // -- Disassembly virtual document provider -----------------------
    disasmProvider = new DisassemblyContentProvider();
    context.subscriptions.push(
        workspace.registerTextDocumentContentProvider('eta-disasm', disasmProvider),
    );

    // -- Disassembly tree view (debug sidebar) ----------------------------
    disasmTreeProvider = new DisassemblyTreeProvider();
    const disasmView = window.createTreeView('etaDisassembly', {
        treeDataProvider: disasmTreeProvider,
        showCollapseAll: false,
    });
    disasmViewVisible = disasmView.visible;
    context.subscriptions.push(
        disasmView,
        disasmView.onDidChangeVisibility(e => {
            disasmViewVisible = e.visible;
        }),
    );

    // -- Child process tree view (debug sidebar) --------------------------
    childProcProvider = new ChildProcessTreeProvider();
    const childProcView = window.createTreeView('etaChildProcesses', {
        treeDataProvider: childProcProvider,
        showCollapseAll: false,
    });
    childProcViewVisible = childProcView.visible;
    context.subscriptions.push(
        childProcView,
        childProcView.onDidChangeVisibility(e => {
            childProcViewVisible = e.visible;
        }),
    );

    // -- Always register the debug adapter --------------------------
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
        commands.registerCommand('eta.runFile', (uri?: Uri) => {
            const target = resolveEtaTarget(uri);
            if (!target) return;
            debug.startDebugging(undefined, {
                type: 'eta',
                request: 'launch',
                name: 'Run Eta file',
                program: target,
            });
        }),
        commands.registerCommand('eta.debugFile', (uri?: Uri, stopOnEntry?: boolean) => {
            const target = resolveEtaTarget(uri);
            if (!target) return;
            debug.startDebugging(undefined, {
                type: 'eta',
                request: 'launch',
                name: 'Debug Eta file',
                program: target,
                stopOnEntry: stopOnEntry === true,
            });
        }),
        commands.registerCommand('eta.runTestFile', async (uri?: Uri) => {
            const target = uri ?? window.activeTextEditor?.document.uri;
            if (!target) {
                window.showWarningMessage('Open a *.test.eta file first.');
                return;
            }
            await runTestsForUri(target);
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
        commands.registerCommand('eta.disassembly.gotoCallee', async (funcIndex: number) => {
            if (!Number.isInteger(funcIndex) || funcIndex < 0) {
                return;
            }
            const target = disasmTreeProvider.findCalleeHeaderLine(funcIndex);
            if (!target) {
                window.showInformationMessage(`Callee function #${funcIndex} not found in current disassembly.`);
                return;
            }
            const uri = Uri.parse(target.uri);
            const doc = await workspace.openTextDocument(uri);
            const ed = await window.showTextDocument(doc, {
                preview: true,
                preserveFocus: false,
                viewColumn: ViewColumn.Beside,
            });
            const line = Math.max(0, Math.min(target.line, Math.max(0, doc.lineCount - 1)));
            const range = new Range(line, 0, line, doc.lineAt(line).text.length);
            ed.selection = new Selection(range.start, range.start);
            ed.revealRange(range, TextEditorRevealType.InCenterIfOutsideViewport);
        }),
    );

    // -- Test Explorer -------------------------------------------
    registerTestController(context);

    // -- Editor providers (inline values, hover-eval, code lens, links) --
    const etaSelector = { scheme: 'file', language: 'eta' } as const;
    const inlineValuesEnabled = workspace.getConfiguration('eta.debug')
        .get<boolean>('inlineValuesEnabled', false);
    log(`Eta inline values enabled: ${inlineValuesEnabled}`);
    logToFile(`activate inlineValuesEnabled=${inlineValuesEnabled}`);
    const editorRegistrations = [
        languages.registerEvaluatableExpressionProvider(etaSelector, new EtaEvaluatableExpressionProvider()),
        languages.registerCodeLensProvider(etaSelector, new EtaCodeLensProvider()),
        languages.registerDocumentLinkProvider(etaSelector, new EtaDocumentLinkProvider()),
    ];
    if (inlineValuesEnabled) {
        editorRegistrations.unshift(
            languages.registerInlineValuesProvider(etaSelector, new EtaInlineValuesProvider()),
        );
    }
    context.subscriptions.push(...editorRegistrations);

    // -- Watch for configuration changes -------------------------------------------
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

    // -- LSP setup --------------------------------------------------
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

// -- Debug configuration provider -------------------------------------------------

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

// -- Debug adapter tracker -----------------------------------------------------

class EtaDebugAdapterTracker implements DebugAdapterTracker {
    private stoppedRefreshRunning = false;
    private stoppedRefreshQueued = false;
    private programOutputBuffer = '';
    private programOutputFlushTimer: NodeJS.Timeout | undefined;
    private static readonly PROGRAM_OUTPUT_FLUSH_MS = 30;
    private static readonly PROGRAM_OUTPUT_MAX_BUFFER = 8192;
    private static readonly DISASM_INSTR_RE = /^\s*\d+\s*:\s+\S+/m;

    constructor(
        private readonly channel: LogOutputChannel,
        private readonly programChannel: LogOutputChannel,
    ) {}

    private flushProgramOutput(): void {
        if (!this.programOutputBuffer) {
            return;
        }
        this.programChannel.append(this.programOutputBuffer);
        this.programOutputBuffer = '';
    }

    private scheduleProgramFlush(): void {
        if (this.programOutputFlushTimer) {
            return;
        }
        this.programOutputFlushTimer = setTimeout(() => {
            this.programOutputFlushTimer = undefined;
            this.flushProgramOutput();
        }, EtaDebugAdapterTracker.PROGRAM_OUTPUT_FLUSH_MS);
    }

    private queueProgramOutput(text: string): void {
        if (!text) {
            return;
        }
        this.programOutputBuffer += text;
        if (this.programOutputBuffer.length >= EtaDebugAdapterTracker.PROGRAM_OUTPUT_MAX_BUFFER) {
            if (this.programOutputFlushTimer) {
                clearTimeout(this.programOutputFlushTimer);
                this.programOutputFlushTimer = undefined;
            }
            this.flushProgramOutput();
            return;
        }
        this.scheduleProgramFlush();
    }

    onWillStartSession(): void {
        logToFile('tracker.onWillStartSession');
        this.channel.appendLine('[DAP] Debug session starting...');
        this.programChannel.clear();
        this.programChannel.show(true);
        if (this.programOutputFlushTimer) {
            clearTimeout(this.programOutputFlushTimer);
            this.programOutputFlushTimer = undefined;
        }
        this.programOutputBuffer = '';
    }

    onWillStopSession(): void {
        logToFile('tracker.onWillStopSession');
        this.channel.appendLine('[DAP] Debug session stopping...');
        if (this.programOutputFlushTimer) {
            clearTimeout(this.programOutputFlushTimer);
            this.programOutputFlushTimer = undefined;
        }
        this.flushProgramOutput();
        HeapInspectorPanel.disposeCurrent();
        childProcProvider?.notifySessionEnded();
    }

    onWillReceiveMessage(message: any): void {
        const cmd: string = message?.command ?? '';
        const args = message?.arguments;
        if (cmd) {
            const reqSeq = typeof message?.seq === 'number' ? message.seq : -1;
            logToFile(`tracker.onWillReceiveMessage cmd=${cmd} seq=${reqSeq}`);
            if (cmd === 'evaluate') {
                const expr = typeof args?.expression === 'string' ? args.expression : '';
                logToFile(`tracker.onWillReceiveMessage evaluate.expr.len=${expr.length}`);
            }
        }
        switch (cmd) {
            case 'initialize':
                this.channel.appendLine('[DAP->] initialize');
                break;
            case 'launch':
                this.channel.appendLine(`[DAP->] launch: program="${args?.program ?? '?'}"`);
                break;
            case 'setBreakpoints': {
                const srcPath: string = args?.source?.path ?? '?';
                const lines: number[] = (args?.breakpoints ?? []).map((b: any) => b.line as number);
                this.channel.appendLine(
                    `[DAP->] setBreakpoints: ${lines.length} bp(s) in "${srcPath}" lines=[${lines.join(',')}]`
                );
                break;
            }
            case 'configurationDone':
                this.channel.appendLine('[DAP->] configurationDone');
                break;
            case 'continue':
            case 'next':
            case 'stepIn':
            case 'stepOut':
            case 'pause':
            case 'disconnect':
                this.channel.appendLine(`[DAP->] ${cmd}`);
                break;
            default:
                if (cmd) { this.channel.appendLine(`[DAP->] ${cmd}`); }
        }
    }

    onDidSendMessage(message: any): void {
        const type: string  = message?.type  ?? '';
        const event: string = message?.event ?? '';

        if (type === 'event') {
            if (event === 'eta-output') {
                const text: string = message?.body?.text ?? '';
                this.queueProgramOutput(text);
            } else if (event === 'output') {
                const output: string = message?.body?.output ?? '';
                if (output) { this.channel.append(output); }
            } else if (event === 'initialized') {
                logToFile('tracker.onDidSendMessage event=initialized');
                this.channel.appendLine('[DAP<-] initialized (adapter ready; VS Code will now send setBreakpoints)');
            } else if (event === 'stopped') {
                const reason: string = message?.body?.reason ?? '?';
                const tid: number    = message?.body?.threadId ?? 0;
                logToFile(`tracker.onDidSendMessage event=stopped reason=${reason} threadId=${tid}`);
                this.channel.appendLine(`[DAP<-] stopped: reason="${reason}" threadId=${tid}`);
                const debugCfg = workspace.getConfiguration('eta.debug');
                const autoRefreshViewsOnStop = debugCfg.get<boolean>('autoRefreshViewsOnStop', false);
                const autoRefreshHeapOnStop = debugCfg.get<boolean>('autoRefreshHeapOnStop', false);
                const autoRefreshDisassemblyOnStop = debugCfg.get<boolean>('autoRefreshDisassemblyOnStop', true);
                const shouldQueueRefresh = autoRefreshViewsOnStop
                    || autoRefreshHeapOnStop
                    || autoRefreshDisassemblyOnStop;
                logToFile(
                    `tracker.stopped refreshFlags all=${autoRefreshViewsOnStop} heap=${autoRefreshHeapOnStop} disasm=${autoRefreshDisassemblyOnStop} queue=${shouldQueueRefresh}`,
                );
                if (shouldQueueRefresh) {
                    this.queueStoppedRefresh();
                }
            } else if (event === 'continued') {
                logToFile('tracker.onDidSendMessage event=continued');
                this.channel.appendLine('[DAP<-] continued');
            } else if (event === 'breakpoint') {
                const bp  = message?.body?.breakpoint ?? {};
                const why = message?.body?.reason ?? '?';
                logToFile(`tracker.onDidSendMessage event=breakpoint reason=${why} id=${bp.id ?? '?'} line=${bp.line ?? '?'}`);
                this.channel.appendLine(
                    `[DAP<-] breakpoint ${why}: id=${bp.id} verified=${bp.verified} line=${bp.line}`
                );
            } else if (event === 'terminated') {
                logToFile('tracker.onDidSendMessage event=terminated');
                this.channel.appendLine('[DAP<-] terminated');
            } else if (event === 'exited') {
                logToFile(`tracker.onDidSendMessage event=exited code=${message?.body?.exitCode ?? '?'}`);
                this.channel.appendLine(`[DAP<-] exited: code=${message?.body?.exitCode ?? '?'}`);
            }
        } else if (type === 'response') {
            const cmd     = message?.command ?? '';
            const success = message?.success ?? false;
            if (cmd) {
                logToFile(`tracker.onDidSendMessage response cmd=${cmd} success=${success}`);
                if (cmd === 'evaluate' && success) {
                    const result = typeof message?.body?.result === 'string' ? message.body.result : '';
                    logToFile(`tracker.onDidSendMessage evaluate.result.len=${result.length}`);
                }
            }
            if (!success) {
                this.channel.appendLine(
                    `[DAP<-] ERROR response to "${cmd}": ${JSON.stringify(message?.body ?? {})}`
                );
            } else if (cmd === 'initialize') {
                const caps = message?.body ?? {};
                this.channel.appendLine(
                    `[DAP<-] initialize OK - supportsConfigurationDone=${caps.supportsConfigurationDoneRequest}`
                );
            }
        }
    }

    private queueStoppedRefresh(): void {
        logToFile(`tracker.queueStoppedRefresh running=${this.stoppedRefreshRunning} queued=${this.stoppedRefreshQueued}`);
        this.stoppedRefreshQueued = true;
        if (this.stoppedRefreshRunning) {
            return;
        }
        this.stoppedRefreshRunning = true;
        void (async () => {
            try {
                // Defer custom requests until after the stopped event callback unwinds.
                await new Promise<void>(resolve => setTimeout(resolve, 0));
                while (this.stoppedRefreshQueued) {
                    this.stoppedRefreshQueued = false;
                    logToFile('tracker.refreshStoppedViews.begin');
                    await this.refreshStoppedViews();
                    logToFile('tracker.refreshStoppedViews.end');
                }
            } finally {
                this.stoppedRefreshRunning = false;
                logToFile('tracker.queueStoppedRefresh.done');
            }
        })();
    }

    private async refreshStoppedViews(): Promise<void> {
        const session = debug.activeDebugSession;
        logToFile(`tracker.refreshStoppedViews.activeSession type=${session?.type ?? 'none'}`);
        if (!session || session.type !== 'eta') {
            return;
        }

        const autoRefreshHeap = workspace.getConfiguration('eta.debug')
            .get<boolean>('autoRefreshHeapOnStop', false);
        const autoShowHeap = workspace.getConfiguration('eta.debug')
            .get<boolean>('autoShowHeap', false);
        if (autoShowHeap && autoRefreshHeap && extensionCtx && !HeapInspectorPanel.current()) {
            HeapInspectorPanel.createOrShow(extensionCtx);
        }
        const heapPanel = HeapInspectorPanel.current();
        const heapPanelVisible = heapPanel?.isVisible() ?? false;
        const shouldFetchHeapPanel = autoRefreshHeap && heapPanelVisible;
        const shouldFetchRoots = autoRefreshHeap && gcRootsViewVisible;
        const shouldFetchHeapSnapshot = shouldFetchHeapPanel || shouldFetchRoots;

        const autoShowDisasm = workspace.getConfiguration('eta.debug')
            .get<boolean>('autoShowDisassembly', false);
        const autoRefreshDisasmOnStop = workspace.getConfiguration('eta.debug')
            .get<boolean>('autoRefreshDisassemblyOnStop', true);
        const autoRefreshViewsOnStop = workspace.getConfiguration('eta.debug')
            .get<boolean>('autoRefreshViewsOnStop', false);
        const disasmDocVisible = window.visibleTextEditors.some(
            ed => ed.document.uri.scheme === 'eta-disasm',
        );
        const providerScope = disasmProvider ? disasmProvider.currentScope() : 'current';
        // When disassembly auto-refresh is enabled, refresh on every stop so
        // the first breakpoint has data even before the UI view becomes visible.
        const shouldRefreshDisasmCurrent = autoRefreshDisasmOnStop;
        const shouldFetchChildren = autoRefreshViewsOnStop && childProcViewVisible;
        logToFile(
            `tracker.refreshStoppedViews.flags heap=${shouldFetchHeapSnapshot} disasm=${shouldRefreshDisasmCurrent} children=${shouldFetchChildren}`,
        );

        const heapPromise = shouldFetchHeapSnapshot
            ? session.customRequest('eta/heapSnapshot', {
                includeKinds: shouldFetchHeapPanel,
                includeRoots: true,
                maxObjectsScanned: shouldFetchHeapPanel ? 120000 : 0,
                maxKindRows: shouldFetchHeapPanel ? 200 : 0,
                maxRootsPerCategory: 600,
            }) as Promise<HeapSnapshot>
            : Promise.resolve(undefined);

        const disasmPromise = shouldRefreshDisasmCurrent
            ? session.customRequest('eta/disassemble', { scope: 'current' }) as Promise<DisassemblyResult>
            : Promise.resolve(undefined);

        const childrenPromise = shouldFetchChildren
            ? session.customRequest('eta/childProcesses') as Promise<{ children: ChildProcessInfo[] }>
            : Promise.resolve(undefined);

        const [heapResult, currentDisasmResult, childrenResult] = await Promise.allSettled([
            heapPromise,
            disasmPromise,
            childrenPromise,
        ]);
        logToFile(
            `tracker.refreshStoppedViews.settled heap=${heapResult.status} disasm=${currentDisasmResult.status} children=${childrenResult.status}`,
        );

        if (shouldFetchHeapSnapshot) {
            if (heapResult.status === 'fulfilled') {
                const snap = heapResult.value as HeapSnapshot;
                if (shouldFetchRoots) {
                    gcRootsProvider?.applySnapshot(snap);
                }
                if (shouldFetchHeapPanel && heapPanel) {
                    heapPanel.applySnapshot(snap);
                }
            } else {
                if (shouldFetchRoots) {
                    gcRootsProvider?.applySnapshot(undefined);
                }
                const msg = heapResult.reason instanceof Error
                    ? heapResult.reason.message
                    : String(heapResult.reason);
                if (shouldFetchHeapPanel && heapPanel) {
                    if (/must be paused/i.test(msg)) {
                        heapPanel.showIdle('Pause the VM (breakpoint or step) to inspect the heap.');
                    } else {
                        heapPanel.showError(msg);
                    }
                }
                this.channel.appendLine(`[DAP] eta/heapSnapshot refresh failed: ${msg}`);
            }
        }

        if (shouldRefreshDisasmCurrent) {
            if (currentDisasmResult.status === 'fulfilled') {
                let disasmResult = currentDisasmResult.value as DisassemblyResult;
                // First-stop race hardening: if the first response carries no
                // instruction rows, retry once shortly after stop handling settles.
                if (!EtaDebugAdapterTracker.DISASM_INSTR_RE.test(disasmResult?.text ?? '')) {
                    try {
                        await new Promise<void>(resolve => setTimeout(resolve, 20));
                        const retry = await session.customRequest(
                            'eta/disassemble',
                            { scope: 'current' },
                        ) as DisassemblyResult;
                        if (EtaDebugAdapterTracker.DISASM_INSTR_RE.test(retry?.text ?? '')) {
                            disasmResult = retry;
                            logToFile('tracker.refreshStoppedViews.disasmRetry used');
                        }
                    } catch (err: any) {
                        logToFile(`tracker.refreshStoppedViews.disasmRetry failed err=${err?.message ?? String(err)}`);
                    }
                }
                if (disasmViewVisible) {
                    disasmTreeProvider?.applyResult(disasmResult);
                }
                if (disasmProvider && providerScope === 'current') {
                    disasmProvider.applyResult(disasmResult, 'current');
                }
            } else {
                const msg = currentDisasmResult.reason instanceof Error
                    ? currentDisasmResult.reason.message
                    : String(currentDisasmResult.reason);
                if (disasmViewVisible) {
                    disasmTreeProvider?.applyResult(undefined);
                }
                if (disasmProvider && providerScope === 'current') {
                    disasmProvider.applyResult(undefined, 'current');
                }
                this.channel.appendLine(`[DAP] eta/disassemble(current) refresh failed: ${msg}`);
            }
        }

        if (disasmProvider && autoShowDisasm && autoRefreshDisasmOnStop) {
            await autoShowDisassemblyOnStop(disasmProvider, true);
        }

        if (shouldFetchChildren) {
            if (childrenResult.status === 'fulfilled') {
                const payload = childrenResult.value as { children: ChildProcessInfo[] } | undefined;
                childProcProvider?.updateChildren(payload?.children ?? []);
            } else {
                childProcProvider?.updateChildren([]);
                const msg = childrenResult.reason instanceof Error
                    ? childrenResult.reason.message
                    : String(childrenResult.reason);
                this.channel.appendLine(`[DAP] eta/childProcesses refresh failed: ${msg}`);
            }
        }
    }

    onError(error: Error): void {
        logToFile(`tracker.onError ${error.message}`);
        if (this.programOutputFlushTimer) {
            clearTimeout(this.programOutputFlushTimer);
            this.programOutputFlushTimer = undefined;
        }
        this.flushProgramOutput();
        this.channel.appendLine(`[DAP] Adapter error: ${error.message}`);
    }

    onExit(code: number | undefined, signal: string | undefined): void {
        logToFile(`tracker.onExit code=${code ?? 'null'} signal=${signal ?? 'null'}`);
        if (this.programOutputFlushTimer) {
            clearTimeout(this.programOutputFlushTimer);
            this.programOutputFlushTimer = undefined;
        }
        this.flushProgramOutput();
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
