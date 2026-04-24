/**
 * codeLens.test.ts — verifies that the code-lens provider emits the expected
 * lenses for plain `.eta` files vs `*.test.eta` files.
 */
import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

const FIXTURES = path.resolve(__dirname, '..', '..', '..', 'test', 'fixtures');

async function lensesFor(file: string): Promise<vscode.CodeLens[]> {
    const uri = vscode.Uri.file(path.join(FIXTURES, file));
    const doc = await vscode.workspace.openTextDocument(uri);
    await vscode.window.showTextDocument(doc);
    // VS Code computes lenses asynchronously after the document is open.
    const lenses = await vscode.commands.executeCommand<vscode.CodeLens[]>(
        'vscode.executeCodeLensProvider',
        uri,
    );
    return lenses ?? [];
}

describe('EtaCodeLensProvider', function () {
    this.timeout(15000);

    it('emits Run/Debug lenses for a plain .eta file', async () => {
        const lenses = await lensesFor('sample.eta');
        const titles = lenses.map(l => l.command?.title ?? '');
        assert.ok(titles.some(t => t.includes('Run File')), `expected Run File lens; got: ${titles.join(' | ')}`);
        assert.ok(titles.some(t => t.includes('Debug File')), `expected Debug File lens; got: ${titles.join(' | ')}`);
    });

    it('emits Run Tests / Debug lenses for a *.test.eta file', async () => {
        const lenses = await lensesFor('sample.test.eta');
        const titles = lenses.map(l => l.command?.title ?? '');
        assert.ok(titles.some(t => t.includes('Run Tests')), `expected Run Tests lens; got: ${titles.join(' | ')}`);
        assert.ok(titles.some(t => t.includes('Debug')), `expected Debug lens; got: ${titles.join(' | ')}`);
    });

    it('binds lenses to existing commands', async () => {
        const lenses = await lensesFor('sample.eta');
        const allCommands = await vscode.commands.getCommands(true);
        for (const lens of lenses) {
            const cmd = lens.command?.command;
            assert.ok(cmd, 'lens has no command');
            assert.ok(allCommands.includes(cmd!), `command ${cmd} is not registered`);
        }
    });
});

