import * as fs from 'fs';
import * as path from 'path';
import { ExtensionContext, workspace } from 'vscode';

export interface EtaBinaries {
    lsp?: string;
    dap?: string;
    test?: string;
    etac?: string;
    etai?: string;
}

function isFile(p: string): boolean {
    try { return fs.statSync(p).isFile(); } catch { return false; }
}

function executableName(base: string): string {
    return process.platform === 'win32' ? `${base}.exe` : base;
}

function resolveConfigBinary(configPath: string, names: string[]): string | undefined {
    if (!configPath) return undefined;
    if (isFile(configPath)) return configPath;
    for (const name of names) {
        const candidate = path.join(configPath, name);
        if (isFile(candidate)) return candidate;
    }
    return undefined;
}

function normalizeKey(p: string): string {
    return process.platform === 'win32' ? p.toLowerCase() : p;
}

function pushUnique(out: string[], seen: Set<string>, p: string): void {
    const key = normalizeKey(path.normalize(p));
    if (seen.has(key)) return;
    seen.add(key);
    out.push(p);
}

function configuredSearchRoots(): string[] {
    const values = workspace.getConfiguration('eta.binaries').get<string[]>('searchPaths', []);
    if (!Array.isArray(values) || values.length === 0) return [];

    const roots: string[] = [];
    const folders = workspace.workspaceFolders ?? [];
    if (folders.length === 0) {
        for (const raw of values) {
            if (typeof raw === 'string' && raw.trim().length > 0) roots.push(raw.trim());
        }
        return roots;
    }

    for (const raw of values) {
        if (typeof raw !== 'string') continue;
        const trimmed = raw.trim();
        if (!trimmed) continue;

        let expanded = trimmed;
        if (expanded.includes('${workspaceFolder}')) {
            for (const folder of folders) {
                roots.push(expanded.replaceAll('${workspaceFolder}', folder.uri.fsPath));
            }
            continue;
        }
        roots.push(expanded);
    }

    return roots;
}

function defaultWorkspaceCandidates(relativePath: string): string[] {
    const out: string[] = [];
    const folders = workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        out.push(path.join(folder.uri.fsPath, relativePath));
    }
    return out;
}

function discoverBinary(
    configPath: string,
    names: string[],
    relativeCandidates: string[],
    extensionPath: string,
): string | undefined {
    const configured = resolveConfigBinary(configPath, names);
    if (configured) return configured;

    for (const name of names) {
        const bundled = path.join(extensionPath, 'bin', name);
        if (isFile(bundled)) return bundled;
    }

    const seen = new Set<string>();
    const candidates: string[] = [];

    for (const rel of relativeCandidates) {
        for (const full of defaultWorkspaceCandidates(rel)) {
            pushUnique(candidates, seen, full);
        }
    }

    const configuredRoots = configuredSearchRoots();
    for (const root of configuredRoots) {
        if (isFile(root)) {
            pushUnique(candidates, seen, root);
            continue;
        }
        for (const name of names) {
            pushUnique(candidates, seen, path.join(root, name));
        }
    }

    for (const candidate of candidates) {
        if (isFile(candidate)) return candidate;
    }

    return names[0];
}

export function discoverBinaries(context: ExtensionContext): EtaBinaries {
    const lspNames = [executableName('eta_lsp')];
    const dapNames = [executableName('eta_dap')];
    const testNames = [executableName('eta_test')];
    const etacNames = [executableName('etac')];
    const etaiNames = [executableName('etai')];

    const lsp = discoverBinary(
        workspace.getConfiguration('eta.lsp').get<string>('serverPath', '').trim(),
        lspNames,
        [
            path.join('out', 'wsl-clang-release', 'eta', 'lsp', lspNames[0]),
            path.join('out', 'build', 'eta', 'lsp', lspNames[0]),
            path.join('build', 'eta', 'lsp', lspNames[0]),
            path.join('out', 'msvc-release', 'eta', 'lsp', lspNames[0]),
            path.join('build-release', 'eta', 'lsp', lspNames[0]),
        ],
        context.extensionPath,
    );

    const dapFromConfig = resolveConfigBinary(
        workspace.getConfiguration('eta.dap').get<string>('executablePath', '').trim(),
        dapNames,
    );
    let dap: string | undefined;
    if (dapFromConfig) {
        dap = dapFromConfig;
    } else if (lsp && isFile(lsp)) {
        const nextToLsp = path.join(path.dirname(lsp), dapNames[0]);
        if (isFile(nextToLsp)) {
            dap = nextToLsp;
        }
    }
    if (!dap) {
        dap = discoverBinary(
            '',
            dapNames,
            [
                path.join('out', 'wsl-clang-release', 'eta', 'dap', dapNames[0]),
                path.join('out', 'build', 'eta', 'dap', dapNames[0]),
                path.join('build', 'eta', 'dap', dapNames[0]),
                path.join('out', 'msvc-release', 'eta', 'dap', dapNames[0]),
                path.join('build-release', 'eta', 'dap', dapNames[0]),
            ],
            context.extensionPath,
        );
    }

    const testFromConfig = resolveConfigBinary(
        workspace.getConfiguration('eta.test').get<string>('runnerPath', '').trim(),
        testNames,
    );
    let test: string | undefined;
    if (testFromConfig) {
        test = testFromConfig;
    } else if (lsp && isFile(lsp)) {
        const nextToLsp = path.join(path.dirname(lsp), testNames[0]);
        if (isFile(nextToLsp)) {
            test = nextToLsp;
        }
    }
    if (!test && dap && isFile(dap)) {
        const nextToDap = path.join(path.dirname(dap), testNames[0]);
        if (isFile(nextToDap)) {
            test = nextToDap;
        }
    }
    if (!test) {
        test = discoverBinary(
            '',
            testNames,
            [
                path.join('out', 'wsl-clang-release', 'eta', 'test_runner', testNames[0]),
                path.join('out', 'build', 'eta', 'test_runner', testNames[0]),
                path.join('build', 'eta', 'test_runner', testNames[0]),
                path.join('out', 'msvc-release', 'eta', 'test_runner', testNames[0]),
                path.join('build-release', 'eta', 'test_runner', testNames[0]),
            ],
            context.extensionPath,
        );
    }

    const etac = discoverBinary(
        '',
        etacNames,
        [
            path.join('out', 'wsl-clang-release', 'eta', 'compiler', etacNames[0]),
            path.join('out', 'build', 'eta', 'compiler', etacNames[0]),
            path.join('build', 'eta', 'compiler', etacNames[0]),
            path.join('out', 'msvc-release', 'eta', 'compiler', etacNames[0]),
            path.join('build-release', 'eta', 'compiler', etacNames[0]),
        ],
        context.extensionPath,
    );

    const etai = discoverBinary(
        '',
        etaiNames,
        [
            path.join('out', 'wsl-clang-release', 'eta', 'interpreter', etaiNames[0]),
            path.join('out', 'build', 'eta', 'interpreter', etaiNames[0]),
            path.join('build', 'eta', 'interpreter', etaiNames[0]),
            path.join('out', 'msvc-release', 'eta', 'interpreter', etaiNames[0]),
            path.join('build-release', 'eta', 'interpreter', etaiNames[0]),
        ],
        context.extensionPath,
    );

    return { lsp, dap, test, etac, etai };
}
