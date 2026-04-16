/**
 * testController.ts — Eta VS Code Test Explorer integration
 *
 * Implements the VS Code Test Explorer API for *.test.eta files.
 * Spawns eta-test per file (for isolation), parses TAP 13 output,
 * and reports pass/fail back to the Test Explorer UI.
 */

import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import * as cp from 'child_process';

// ---------------------------------------------------------------------------
// Binary resolution
// ---------------------------------------------------------------------------

function isFile(p: string): boolean {
    try { return fs.statSync(p).isFile(); } catch { return false; }
}

function findTestRunner(context: vscode.ExtensionContext): string | undefined {
    const exe = process.platform === 'win32' ? 'eta_test.exe' : 'eta_test';

    // 1. User config
    const cfg = vscode.workspace.getConfiguration('eta.test');
    const cfgPath = cfg.get<string>('runnerPath', '').trim();
    if (cfgPath) {
        if (isFile(cfgPath)) return cfgPath;
        const c = path.join(cfgPath, exe);
        if (isFile(c)) return c;
    }

    // 2. Bundled next to the extension
    const bundled = path.join(context.extensionPath, 'bin', exe);
    if (isFile(bundled)) return bundled;

    // 3. Workspace build dirs
    const folders = vscode.workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        for (const rel of [
            path.join('out', 'wsl-clang-release', 'eta', 'test_runner', exe),
            path.join('out', 'build', 'eta', 'test_runner', exe),
            path.join('build', 'eta', 'test_runner', exe),
            path.join('build-release', 'eta', 'test_runner', exe),
            path.join('out', 'msvc-release', 'eta', 'test_runner', exe),
        ]) {
            const c = path.join(folder.uri.fsPath, rel);
            if (isFile(c)) return c;
        }
    }

    // 4. PATH
    return exe;
}

// ---------------------------------------------------------------------------
// TAP parser
// ---------------------------------------------------------------------------

interface TapTestResult {
    ok: boolean;
    num: number;
    description: string;
    message?: string;   // failure message from YAML diagnostic block
}

function parseTap(output: string): TapTestResult[] {
    const results: TapTestResult[] = [];
    const lines = output.split(/\r?\n/);
    let current: TapTestResult | null = null;

    for (const line of lines) {
        if (line.startsWith('TAP version') || line.match(/^\d+\.\.\d+$/)) continue;

        const okMatch = line.match(/^ok\s+(\d+)\s*-?\s*(.*)/);
        if (okMatch) {
            current = { ok: true, num: parseInt(okMatch[1], 10), description: okMatch[2].trim() };
            results.push(current);
            continue;
        }

        const notOkMatch = line.match(/^not ok\s+(\d+)\s*-?\s*(.*)/);
        if (notOkMatch) {
            current = { ok: false, num: parseInt(notOkMatch[1], 10), description: notOkMatch[2].trim() };
            results.push(current);
            continue;
        }

        // YAML diagnostic lines
        if (current && line.startsWith('  message: ')) {
            current.message = line.slice(11).trim();
        }
    }

    return results;
}

// ---------------------------------------------------------------------------
// Test controller
// ---------------------------------------------------------------------------

let controller: vscode.TestController | undefined;
let fileWatcher: vscode.FileSystemWatcher | undefined;
const testItemMap = new Map<string, vscode.TestItem>();

export function registerTestController(context: vscode.ExtensionContext): void {
    controller = vscode.tests.createTestController('eta-test-controller', 'Eta Tests');
    context.subscriptions.push(controller);

    // Resolve handler: when a test item is expanded, populate children
    controller.resolveHandler = async (item) => {
        if (!item) {
            await discoverAllTests();
        }
    };

    // Run profile: run tests normally
    controller.createRunProfile(
        'Run',
        vscode.TestRunProfileKind.Run,
        (request, token) => runTests(request, token, context),
        /*isDefault=*/true,
    );

    // Watch for new/changed/deleted *.test.eta files
    fileWatcher = vscode.workspace.createFileSystemWatcher('**/*.test.eta');
    fileWatcher.onDidCreate(uri => addTestFile(uri));
    fileWatcher.onDidChange(uri => addTestFile(uri));
    fileWatcher.onDidDelete(uri => removeTestFile(uri));
    context.subscriptions.push(fileWatcher);

    // Initial discovery
    discoverAllTests();
}

async function discoverAllTests(): Promise<void> {
    if (!controller) return;
    const uris = await vscode.workspace.findFiles('**/*.test.eta', '**/node_modules/**');
    for (const uri of uris) {
        addTestFile(uri);
    }
}

function addTestFile(uri: vscode.Uri): void {
    if (!controller) return;
    const key = uri.toString();
    if (!testItemMap.has(key)) {
        const label = path.basename(uri.fsPath, '.eta');
        const item = controller.createTestItem(key, label, uri);
        item.canResolveChildren = false;
        controller.items.add(item);
        testItemMap.set(key, item);
    }
}

function removeTestFile(uri: vscode.Uri): void {
    if (!controller) return;
    const key = uri.toString();
    controller.items.delete(key);
    testItemMap.delete(key);
}

// ---------------------------------------------------------------------------
// Run handler
// ---------------------------------------------------------------------------

async function runTests(
    request: vscode.TestRunRequest,
    token: vscode.CancellationToken,
    context: vscode.ExtensionContext,
): Promise<void> {
    if (!controller) return;
    const run = controller.createTestRun(request);

    const etaTest = findTestRunner(context);
    const modulePath = vscode.workspace.getConfiguration('eta').get<string>('modulePath', '')
        || process.env['ETA_MODULE_PATH']
        || '';

    // Determine which items to run
    const items: vscode.TestItem[] = [];
    if (request.include) {
        request.include.forEach(i => items.push(i));
    } else {
        controller.items.forEach(i => items.push(i));
    }

    for (const item of items) {
        if (token.isCancellationRequested) break;
        if (!item.uri) continue;

        run.started(item);

        await new Promise<void>((resolve) => {
            const args: string[] = [];
            if (modulePath) { args.push('--path', modulePath); }
            args.push('--format', 'tap');
            args.push(item.uri!.fsPath);

            const env: NodeJS.ProcessEnv = { ...process.env };
            if (modulePath) env['ETA_MODULE_PATH'] = modulePath;

            let stdout = '';
            let stderr = '';

            const proc = cp.spawn(etaTest!, args, {
                env,
                cwd: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath,
            });

            proc.stdout.on('data', (d: Buffer) => { stdout += d.toString(); });
            proc.stderr.on('data', (d: Buffer) => { stderr += d.toString(); });

            const cancelSub = token.onCancellationRequested(() => proc.kill());

            proc.on('close', (code) => {
                cancelSub.dispose();

                if (stderr.trim()) {
                    run.appendOutput(stderr.replace(/\r?\n/g, '\r\n'));
                }

                const tapResults = parseTap(stdout);

                if (tapResults.length === 0) {
                    if (code !== 0) {
                        run.failed(item, new vscode.TestMessage(stderr || 'eta-test failed with no output'));
                    } else {
                        run.passed(item);
                    }
                } else {
                    // Create child items for each TAP test case
                    item.children.replace([]);
                    const childItems: vscode.TestItem[] = [];

                    for (const tr of tapResults) {
                        const childId = `${item.id}::${tr.num}`;
                        const child = controller!.createTestItem(childId, tr.description, item.uri);
                        childItems.push(child);

                        run.started(child);
                        if (tr.ok) {
                            run.passed(child);
                        } else {
                            const msg = new vscode.TestMessage(
                                tr.message ?? 'test failed'
                            );
                            run.failed(child, msg);
                        }
                    }

                    item.children.replace(childItems);

                    const allPassed = tapResults.every(r => r.ok);
                    if (allPassed) {
                        run.passed(item);
                    } else {
                        const failCount = tapResults.filter(r => !r.ok).length;
                        run.failed(item, new vscode.TestMessage(
                            `${failCount} of ${tapResults.length} tests failed`
                        ));
                    }
                }

                resolve();
            });

            proc.on('error', (err) => {
                cancelSub.dispose();
                run.failed(item, new vscode.TestMessage(
                    `Failed to start eta-test: ${err.message}\n` +
                    `Searched for: ${etaTest}\n` +
                    `Set eta.test.runnerPath in settings to the full path.`
                ));
                resolve();
            });
        });
    }

    run.end();
}

