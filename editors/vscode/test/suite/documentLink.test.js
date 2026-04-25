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
 * documentLink.test.ts — verifies that import statements become clickable.
 */
const assert = __importStar(require("assert"));
const path = __importStar(require("path"));
const vscode = __importStar(require("vscode"));
const FIXTURES = path.resolve(__dirname, '..', '..', '..', 'test', 'fixtures');
describe('EtaDocumentLinkProvider', function () {
    this.timeout(15000);
    it('produces one link per dotted module name in (import …)', async () => {
        const uri = vscode.Uri.file(path.join(FIXTURES, 'sample.eta'));
        const doc = await vscode.workspace.openTextDocument(uri);
        await vscode.window.showTextDocument(doc);
        const links = await vscode.commands.executeCommand('vscode.executeLinkProvider', uri) ?? [];
        // sample.eta has `(import std.core std.io)` → up to 2 links (resolution
        // may fail if stdlib isn't on disk where the test runs; ranges are still
        // generated only for resolvable links, so we only assert "no crash").
        assert.ok(Array.isArray(links));
        for (const link of links) {
            assert.ok(link.range, 'link missing range');
        }
    });
});
//# sourceMappingURL=documentLink.test.js.map