/**
 * sexpr.test.ts — unit tests for the s-expression range walker.
 */
import * as assert from 'assert';
import * as vscode from 'vscode';
import { enclosingSexpr } from '../../src/evaluatableExpression';

function docOf(text: string): Promise<vscode.TextDocument> {
    return Promise.resolve(vscode.workspace.openTextDocument({ language: 'eta', content: text }))
        .then(d => d);
}

describe('enclosingSexpr', () => {
    it('returns undefined when not inside any list', async () => {
        const doc = await docOf('hello world\n');
        const r = enclosingSexpr(doc, new vscode.Position(0, 3));
        assert.strictEqual(r, undefined);
    });

    it('returns the innermost list containing the cursor', async () => {
        const text = '(let ((x 1) (y 2)) (+ x y))';
        //            0123456789012345678901234567
        const doc = await docOf(text);
        // Cursor sits inside `(+ x y)` at offset 22 (the `x`).
        const pos = doc.positionAt(22);
        const r = enclosingSexpr(doc, pos);
        assert.ok(r);
        assert.strictEqual(doc.getText(r!), '(+ x y)');
    });

    it('skips parens inside string literals', async () => {
        const text = '(display "(((") (+ 1 2)';
        // The first `(display ...)` is closed by the `)` at position 14.
        // Cursor at the `+` is inside the second list.
        const doc = await docOf(text);
        const plusOff = text.indexOf('+');
        const r = enclosingSexpr(doc, doc.positionAt(plusOff));
        assert.ok(r);
        assert.strictEqual(doc.getText(r!), '(+ 1 2)');
    });

    it('skips parens inside line comments', async () => {
        const text = '; (((\n(+ 1 2)';
        const doc = await docOf(text);
        const plusOff = text.indexOf('+');
        const r = enclosingSexpr(doc, doc.positionAt(plusOff));
        assert.ok(r);
        assert.strictEqual(doc.getText(r!), '(+ 1 2)');
    });

    it('handles nested lists by returning the closest enclosing one', async () => {
        const text = '(a (b (c d) e) f)';
        const doc = await docOf(text);
        const dOff = text.indexOf('d');
        const r = enclosingSexpr(doc, doc.positionAt(dOff));
        assert.ok(r);
        assert.strictEqual(doc.getText(r!), '(c d)');
    });

    it('treats square brackets as list delimiters', async () => {
        const text = '[a b c]';
        const doc = await docOf(text);
        const r = enclosingSexpr(doc, doc.positionAt(3));
        assert.ok(r);
        assert.strictEqual(doc.getText(r!), '[a b c]');
    });
});

