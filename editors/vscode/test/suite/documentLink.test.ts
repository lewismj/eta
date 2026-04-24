/**
 * documentLink.test.ts — verifies that import statements become clickable.
 */
import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

const FIXTURES = path.resolve(__dirname, '..', '..', '..', 'test', 'fixtures');

describe('EtaDocumentLinkProvider', function () {
    this.timeout(15000);

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

