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

let client: LanguageClient | undefined;
let outputChannel: OutputChannel;

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

function findServerBinary(context: ExtensionContext): string | undefined {
    log('[Checking LSP Binary]');
    log(`  Platform: ${process.platform}`);

    // 1. Check user configuration
    const config = workspace.getConfiguration('eta.lsp');
    const configPath = config.get<string>('serverPath', '').trim();
    if (configPath) {
        log(`  - Searching config: "${configPath}"`);
        if (checkPath('config path (direct)', configPath)) { return configPath; }
        for (const name of ['eta_lsp.exe', 'eta_lsp']) {
            const candidate = path.join(configPath, name);
            if (checkPath(`config path (${name})`, candidate)) { return candidate; }
        }
        const msg = `Eta LSP: configured serverPath "${configPath}" does not point to a valid executable.`;
        log(`  ERROR: ${msg}`);
        window.showWarningMessage(msg);
        return undefined;
    } else {
        log(`  - Searching config: "" (not set)`);
    }

    // 2. Check bundled binary inside the extension directory
    const bundledCandidates = [
        path.join(context.extensionPath, 'bin', 'eta_lsp'),
        path.join(context.extensionPath, 'bin', 'eta_lsp.exe'),
    ];
    for (const c of bundledCandidates) {
        if (checkPath('bundled', c)) { return c; }
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

    // 4. Fall back to PATH (hope it's installed)
    log(`  - Falling back to PATH: eta_lsp`);
    return 'eta_lsp';
}

function findInterpreterBinary(lspPath: string | undefined, context: ExtensionContext): string {
    const exe = process.platform === 'win32' ? 'etai.exe' : 'etai';
    log('[Checking Interpreter Binary]');
    log(`  Platform: ${process.platform}, looking for: ${exe}`);

    // 1. Next to the LSP binary (most common case after an install)
    if (lspPath && lspPath !== 'eta_lsp' && lspPath !== 'eta_lsp.exe') {
        const candidate = path.join(path.dirname(lspPath), exe);
        if (checkPath('next to LSP binary', candidate)) { return candidate; }
    }

    // 2. Bundled alongside the extension
    const bundled = path.join(context.extensionPath, 'bin', exe);
    if (checkPath('bundled', bundled)) { return bundled; }

    // 3. Workspace build output
    const workspaceFolders = workspace.workspaceFolders;
    if (workspaceFolders) {
        for (const folder of workspaceFolders) {
            for (const rel of [
                path.join('out', 'wsl-clang-release', 'eta', 'interpreter', exe),
                path.join('out', 'build', 'eta', 'interpreter', exe),
                path.join('build', 'eta', 'interpreter', exe),
                path.join('build-release', 'eta', 'interpreter', exe),
                path.join('out', 'msvc-release', 'eta', 'interpreter', exe),
            ]) {
                const c = path.join(folder.uri.fsPath, rel);
                if (checkPath('workspace', c)) { return c; }
            }
        }
    }

    // 4. Check user-configured serverPath directory (may contain etai too)
    const configDir = workspace.getConfiguration('eta.lsp').get<string>('serverPath', '').trim();
    if (configDir) {
        const c = path.join(configDir, exe);
        if (checkPath('config dir', c)) { return c; }
    }

    // 5. Fall back to PATH
    log(`  - Falling back to PATH: ${exe}`);
    return exe;
}

function findDapBinary(etaiPath: string | undefined, lspPath: string | undefined, context: ExtensionContext): string {
    const exe = process.platform === 'win32' ? 'eta_dap.exe' : 'eta_dap';
    log('[Checking DAP Binary]');
    log(`  Platform: ${process.platform}, looking for: ${exe}`);

    // 1. User configuration
    const configPath = workspace.getConfiguration('eta.dap').get<string>('executablePath', '').trim();
    if (configPath) {
        if (checkPath('config path (direct)', configPath)) { return configPath; }
        const candidate = path.join(configPath, exe);
        if (checkPath(`config path (${exe})`, candidate)) { return candidate; }
    }

    // 2. Next to etai (most common after an install)
    if (etaiPath && etaiPath !== 'etai' && etaiPath !== 'etai.exe') {
        const candidate = path.join(path.dirname(etaiPath), exe);
        if (checkPath('next to etai', candidate)) { return candidate; }
    }

    // 3. Next to the LSP binary
    if (lspPath && lspPath !== 'eta_lsp' && lspPath !== 'eta_lsp.exe') {
        const candidate = path.join(path.dirname(lspPath), exe);
        if (checkPath('next to LSP binary', candidate)) { return candidate; }
    }

    // 4. Bundled alongside the extension
    const bundled = path.join(context.extensionPath, 'bin', exe);
    if (checkPath('bundled', bundled)) { return bundled; }

    // 5. Workspace build output
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

    // 6. Fall back to PATH
    log(`  - Falling back to PATH: ${exe}`);
    return exe;
}

function validateAndLogServerPath(configPath: string): void {
    log('[Validating eta.lsp.serverPath]');
    if (!configPath) {
        log('  (not set — automatic discovery will be used)');
        return;
    }
    const direct = isFile(configPath);
    if (direct) {
        log(`  OK: "${configPath}" is a valid executable.`);
        return;
    }
    let found = false;
    for (const name of ['eta_lsp.exe', 'eta_lsp']) {
        const candidate = path.join(configPath, name);
        if (isFile(candidate)) {
            log(`  OK: found "${candidate}" inside the configured directory.`);
            found = true;
            break;
        }
    }
    if (!found) {
        const msg = `Eta: eta.lsp.serverPath "${configPath}" is not a valid executable or directory containing eta_lsp.`;
        log(`  WARNING: ${msg}`);
        window.showWarningMessage(msg);
    }
}

export function activate(context: ExtensionContext) {
    outputChannel = window.createOutputChannel('Eta Language');
    context.subscriptions.push(outputChannel);
    outputChannel.show(true);   // reveal, but don't steal focus
    log('Eta extension activating...');

    // ── Always register the debug adapter ──────────────────────────
    const serverPath = findServerBinary(context);
    const etaiPath   = findInterpreterBinary(serverPath, context);
    const dapPath    = findDapBinary(etaiPath, serverPath, context);

    log(`[Summary]`);
    log(`  LSP binary  : ${serverPath ?? '(none)'}`);
    log(`  etai binary : ${etaiPath}`);
    log(`  DAP binary  : ${dapPath}`);

    const factory = new EtaDebugAdapterFactory(dapPath, etaiPath, outputChannel);
    context.subscriptions.push(
        debug.registerDebugAdapterDescriptorFactory('eta', factory),
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
    );

    // ── Watch for configuration changes ────────────────────────────
    context.subscriptions.push(
        workspace.onDidChangeConfiguration(e => {
            if (e.affectsConfiguration('eta.lsp.serverPath')) {
                const newPath = workspace.getConfiguration('eta.lsp').get<string>('serverPath', '').trim();
                validateAndLogServerPath(newPath);
            }
        })
    );

    // ── LSP setup — can be disabled or unavailable independently ───
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

    // Build environment: inherit process env and inject ETA_MODULE_PATH if configured
    const config = workspace.getConfiguration('eta.lsp');
    const modulePath = config.get<string>('modulePath', '') || process.env['ETA_MODULE_PATH'] || '';
    const serverEnv: NodeJS.ProcessEnv = { ...process.env };
    if (modulePath) {
        serverEnv['ETA_MODULE_PATH'] = modulePath;
        log(`  ETA_MODULE_PATH : ${modulePath}`);
    }

    log(`[Starting LSP]`);
    log(`  Command: ${serverPath}`);

    const run: Executable = { command: serverPath, options: { env: serverEnv } };
    const debug_exec: Executable = { command: serverPath, options: { env: serverEnv } };

    const serverOptions: ServerOptions = { run, debug: debug_exec };

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

// ── Debug configuration provider ─────────────────────────────────────────

class EtaDebugConfigurationProvider implements DebugConfigurationProvider {
    // Called when F5 / "Run" is pressed and there is no launch.json
    resolveDebugConfiguration(
        _folder: WorkspaceFolder | undefined,
        config: DebugConfiguration,
        _token?: CancellationToken,
    ): DebugConfiguration | undefined {
        // No launch.json at all: VS Code passes an empty config object
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

        // launch.json exists but program is missing
        if (!config.program) {
            window.showWarningMessage(
                'Set "program" in your launch.json to the .eta file you want to run (e.g. "${file}").'
            );
            return undefined;
        }

        return config;
    }
}

// ── Debug adapter ─────────────────────────────────────────────────────────

class EtaDebugAdapterFactory implements DebugAdapterDescriptorFactory {
    constructor(
        private readonly dapPath: string,
        private readonly etaiPath: string,
        private readonly channel: OutputChannel,
    ) {}

    createDebugAdapterDescriptor(_session: DebugSession): DebugAdapterDescriptor {
        // Build environment for the DAP process (ETA_MODULE_PATH etc.)
        const config = workspace.getConfiguration('eta.lsp');
        const modulePath = config.get<string>('modulePath', '') || process.env['ETA_MODULE_PATH'] || '';
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
