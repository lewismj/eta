/**
 * inlineValues.ts — Eta inline-values provider.
 *
 * For every identifier appearing in the visible viewport (up to and including
 * the stopped line), emit an InlineValueEvaluatableExpression so VS Code asks
 * the DAP server to evaluate it via the sandbox. Identifiers are de-duplicated
 * per call and obviously-non-variable tokens (numbers, keywords, the head of
 * a syntactic form) are skipped to keep request volume sane.
 */

import * as vscode from 'vscode';

/** Identifier-character predicate matching language-configuration wordPattern. */
const IDENT_STOP = new Set<string>([
    ' ', '\t', '\r', '\n',
    '(', ')', '[', ']', '{', '}',
    '"', "'", '`', ';', '#', '|', '\\', ',',
]);

/**
 * Scheme/Eta forms whose first sub-form is *binding syntax*, not a value
 * to evaluate. We skip the head token of these so we don't try to eval
 * `defun`, `lambda`, etc.
 */
const SYNTACTIC_HEADS = new Set<string>([
    'defun', 'define', 'define-record-type', 'define-syntax',
    'lambda', 'let', 'let*', 'letrec', 'letrec*', 'fn',
    'if', 'cond', 'case', 'when', 'unless', 'and', 'or', 'not',
    'begin', 'do', 'set!', 'quote', 'quasiquote', 'unquote',
    'module', 'import', 'export',
    'try', 'catch', 'throw', 'finally',
    'spawn', 'spawn-thread', 'send', 'receive',
    'match', 'with',
]);

/** Keywords that are valid identifiers but never evaluate to a useful value. */
const VALUE_BLACKLIST = new Set<string>([
    'true', 'false', 'nil', 'null', '#t', '#f',
    'else', '=>', '...', '_',
]);

function isIdentChar(ch: string): boolean { return !IDENT_STOP.has(ch); }

function isNumericLiteral(text: string): boolean {
    return /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/.test(text);
}

interface Token {
    text: string;
    range: vscode.Range;
    /** True if this token is the first sub-form of a `(...)` list. */
    isHead: boolean;
}

/**
 * Tokenize lines `[startLine, endLine]` (inclusive) producing identifier-like
 * tokens with a hint for whether each is the syntactic head of an enclosing
 * list. Strings and comments are skipped.
 */
function tokenize(doc: vscode.TextDocument, startLine: number, endLine: number): Token[] {
    const tokens: Token[] = [];
    let inString = false;
    let inBlock = false;
    /** Have we seen a non-whitespace token since the last `(`? */
    let sawAnythingInList = false;

    for (let ln = startLine; ln <= endLine; ln++) {
        const text = doc.lineAt(ln).text;
        let col = 0;
        let inLine = false;
        while (col < text.length) {
            const ch = text.charAt(col);
            const nx = col + 1 < text.length ? text.charAt(col + 1) : '';

            if (inLine) { col++; continue; }
            if (inBlock) {
                if (ch === '|' && nx === '#') { inBlock = false; col += 2; continue; }
                col++; continue;
            }
            if (inString) {
                if (ch === '\\') { col += 2; continue; }
                if (ch === '"') { inString = false; }
                col++; continue;
            }

            if (ch === ';') { inLine = true; col++; continue; }
            if (ch === '#' && nx === '|') { inBlock = true; col += 2; continue; }
            if (ch === '"') { inString = true; col++; continue; }
            if (ch === '(' || ch === '[') { sawAnythingInList = false; col++; continue; }
            if (ch === ')' || ch === ']') { sawAnythingInList = true; col++; continue; }
            if (ch === ' ' || ch === '\t') { col++; continue; }

            // Start of a token.
            const start = col;
            while (col < text.length && isIdentChar(text.charAt(col))) col++;
            const tokText = text.substring(start, col);
            if (tokText.length > 0) {
                tokens.push({
                    text: tokText,
                    range: new vscode.Range(ln, start, ln, col),
                    isHead: !sawAnythingInList,
                });
                sawAnythingInList = true;
            }
        }
        // Line comments terminate at EOL.
    }

    return tokens;
}

export class EtaInlineValuesProvider implements vscode.InlineValuesProvider {
    provideInlineValues(
        document: vscode.TextDocument,
        viewPort: vscode.Range,
        context: vscode.InlineValueContext,
        _token: vscode.CancellationToken,
    ): vscode.ProviderResult<vscode.InlineValue[]> {
        // Bound the scan: only show values for identifiers at or before the
        // currently-stopped line (later lines haven't executed yet).
        const lastLine = Math.min(viewPort.end.line, context.stoppedLocation.end.line);
        const firstLine = viewPort.start.line;
        if (lastLine < firstLine) return [];

        const tokens = tokenize(document, firstLine, lastLine);
        const out: vscode.InlineValue[] = [];
        const seenOnLine = new Map<number, Set<string>>();

        for (const tok of tokens) {
            if (tok.isHead && SYNTACTIC_HEADS.has(tok.text)) continue;
            if (VALUE_BLACKLIST.has(tok.text)) continue;
            if (isNumericLiteral(tok.text)) continue;
            // Skip logic vars / quoted symbols: ?x, 'foo, `foo
            if (tok.text.startsWith('?') || tok.text.startsWith("'") || tok.text.startsWith('`')) continue;
            // Skip very short noise (single punctuation-like ops are still useful, allow length>=1)
            if (tok.text.length === 0) continue;

            // De-dup per line — VS Code groups multiple same-name identifiers anyway,
            // and avoids triggering N evaluations of the same name on one line.
            const ln = tok.range.start.line;
            let lineSet = seenOnLine.get(ln);
            if (!lineSet) { lineSet = new Set(); seenOnLine.set(ln, lineSet); }
            if (lineSet.has(tok.text)) continue;
            lineSet.add(tok.text);

            out.push(new vscode.InlineValueEvaluatableExpression(tok.range, tok.text));
        }

        return out;
    }
}

