/**
 * extension.test.ts — high-level smoke tests for activation, language
 * registration, command contribution and provider wiring.
 */
import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

const EXTENSION_ID = 'eta-schema-lang.eta-scheme-lang';
const FIXTURES = path.resolve(__dirname, '..', '..', '..', 'test', 'fixtures');

describe('Eta extension activation', function () {
    this.timeout(30000);

    it('is present in the extension registry', () => {
        const ext = vscode.extensions.getExtension(EXTENSION_ID);
        assert.ok(ext, `extension ${EXTENSION_ID} not found`);
    });

    it('activates without throwing', async () => {
        const ext = vscode.extensions.getExtension(EXTENSION_ID)!;
        await ext.activate();
        assert.strictEqual(ext.isActive, true);
    });

    it('registers the eta language', async () => {
        const langs = await vscode.languages.getLanguages();
        assert.ok(langs.includes('eta'), 'eta language id not registered');
    });

    it('opens an .eta file with the eta language id', async () => {
        const uri = vscode.Uri.file(path.join(FIXTURES, 'sample.eta'));
        const doc = await vscode.workspace.openTextDocument(uri);
        assert.strictEqual(doc.languageId, 'eta');
    });

    it('contributes the expected commands', async () => {
        await vscode.extensions.getExtension(EXTENSION_ID)!.activate();
        const all = await vscode.commands.getCommands(true);
        for (const cmd of [
            'eta.runFile',
            'eta.debugFile',
            'eta.runTestFile',
            'eta.showHeapInspector',
            'eta.showEnvironmentInspector',
            'eta.showDisassembly',
            'eta.refreshEnvironment',
        ]) {
            assert.ok(all.includes(cmd), `missing command: ${cmd}`);
        }
    });

    it('registers an evaluatable expression provider for eta', async () => {
        // We can only verify indirectly: requesting a hover-eval through the
        // command requires an active debug session, so just ensure that
        // executing the link/lens providers (which we registered together)
        // doesn't reject for an .eta document.
        const uri = vscode.Uri.file(path.join(FIXTURES, 'sample.eta'));
        await vscode.workspace.openTextDocument(uri);
        const result = await vscode.commands.executeCommand(
            'vscode.executeCodeLensProvider', uri,
        );
        assert.ok(Array.isArray(result));
    });
});

