import * as path from 'path';
import * as fs from 'fs';
import { workspace, ExtensionContext, window } from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Executable,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

function findServerBinary(context: ExtensionContext): string | undefined {
    // 1. Check user configuration
    const config = workspace.getConfiguration('eta.lsp');
    const configPath = config.get<string>('serverPath', '');
    if (configPath && fs.existsSync(configPath)) {
        return configPath;
    }

    // 2. Check bundled binary inside the extension directory
    //    (populated when packaging a release .vsix)
    const bundledCandidates = [
        path.join(context.extensionPath, 'bin', 'eta_lsp'),
        path.join(context.extensionPath, 'bin', 'eta_lsp.exe'),
    ];
    for (const c of bundledCandidates) {
        if (fs.existsSync(c)) {
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
                if (fs.existsSync(c)) {
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

    const run: Executable = { command: serverPath };
    const debug: Executable = { command: serverPath };

    const serverOptions: ServerOptions = { run, debug };

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
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}

