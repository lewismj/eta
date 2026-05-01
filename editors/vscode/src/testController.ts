/**
 * testController.ts — Eta VS Code Test Explorer integration (B5 v2).
 *
 * - Spawns `eta_test --format tap` per *.test.eta file (process-level
 *   isolation between files).
 * - Parses TAP 13 *streamingly* via `TapStreamParser`, surfacing each
 *   test result the moment its YAML diagnostic block closes — child
 *   items appear/turn green/red live in the Test Explorer.
 * - Honours per-test cancellation: when the run token fires, the active
 *   process is killed and any not-yet-reported tests in that file are
 *   marked `skipped` instead of failing the whole file.
 * - Three run profiles:
 *     ▸ **Run**       — TAP-driven default profile.
 *     ▸ **Debug**     — `vscode.debug.startDebugging` per file with
 *                        debugType `eta`.
 *     ▸ **Coverage**  — passes `--coverage` to `eta_test`; degrades
 *                        gracefully when the runner doesn't recognise
 *                        the flag (TODO: parse LCOV when supported).
 */

import * as vscode from 'vscode';
import * as path from 'path';
import * as cp from 'child_process';
import { discoverBinaries } from './binaries';

// ---------------------------------------------------------------------------
// TAP parser — batch entry point preserved for unit tests.
// ---------------------------------------------------------------------------

export interface TapTestResult {
    ok: boolean;
    num: number;
    description: string;
    message?: string;
    severity?: string;
    at?: string;
    expected?: string;
    actual?: string;
}

export function parseTap(output: string): TapTestResult[] {
    const stream = new TapStreamParser();
    stream.feed(output);
    stream.flush();
    return stream.all();
}

// ---------------------------------------------------------------------------
// Streaming TAP 13 parser — emits results the moment they complete.
// ---------------------------------------------------------------------------

export class TapStreamParser {
    private buffer = '';
    private current: TapTestResult | null = null;
    private inYaml = false;
    private completed: TapTestResult[] = [];

    /**
     * Feed an arbitrary chunk of stdout. Any complete lines are processed;
     * the trailing partial line is buffered until the next call to feed().
     * Returns the test results that were finalised by this chunk.
     */
    feed(chunk: string): TapTestResult[] {
        const released: TapTestResult[] = [];
        this.buffer += chunk;
        let nl: number;
        while ((nl = this.buffer.indexOf('\n')) !== -1) {
            const rawLine = this.buffer.slice(0, nl).replace(/\r$/, '');
            this.buffer = this.buffer.slice(nl + 1);
            this.handleLine(rawLine, released);
        }
        return released;
    }

    /**
     * After EOF, flush any buffered partial line and the in-flight test.
     * Returns every result observed during the stream that has not yet been
     * delivered via `feed()`.
     */
    flush(): TapTestResult[] {
        const released: TapTestResult[] = [];
        if (this.buffer.length > 0) {
            this.handleLine(this.buffer.replace(/\r$/, ''), released);
            this.buffer = '';
        }
        if (this.current) {
            released.push(this.current);
            this.completed.push(this.current);
            this.current = null;
            this.inYaml = false;
        }
        return released;
    }

    private handleLine(line: string, released: TapTestResult[]): void {
        if (line.startsWith('TAP version') || /^\d+\.\.\d+$/.test(line)) return;

        const okMatch = line.match(/^ok\s+(\d+)\s*-?\s*(.*)/);
        if (okMatch) {
            this.releaseCurrent(released);
            this.current = {
                ok: true,
                num: parseInt(okMatch[1], 10),
                description: okMatch[2].trim(),
            };
            this.inYaml = false;
            return;
        }

        const notOkMatch = line.match(/^not ok\s+(\d+)\s*-?\s*(.*)/);
        if (notOkMatch) {
            this.releaseCurrent(released);
            this.current = {
                ok: false,
                num: parseInt(notOkMatch[1], 10),
                description: notOkMatch[2].trim(),
            };
            this.inYaml = false;
            return;
        }

        if (!this.current) return;

        if (line.trim() === '---') { this.inYaml = true;  return; }
        if (line.trim() === '...') { this.inYaml = false; return; }

        if (!this.inYaml) return;

        const yamlField = line.match(/^\s{2}([a-zA-Z0-9_-]+):\s*(.*)$/);
        if (!yamlField) return;
        const [, key, rawValue] = yamlField;
        const value = rawValue.trim();
        switch (key) {
            case 'message':  this.current.message = value;  break;
            case 'severity': this.current.severity = value; break;
            case 'at':       this.current.at = value;       break;
            case 'expected': this.current.expected = value; break;
            case 'actual':   this.current.actual = value;   break;
            default: break;
        }
    }

    private releaseCurrent(released: TapTestResult[]): void {
        if (!this.current) return;
        released.push(this.current);
        this.completed.push(this.current);
        this.current = null;
    }

    /** All results observed since construction (used for end-of-run summary). */
    all(): TapTestResult[] {
        return this.completed.slice();
    }
}

// ---------------------------------------------------------------------------
// `at:` location parsing.
// ---------------------------------------------------------------------------

export function parseTapAtLocation(at: string, fallbackUri: vscode.Uri): vscode.Location | undefined {
    const m = at.match(/^(.*?):(\d+)(?::(\d+))?$/);
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
// Test controller setup.
// ---------------------------------------------------------------------------

let controller: vscode.TestController | undefined;
let fileWatcher: vscode.FileSystemWatcher | undefined;
let extensionContextRef: vscode.ExtensionContext | undefined;
const testItemMap = new Map<string, vscode.TestItem>();

export function registerTestController(context: vscode.ExtensionContext): void {
    extensionContextRef = context;
    controller = vscode.tests.createTestController('eta-test-controller', 'Eta Tests');
    context.subscriptions.push(controller);

    controller.resolveHandler = async (item) => {
        if (!item) await discoverAllTests();
    };

    // Run profile (default).
    controller.createRunProfile(
        'Run', vscode.TestRunProfileKind.Run,
        (request, token) => runTests(request, token, context, { mode: 'run' }),
        /*isDefault=*/true,
    );
    // Debug profile — launches `eta_dap` for the file, ignores TAP.
    controller.createRunProfile(
        'Debug', vscode.TestRunProfileKind.Debug,
        (request, token) => runTests(request, token, context, { mode: 'debug' }),
        /*isDefault=*/false,
    );
    // Coverage profile — passes `--coverage` (graceful fallback today).
    controller.createRunProfile(
        'Coverage', vscode.TestRunProfileKind.Coverage,
        (request, token) => runTests(request, token, context, { mode: 'coverage' }),
        /*isDefault=*/false,
    );

    fileWatcher = vscode.workspace.createFileSystemWatcher('**/*.test.eta');
    fileWatcher.onDidCreate((uri) => addTestFile(uri));
    fileWatcher.onDidChange((uri) => addTestFile(uri));
    fileWatcher.onDidDelete((uri) => removeTestFile(uri));
    context.subscriptions.push(fileWatcher);

    discoverAllTests();
}

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
        await runTests(request, tokenSrc.token, extensionContextRef, { mode: 'run' });
    } finally {
        tokenSrc.dispose();
    }
}

async function discoverAllTests(): Promise<void> {
    if (!controller) return;
    const uris = await vscode.workspace.findFiles('**/*.test.eta', '**/node_modules/**');
    for (const uri of uris) addTestFile(uri);
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
// Run handler — shared by Run / Debug / Coverage.
// ---------------------------------------------------------------------------

interface RunOptions {
    mode: 'run' | 'debug' | 'coverage';
}

async function runTests(
    request: vscode.TestRunRequest,
    token: vscode.CancellationToken,
    context: vscode.ExtensionContext,
    opts: RunOptions,
): Promise<void> {
    if (!controller) return;
    const run = controller.createTestRun(request);

    const items: vscode.TestItem[] = [];
    if (request.include) request.include.forEach((i) => items.push(i));
    else                 controller.items.forEach((i) => items.push(i));

    try {
        for (const item of items) {
            if (token.isCancellationRequested) {
                run.skipped(item);
                continue;
            }
            if (!item.uri) continue;
            run.started(item);

            if (opts.mode === 'debug') {
                await runOneFileUnderDebugger(item, run, token);
            } else {
                await runOneFile(item, run, token, context, opts);
            }
        }
    } finally {
        run.end();
    }
}

// ---------------------------------------------------------------------------
// Debug profile — launches the DAP adapter against the test file.
// ---------------------------------------------------------------------------

async function runOneFileUnderDebugger(
    item: vscode.TestItem,
    run: vscode.TestRun,
    token: vscode.CancellationToken,
): Promise<void> {
    if (!item.uri) {
        run.failed(item, new vscode.TestMessage('Test item has no associated file.'));
        return;
    }
    const config: vscode.DebugConfiguration = {
        type: 'eta',
        request: 'launch',
        name: `Debug ${path.basename(item.uri.fsPath)}`,
        program: item.uri.fsPath,
        stopOnEntry: false,
    };
    let session: vscode.DebugSession | undefined;
    const startedDisposable = vscode.debug.onDidStartDebugSession((s) => {
        if (s.configuration.program === item.uri!.fsPath) session = s;
    });
    const cancelSub = token.onCancellationRequested(() => {
        if (session) vscode.debug.stopDebugging(session);
    });
    try {
        const ok = await vscode.debug.startDebugging(undefined, config);
        if (!ok) {
            run.failed(item, new vscode.TestMessage('Failed to start eta debug session.'));
            return;
        }
        await new Promise<void>((resolve) => {
            const sub = vscode.debug.onDidTerminateDebugSession((s) => {
                if (s === session) { sub.dispose(); resolve(); }
            });
        });
        run.passed(item);
    } finally {
        startedDisposable.dispose();
        cancelSub.dispose();
    }
}

// ---------------------------------------------------------------------------
// Run / Coverage — TAP streaming.
// ---------------------------------------------------------------------------

async function runOneFile(
    item: vscode.TestItem,
    run: vscode.TestRun,
    token: vscode.CancellationToken,
    context: vscode.ExtensionContext,
    opts: RunOptions,
): Promise<void> {
    const etaTest = discoverBinaries(context).test;
    if (!etaTest) {
        run.failed(item, new vscode.TestMessage(
            'Could not locate the eta_test binary. Set eta.test.runnerPath in settings.',
        ));
        return;
    }
    const modulePath = vscode.workspace.getConfiguration('eta').get<string>('modulePath', '')
        || process.env['ETA_MODULE_PATH']
        || '';

    const args: string[] = [];
    if (modulePath) args.push('--path', modulePath);
    args.push('--format', 'tap');
    if (opts.mode === 'coverage') args.push('--coverage');
    args.push(item.uri!.fsPath);

    const env: NodeJS.ProcessEnv = { ...process.env };
    if (modulePath) env['ETA_MODULE_PATH'] = modulePath;

    const stream = new TapStreamParser();
    /** Test items reported as completed during this file. */
    const reportedNums = new Set<number>();
    /** Child items keyed by test number, for in-flight cancel/skip handling. */
    const childItems = new Map<number, vscode.TestItem>();
    item.children.replace([]);

    let stderr = '';
    let cancelled = false;

    await new Promise<void>((resolve) => {
        const proc = cp.spawn(etaTest, args, {
            env,
            cwd: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath,
        });

        const cancelSub = token.onCancellationRequested(() => {
            cancelled = true;
            proc.kill();
        });

        proc.stdout.on('data', (d: Buffer) => {
            const chunk = d.toString();
            run.appendOutput(chunk.replace(/\r?\n/g, '\r\n'));
            const newResults = stream.feed(chunk);
            for (const tr of newResults) {
                reportTapResult(tr, item, run, childItems);
                reportedNums.add(tr.num);
            }
        });
        proc.stderr.on('data', (d: Buffer) => {
            const chunk = d.toString();
            stderr += chunk;
            run.appendOutput(chunk.replace(/\r?\n/g, '\r\n'));
        });

        proc.on('error', (err) => {
            cancelSub.dispose();
            run.failed(item, new vscode.TestMessage(
                `Failed to start eta_test: ${err.message}\n` +
                `Resolved runner: ${etaTest}\n` +
                `If this is not an absolute path, VS Code could not resolve it from PATH.\n` +
                `Set eta.test.runnerPath to the eta_test executable or its directory.`,
            ));
            resolve();
        });

        proc.on('close', (code) => {
            cancelSub.dispose();
            const tail = stream.flush();
            for (const tr of tail) {
                if (reportedNums.has(tr.num)) continue;
                reportTapResult(tr, item, run, childItems);
                reportedNums.add(tr.num);
            }
            const all = stream.all();

            // Coverage profile gracefully degrades when the runner rejects the flag.
            if (opts.mode === 'coverage' && code !== 0
                && /invalid argument:\s*--coverage/i.test(stderr)) {
                run.skipped(item);
                run.appendOutput(
                    '\r\n[eta] Coverage profile requires `eta_test --coverage`, ' +
                    'which is not yet supported by your eta_test binary.\r\n',
                );
                resolve();
                return;
            }

            if (cancelled) {
                for (const child of childItems.values()) {
                    const num = parseInt(child.id.split('::').pop()!, 10);
                    if (!reportedNums.has(num)) run.skipped(child);
                }
                run.skipped(item);
                resolve();
                return;
            }

            if (all.length === 0) {
                if (code !== 0) {
                    run.failed(item, new vscode.TestMessage(
                        stderr || 'eta_test failed with no output',
                    ));
                } else {
                    run.passed(item);
                }
            } else {
                const failCount = all.filter((r) => !r.ok).length;
                if (failCount === 0) run.passed(item);
                else run.failed(item, new vscode.TestMessage(
                    `${failCount} of ${all.length} tests failed`,
                ));
            }
            resolve();
        });
    });
}

function reportTapResult(
    tr: TapTestResult,
    parent: vscode.TestItem,
    run: vscode.TestRun,
    childItems: Map<number, vscode.TestItem>,
): void {
    if (!controller) return;
    const childId = `${parent.id}::${tr.num}`;
    let child = childItems.get(tr.num);
    if (!child) {
        child = controller.createTestItem(childId, tr.description || `test ${tr.num}`, parent.uri);
        childItems.set(tr.num, child);
        parent.children.add(child);
    } else {
        child.label = tr.description || child.label;
    }
    run.started(child);
    if (tr.ok) {
        run.passed(child);
        return;
    }
    const headline = tr.message ?? tr.description ?? 'test failed';
    const detailParts: string[] = [];
    if (tr.severity) detailParts.push(`severity: ${tr.severity}`);
    if (tr.expected !== undefined) detailParts.push(`expected: ${tr.expected}`);
    if (tr.actual   !== undefined) detailParts.push(`actual: ${tr.actual}`);
    const detail = detailParts.length > 0
        ? `${headline}\n${detailParts.join('\n')}`
        : headline;
    const msg = new vscode.TestMessage(detail);
    if (tr.expected !== undefined) msg.expectedOutput = tr.expected;
    if (tr.actual   !== undefined) msg.actualOutput   = tr.actual;
    if (tr.at && parent.uri) {
        const loc = parseTapAtLocation(tr.at, parent.uri);
        if (loc) msg.location = loc;
    }
    run.failed(child, msg);
}

