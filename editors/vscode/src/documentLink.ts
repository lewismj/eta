/**
 * documentLink.ts — Make `(import std.foo std.bar)` clickable.
 *
 * Resolves dotted module names to files using ETA_MODULE_PATH and the
 * workspace's stdlib/<...> if present. A module `a.b.c` maps to `a/b/c.eta`.
 */

import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

function moduleSearchPaths(): string[] {
    const cfg = vscode.workspace.getConfiguration('eta').get<string>('modulePath', '')
        || vscode.workspace.getConfiguration('eta.lsp').get<string>('modulePath', '')
        || process.env['ETA_MODULE_PATH']
        || '';
    const sep = process.platform === 'win32' ? /[;:]/ : /:/;
    const parts = cfg.split(sep).map(s => s.trim()).filter(Boolean);

    // Augment with workspace folder + workspace/stdlib as a friendly default.
    for (const ws of vscode.workspace.workspaceFolders ?? []) {
        parts.push(ws.uri.fsPath);
        parts.push(path.join(ws.uri.fsPath, 'stdlib'));
    }
    return parts;
}

function resolveModule(name: string, anchorDir: string): vscode.Uri | undefined {
    const rel = name.split('.').join(path.sep) + '.eta';
    const candidates: string[] = [];
    candidates.push(path.resolve(anchorDir, rel));
    for (const root of moduleSearchPaths()) {
        candidates.push(path.resolve(root, rel));
    }
    for (const c of candidates) {
        try {
            if (fs.statSync(c).isFile()) return vscode.Uri.file(c);
        } catch { /* keep looking */ }
    }
    return undefined;
}

const IMPORT_RE = /\(import\b([^)]*)\)/g;
const NAME_RE   = /[A-Za-z_][\w.\-?!*/+<>=]*/g;

export class EtaDocumentLinkProvider implements vscode.DocumentLinkProvider {
    provideDocumentLinks(
        document: vscode.TextDocument,
        _token: vscode.CancellationToken,
    ): vscode.ProviderResult<vscode.DocumentLink[]> {
        const links: vscode.DocumentLink[] = [];
        const text = document.getText();
        const anchorDir = path.dirname(document.uri.fsPath);

        IMPORT_RE.lastIndex = 0;
        let m: RegExpExecArray | null;
        while ((m = IMPORT_RE.exec(text)) !== null) {
            const inner = m[1];
            const innerStart = m.index + m[0].indexOf(inner);

            NAME_RE.lastIndex = 0;
            let nm: RegExpExecArray | null;
            while ((nm = NAME_RE.exec(inner)) !== null) {
                const name = nm[0];
                if (!name.includes('.')) continue; // only dotted module names

                const startOff = innerStart + nm.index;
                const endOff   = startOff + name.length;
                const range = new vscode.Range(
                    document.positionAt(startOff),
                    document.positionAt(endOff),
                );

                const target = resolveModule(name, anchorDir);
                if (target) {
                    const link = new vscode.DocumentLink(range, target);
                    link.tooltip = `Open module ${name}`;
                    links.push(link);
                }
            }
        }
        return links;
    }
}

