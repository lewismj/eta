import * as assert from 'assert';
import * as vm from 'vm';
import * as vscode from 'vscode';
import { EtaInlineValuesProvider } from '../../src/inlineValues';

describe('EtaInlineValuesProvider', () => {
    async function values(
        content: string,
        stoppedLine?: number,
    ): Promise<vscode.InlineValueEvaluatableExpression[]> {
        const document = await vscode.workspace.openTextDocument({ language: 'eta', content });
        const lastLine = document.lineCount - 1;
        const cancellation = new vscode.CancellationTokenSource();
        try {
            // A synchronous tokenizer loop must fail the test, not freeze the test host.
            const result: unknown = vm.runInNewContext(
                'provider.provideInlineValues(document, viewport, context, token)',
                {
                    provider: new EtaInlineValuesProvider(),
                    document,
                    viewport: new vscode.Range(0, 0, lastLine, document.lineAt(lastLine).text.length),
                    context: {
                        frameId: 1,
                        stoppedLocation: new vscode.Range(stoppedLine ?? lastLine, 0, stoppedLine ?? lastLine, 0),
                    },
                    token: cancellation.token,
                },
                { timeout: 1000 },
            );
            assert.ok(Array.isArray(result));
            assert.ok(result.every(value => value instanceof vscode.InlineValueEvaluatableExpression));
            return result;
        } finally {
            cancellation.dispose();
        }
    }

    it('preserves identifiers, operators, ranges and per-line deduplication', async () => {
        const result = await values('(define total (+ x x 42))\n(println total)');
        assert.deepStrictEqual(result.map(value => value.expression), ['total', '+', 'x', 'println', 'total']);
        const x = result.find(value => value.expression === 'x');
        assert.ok(x);
        assert.strictEqual(x.range.start.line, 0);
        assert.strictEqual(x.range.start.character, 17);
        assert.strictEqual(x.range.end.character, 18);
    });

    it('returns for the basics.eta quote and boolean syntax without evaluating quoted atoms', async () => {
        const result = await values([
            "(define win (eq? (platform) 'Win32))",
            '(println (if win "-- Symbols & Quoting --" "Symbols & Quoting"))',
            "(println 'hello)",
            "(println '(one two three))",
            '(println (and #t #f))',
            '(println `symbol)',
        ].join('\n'));
        const expressions = result.map(value => value.expression);
        assert.ok(expressions.includes('win'));
        assert.ok(expressions.includes('println'));
        for (const literal of ['Win32', 'hello', 'symbol', 't', 'f', '#t', '#f']) {
            assert.ok(!expressions.includes(literal), `Unexpected literal evaluation: ${literal}`);
        }
    });

    it('advances past every identifier delimiter and incomplete reader prefix', async () => {
        for (const delimiter of ["'", '`', '#', '|', '\\', ',', '{', '}']) {
            await values(`(println ${delimiter})`);
            await values(delimiter);
        }
        const result = await values('(println #\\space #\\) #\\; x)');
        assert.deepStrictEqual(result.map(value => value.expression), ['println', 'x']);
    });

    it('continues to skip strings, comments, numeric literals and logic variables', async () => {
        const result = await values([
            '(println "quoted \\"text\\"" x) ; ignored',
            '#| hidden',
            'still-hidden |# (println y -1 2.5 ?logic)',
        ].join('\n'));
        assert.deepStrictEqual(result.map(value => value.expression), ['println', 'x', 'println', 'y']);
    });

    it('does not scan beyond the stopped line', async () => {
        const result = await values('(println before)\n(println after)', 0);
        assert.deepStrictEqual(result.map(value => value.expression), ['println', 'before']);
    });
});
