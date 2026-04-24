/**
 * codeLens.ts — Run / Test code lenses for Eta files.
 *
 *   ▶ Run File           — above the first `(module ...)` or `(begin ...)` form
 *                          in any *.eta file (skipped for *.test.eta).
 *   ▶ Run Tests in File  — above the first `(module ...)` form in *.test.eta.
 */

import * as vscode from 'vscode';

const MODULE_RE = /^\s*\(module\b/;
const BEGIN_RE  = /^\s*\(begin\b/;

export class EtaCodeLensProvider implements vscode.CodeLensProvider {
    private readonly emitter = new vscode.EventEmitter<void>();
    readonly onDidChangeCodeLenses = this.emitter.event;

    refresh(): void { this.emitter.fire(); }

    provideCodeLenses(
        document: vscode.TextDocument,
        _token: vscode.CancellationToken,
    ): vscode.ProviderResult<vscode.CodeLens[]> {
        const isTestFile = document.fileName.endsWith('.test.eta');
        const lenses: vscode.CodeLens[] = [];

        // Find first anchor line (module or begin).
        let anchor: vscode.Range | undefined;
        for (let i = 0; i < document.lineCount; i++) {
            const text = document.lineAt(i).text;
            if (MODULE_RE.test(text) || BEGIN_RE.test(text)) {
                anchor = new vscode.Range(i, 0, i, 0);
                break;
            }
        }
        if (!anchor) return lenses;

        if (isTestFile) {
            lenses.push(new vscode.CodeLens(anchor, {
                title: '$(beaker) Run Tests in File',
                command: 'eta.runTestFile',
                arguments: [document.uri],
                tooltip: 'Run all tests in this file via the Eta Test Explorer',
            }));
            lenses.push(new vscode.CodeLens(anchor, {
                title: '$(debug-alt) Debug Tests in File',
                command: 'eta.debugFile',
                arguments: [document.uri],
                tooltip: 'Launch this file under the Eta debugger',
            }));
        } else {
            lenses.push(new vscode.CodeLens(anchor, {
                title: '$(play) Run File',
                command: 'eta.runFile',
                arguments: [document.uri],
                tooltip: 'Run this Eta file under the debugger',
            }));
            lenses.push(new vscode.CodeLens(anchor, {
                title: '$(debug-alt) Debug File (stop on entry)',
                command: 'eta.debugFile',
                arguments: [document.uri, /*stopOnEntry*/ true],
                tooltip: 'Launch this Eta file paused at entry',
            }));
        }

        return lenses;
    }
}

