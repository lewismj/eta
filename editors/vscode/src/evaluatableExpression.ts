/**
 * evaluatableExpression.ts — Eta hover-evaluate provider.
 *
 * Given a position, grow outward to the smallest enclosing s-expression
 * (or fall back to the identifier under the cursor) and hand it to the
 * debug session. The DAP server sandboxes evaluation, so side-effecting
 * forms simply fail without affecting the debugged program.
 */

import * as vscode from 'vscode';

/** Characters that terminate an Eta identifier (mirrors language-configuration wordPattern). */
const IDENT_STOP = new Set<string>([
    ' ', '\t', '\r', '\n',
    '(', ')', '[', ']', '{', '}',
    '"', "'", '`', ';', '#', '|', '\\',
]);

function isIdentChar(ch: string): boolean {
    return !IDENT_STOP.has(ch);
}

/** Find the identifier range that contains `pos`, if any. */
function identifierRange(line: string, lineNo: number, character: number): vscode.Range | undefined {
    if (character > line.length) return undefined;

    // Scan left.
    let start = character;
    while (start > 0 && isIdentChar(line.charAt(start - 1))) start--;

    // Scan right.
    let end = character;
    while (end < line.length && isIdentChar(line.charAt(end))) end++;

    if (end === start) return undefined;
    return new vscode.Range(lineNo, start, lineNo, end);
}

/**
 * Walk outward from `pos` to the enclosing s-expression. Returns the range of
 * the enclosing list including its parentheses, or undefined if not inside one.
 *
 * Skips over string literals and line/block comments.
 */
export function enclosingSexpr(doc: vscode.TextDocument, pos: vscode.Position): vscode.Range | undefined {
    const fullText = doc.getText();
    const offset = doc.offsetAt(pos);

    // Walk left, tracking nesting depth, skipping strings/comments.
    // Strategy: scan from start of doc to `offset`, maintain a stack of '(' / '[' offsets,
    // also track whether we're inside a string or block-comment.
    const opens: number[] = [];
    let i = 0;
    let inString = false;
    let inBlockComment = false;
    let inLineComment = false;

    while (i < offset) {
        const ch = fullText.charAt(i);
        const nx = i + 1 < fullText.length ? fullText.charAt(i + 1) : '';

        if (inLineComment) {
            if (ch === '\n') inLineComment = false;
            i++;
            continue;
        }
        if (inBlockComment) {
            if (ch === '|' && nx === '#') { inBlockComment = false; i += 2; continue; }
            i++;
            continue;
        }
        if (inString) {
            if (ch === '\\') { i += 2; continue; }
            if (ch === '"') { inString = false; }
            i++;
            continue;
        }

        if (ch === ';') { inLineComment = true; i++; continue; }
        if (ch === '#' && nx === '|') { inBlockComment = true; i += 2; continue; }
        if (ch === '"') { inString = true; i++; continue; }
        if (ch === '(' || ch === '[') { opens.push(i); i++; continue; }
        if (ch === ')' || ch === ']') { opens.pop(); i++; continue; }
        i++;
    }

    if (opens.length === 0) return undefined;
    const startOffset = opens[opens.length - 1];

    // Walk right from `offset` to find matching close.
    let depth = 1;
    i = offset;
    inString = false;
    inBlockComment = false;
    inLineComment = false;
    while (i < fullText.length) {
        const ch = fullText.charAt(i);
        const nx = i + 1 < fullText.length ? fullText.charAt(i + 1) : '';

        if (inLineComment) {
            if (ch === '\n') inLineComment = false;
            i++;
            continue;
        }
        if (inBlockComment) {
            if (ch === '|' && nx === '#') { inBlockComment = false; i += 2; continue; }
            i++;
            continue;
        }
        if (inString) {
            if (ch === '\\') { i += 2; continue; }
            if (ch === '"') { inString = false; }
            i++;
            continue;
        }

        if (ch === ';') { inLineComment = true; i++; continue; }
        if (ch === '#' && nx === '|') { inBlockComment = true; i += 2; continue; }
        if (ch === '"') { inString = true; i++; continue; }
        if (ch === '(' || ch === '[') { depth++; i++; continue; }
        if (ch === ')' || ch === ']') {
            depth--;
            if (depth === 0) {
                return new vscode.Range(
                    doc.positionAt(startOffset),
                    doc.positionAt(i + 1),
                );
            }
            i++;
            continue;
        }
        i++;
    }

    return undefined;
}

export class EtaEvaluatableExpressionProvider implements vscode.EvaluatableExpressionProvider {
    provideEvaluatableExpression(
        document: vscode.TextDocument,
        position: vscode.Position,
        _token: vscode.CancellationToken,
    ): vscode.ProviderResult<vscode.EvaluatableExpression> {
        const line = document.lineAt(position.line).text;
        const identRange = identifierRange(line, position.line, position.character);

        // Prefer the identifier; fall back to enclosing s-expression.
        if (identRange) {
            const text = document.getText(identRange);
            // Skip pure punctuation / numbers — they hover-evaluate to themselves
            // but the DAP server will just echo them.
            if (text.length > 0) {
                return new vscode.EvaluatableExpression(identRange, text);
            }
        }

        const sexpr = enclosingSexpr(document, position);
        if (sexpr) {
            const text = document.getText(sexpr);
            // Skip extremely long forms — VS Code shows the value in a hover
            // and the request is sent on every hover.
            if (text.length <= 256) {
                return new vscode.EvaluatableExpression(sexpr, text);
            }
        }
        return undefined;
    }
}

