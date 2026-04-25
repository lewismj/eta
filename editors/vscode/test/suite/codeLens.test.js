"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
/**
 * codeLens.test.ts — verifies that the code-lens provider emits the expected
 * lenses for plain `.eta` files vs `*.test.eta` files.
 */
const assert = __importStar(require("assert"));
const path = __importStar(require("path"));
const vscode = __importStar(require("vscode"));
const FIXTURES = path.resolve(__dirname, '..', '..', '..', 'test', 'fixtures');
async function lensesFor(file) {
    const uri = vscode.Uri.file(path.join(FIXTURES, file));
    const doc = await vscode.workspace.openTextDocument(uri);
    await vscode.window.showTextDocument(doc);
    // VS Code computes lenses asynchronously after the document is open.
    const lenses = await vscode.commands.executeCommand('vscode.executeCodeLensProvider', uri);
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
            assert.ok(allCommands.includes(cmd), `command ${cmd} is not registered`);
        }
    });
});
//# sourceMappingURL=codeLens.test.js.map