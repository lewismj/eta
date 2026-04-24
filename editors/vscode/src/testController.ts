/**
 * testController.ts — Eta VS Code Test Explorer integration
 *
 * Implements the VS Code Test Explorer API for *.test.eta files.
 * Spawns eta-test per file (for isolation), parses TAP 13 output,
 * and reports pass/fail back to the Test Explorer UI.
 */

import * as vscode from 'vscode';
import * as path from 'path';
import * as cp from 'child_process';
import { discoverBinaries } from './binaries';

// ---------------------------------------------------------------------------
// TAP parser
// ---------------------------------------------------------------------------

interface TapTestResult {
    ok: boolean;
    num: number;
    description: string;
    message?: string;
    severity?: string;
    at?: string;
    expected?: string;
    actual?: string;
}

function parseTap(output: string): TapTestResult[] {
    const results: TapTestResult[] = [];
    const lines = output.split(/\r?\n/);
    let current: TapTestResult | null = null;
    let inYaml = false;

    for (const line of lines) {
        if (line.startsWith('TAP version') || line.match(/^\d+\.\.\d+$/)) continue;

        const okMatch = line.match(/^ok\s+(\d+)\s*-?\s*(.*)/);
        if (okMatch) {
            current = { ok: true, num: parseInt(okMatch[1], 10), description: okMatch[2].trim() };
            results.push(current);
            inYaml = false;
            continue;
        }

        const notOkMatch = line.match(/^not ok\s+(\d+)\s*-?\s*(.*)/);
        if (notOkMatch) {
            current = { ok: false, num: parseInt(notOkMatch[1], 10), description: notOkMatch[2].trim() };
            results.push(current);
            inYaml = false;
            continue;
        }

        if (!current) continue;

        if (line.trim() === '---') {
            inYaml = true;
            continue;
        }
        if (line.trim() === '...') {
            inYaml = false;
            continue;
        }

        if (!inYaml) continue;

        const yamlField = line.match(/^\s{2}([a-zA-Z0-9_-]+):\s*(.*)$/);
        if (!yamlField) continue;

        const key = yamlField[1];
        const value = yamlField[2].trim();
        switch (key) {
            case 'message':
                current.message = value;
                break;
            case 'severity':
                current.severity = value;
                break;
            case 'at':
                current.at = value;
                break;
            case 'expected':
                current.expected = value;
                break;
            case 'actual':
                current.actual = value;
                break;
            default:
                break;
        }
    }

    return results;
}

function parseTapAtLocation(at: string, fallbackUri: vscode.Uri): vscode.Location | undefined {
    const m = at.match(/^(.*):(\d+)(?::(\d+))?$/);
    if (!m) return undefined;

    const filePart = m[1].trim();
    const line = Math.max(1, parseInt(m[2], 10));
    const col = m[3] ? Math.max(1, parseInt(m[3], 10)) : 1;

    const baseDir = path.dirname(fallbackUri.fsPath);
    const abs = path.isAbsolute(filePart) ? filePart : path.resolve(baseDir, filePart);
    const uri = vscode.Uri.file(abs);
    const pos = new vscode.Position(line - 1, col - 1);
    return new vscode.Location(uri, pos);
}

// ---------------------------------------------------------------------------
// Test controller
// ---------------------------------------------------------------------------

let controller: vscode.TestController | undefined;
let fileWatcher: vscode.FileSystemWatcher | undefined;
let extensionContextRef: vscode.ExtensionContext | undefined;
const testItemMap = new Map<string, vscode.TestItem>();

export function registerTestController(context: vscode.ExtensionContext): void {
    extensionContextRef = context;
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

/**
 * Programmatic entry point used by the code-lens "▶ Run Tests in File"
 * action: ensures the file is registered, then triggers a TestRun
 * limited to it.
 */
export async function runTestsForUri(uri: vscode.Uri): Promise<void> {
    if (!controller || !extensionContextRef) {
        vscode.window.showWarningMessage('Eta test controller is not initialised yet.');
        return;
    }
    addTestFile(uri);
    const item = testItemMap.get(uri.toString());
    if (!item) return;
    const request = new vscode.TestRunRequest([item]);
    const tokenSrc = new vscode.CancellationTokenSource();
    try {
        await runTests(request, tokenSrc.token, extensionContextRef);
    } finally {
        tokenSrc.dispose();
    }
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

    const etaTest = discoverBinaries(context).test;
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

            proc.stdout.on('data', (d: Buffer) => {
                const chunk = d.toString();
                stdout += chunk;
                run.appendOutput(chunk.replace(/\r?\n/g, '\r\n'));
            });
            proc.stderr.on('data', (d: Buffer) => {
                const chunk = d.toString();
                stderr += chunk;
                run.appendOutput(chunk.replace(/\r?\n/g, '\r\n'));
            });

            const cancelSub = token.onCancellationRequested(() => proc.kill());

            proc.on('close', (code) => {
                cancelSub.dispose();

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
                            const headline = tr.message ?? tr.description ?? 'test failed';
                            const detailParts: string[] = [];
                            if (tr.severity) detailParts.push(`severity: ${tr.severity}`);
                            if (tr.expected !== undefined) detailParts.push(`expected: ${tr.expected}`);
                            if (tr.actual !== undefined) detailParts.push(`actual: ${tr.actual}`);
                            const detail = detailParts.length > 0
                                ? `${headline}\n${detailParts.join('\n')}`
                                : headline;
                            const msg = new vscode.TestMessage(detail);
                            if (tr.expected !== undefined) {
                                msg.expectedOutput = tr.expected;
                            }
                            if (tr.actual !== undefined) {
                                msg.actualOutput = tr.actual;
                            }
                            if (tr.at && item.uri) {
                                const loc = parseTapAtLocation(tr.at, item.uri);
                                if (loc) msg.location = loc;
                            }
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

