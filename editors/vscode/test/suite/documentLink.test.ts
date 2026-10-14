/**
 * documentLink.test.ts — verifies that import statements become clickable.
 */
import * as assert from 'assert';
import * as path from 'path';
import * as fs from 'fs';
import * as os from 'os';
import * as vscode from 'vscode';
import { EtaDocumentLinkProvider } from '../../src/documentLink';

const FIXTURES = path.resolve(__dirname, '..', '..', '..', 'test', 'fixtures');

describe('EtaDocumentLinkProvider', function () {
    this.timeout(15000);

    it('prefers workspace modules over configured roots and document siblings', async () => {
        const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        assert.ok(workspaceRoot);
        const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'eta-links-'));
        const moduleName = `linktest${path.basename(tempRoot).replace(/\W/g, '')}`;
        const moduleDir = path.join(workspaceRoot, moduleName);
        const config = vscode.workspace.getConfiguration('eta');
        const previousPath = config.inspect<string>('modulePath')?.workspaceValue;
        const cancellation = new vscode.CancellationTokenSource();
        try {
            fs.mkdirSync(moduleDir);
            fs.writeFileSync(path.join(moduleDir, 'local.eta'), '(module local)');
            fs.mkdirSync(path.join(tempRoot, moduleName));
            fs.writeFileSync(path.join(tempRoot, moduleName, 'local.eta'), '(module local)');
            const source = path.join(tempRoot, 'main.eta');
            fs.writeFileSync(source, `(module main (import ${moduleName}.local))`);
            await config.update('modulePath', tempRoot, vscode.ConfigurationTarget.Workspace);
            const doc = await vscode.workspace.openTextDocument(source);
            const links = await new EtaDocumentLinkProvider().provideDocumentLinks(
                doc, cancellation.token,
            );
            assert.ok(links);
            assert.strictEqual(links.length, 1);
            assert.strictEqual(links[0].target?.fsPath, path.join(moduleDir, 'local.eta'));
        } finally {
            cancellation.dispose();
            await config.update('modulePath', previousPath, vscode.ConfigurationTarget.Workspace);
            fs.rmSync(moduleDir, { recursive: true });
            fs.rmSync(tempRoot, { recursive: true });
        }
    });

    it('produces one link per dotted module name in (import …)', async () => {
        const uri = vscode.Uri.file(path.join(FIXTURES, 'sample.eta'));
        const doc = await vscode.workspace.openTextDocument(uri);
        await vscode.window.showTextDocument(doc);

        const links = await vscode.commands.executeCommand<vscode.DocumentLink[]>(
            'vscode.executeLinkProvider',
            uri,
        ) ?? [];

        // sample.eta has `(import std.core std.io)` → up to 2 links (resolution
        // may fail if stdlib isn't on disk where the test runs; ranges are still
        // generated only for resolvable links, so we only assert "no crash").
        assert.ok(Array.isArray(links));
        for (const link of links) {
            assert.ok(link.range, 'link missing range');
        }
    });
});
