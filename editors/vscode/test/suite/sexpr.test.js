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
 * sexpr.test.ts — unit tests for the s-expression range walker.
 */
const assert = __importStar(require("assert"));
const vscode = __importStar(require("vscode"));
const evaluatableExpression_1 = require("../../src/evaluatableExpression");
function docOf(text) {
    return Promise.resolve(vscode.workspace.openTextDocument({ language: 'eta', content: text }))
        .then(d => d);
}
describe('enclosingSexpr', () => {
    it('returns undefined when not inside any list', async () => {
        const doc = await docOf('hello world\n');
        const r = (0, evaluatableExpression_1.enclosingSexpr)(doc, new vscode.Position(0, 3));
        assert.strictEqual(r, undefined);
    });
    it('returns the innermost list containing the cursor', async () => {
        const text = '(let ((x 1) (y 2)) (+ x y))';
        //            0123456789012345678901234567
        const doc = await docOf(text);
        // Cursor sits inside `(+ x y)` at offset 22 (the `x`).
        const pos = doc.positionAt(22);
        const r = (0, evaluatableExpression_1.enclosingSexpr)(doc, pos);
        assert.ok(r);
        assert.strictEqual(doc.getText(r), '(+ x y)');
    });
    it('skips parens inside string literals', async () => {
        const text = '(display "(((") (+ 1 2)';
        // The first `(display ...)` is closed by the `)` at position 14.
        // Cursor at the `+` is inside the second list.
        const doc = await docOf(text);
        const plusOff = text.indexOf('+');
        const r = (0, evaluatableExpression_1.enclosingSexpr)(doc, doc.positionAt(plusOff));
        assert.ok(r);
        assert.strictEqual(doc.getText(r), '(+ 1 2)');
    });
    it('skips parens inside line comments', async () => {
        const text = '; (((\n(+ 1 2)';
        const doc = await docOf(text);
        const plusOff = text.indexOf('+');
        const r = (0, evaluatableExpression_1.enclosingSexpr)(doc, doc.positionAt(plusOff));
        assert.ok(r);
        assert.strictEqual(doc.getText(r), '(+ 1 2)');
    });
    it('handles nested lists by returning the closest enclosing one', async () => {
        const text = '(a (b (c d) e) f)';
        const doc = await docOf(text);
        const dOff = text.indexOf('d');
        const r = (0, evaluatableExpression_1.enclosingSexpr)(doc, doc.positionAt(dOff));
        assert.ok(r);
        assert.strictEqual(doc.getText(r), '(c d)');
    });
    it('treats square brackets as list delimiters', async () => {
        const text = '[a b c]';
        const doc = await docOf(text);
        const r = (0, evaluatableExpression_1.enclosingSexpr)(doc, doc.positionAt(3));
        assert.ok(r);
        assert.strictEqual(doc.getText(r), '[a b c]');
    });
});
//# sourceMappingURL=sexpr.test.js.map