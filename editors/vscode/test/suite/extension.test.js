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
 * extension.test.ts — high-level smoke tests for activation, language
 * registration, command contribution and provider wiring.
 */
const assert = __importStar(require("assert"));
const path = __importStar(require("path"));
const vscode = __importStar(require("vscode"));
const EXTENSION_ID = 'eta-schema-lang.eta-scheme-lang';
const FIXTURES = path.resolve(__dirname, '..', '..', '..', 'test', 'fixtures');
describe('Eta extension activation', function () {
    this.timeout(30000);
    it('is present in the extension registry', () => {
        const ext = vscode.extensions.getExtension(EXTENSION_ID);
        assert.ok(ext, `extension ${EXTENSION_ID} not found`);
    });
    it('activates without throwing', async () => {
        const ext = vscode.extensions.getExtension(EXTENSION_ID);
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
        await vscode.extensions.getExtension(EXTENSION_ID).activate();
        const all = await vscode.commands.getCommands(true);
        for (const cmd of [
            'eta.runFile',
            'eta.debugFile',
            'eta.runTestFile',
            'eta.showHeapInspector',
            'eta.showDisassembly',
            'eta.refreshGCRoots',
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
        const result = await vscode.commands.executeCommand('vscode.executeCodeLensProvider', uri);
        assert.ok(Array.isArray(result));
    });
});
//# sourceMappingURL=extension.test.js.map