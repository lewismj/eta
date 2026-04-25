/**
 * snippets.test.ts — verifies the snippets file is valid JSON and contains
 * the prefixes promised by Track B7 of docs/dap_vs_plan.md.
 */
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';

const SNIPPETS = path.resolve(
    __dirname, '..', '..', '..', 'snippets', 'eta.json',
);

interface Snippet {
    prefix: string | string[];
    body: string | string[];
    description?: string;
}

function loadSnippets(): Record<string, Snippet> {
    const raw = fs.readFileSync(SNIPPETS, 'utf8');
    return JSON.parse(raw);
}

describe('Eta snippets (B7)', () => {
    it('is valid JSON', () => {
        assert.doesNotThrow(loadSnippets);
    });

    it('exposes every prefix promised by Track B7', () => {
        const snippets = loadSnippets();
        const prefixes = new Set<string>();
        for (const entry of Object.values(snippets)) {
            const p = entry.prefix;
            if (Array.isArray(p)) {
                p.forEach((x) => prefixes.add(x));
            } else {
                prefixes.add(p);
            }
        }
        const required = [
            // existing core forms
            'module', 'defun', 'define', 'lambda', 'let', 'cond',
            'define-record-type', 'define-syntax', 'import',
            // logic / relational
            'defrel', 'tabled', 'findall', 'run*', 'run1', 'run-n',
            'conde', 'fresh', 'freeze', 'dif',
            // CLP
            'clp-domain', 'clp-solve', 'clp-all-different',
            'clpr-maximize', 'clpb-solve',
            // supervisors / actors
            'one-for-one', 'one-for-all',
            'spawn-thread', 'spawn-thread-with', 'current-mailbox',
            // AAD / torch
            'grad', 'tape', 'tensor', 'backward',
            // testing
            'test-group',
        ];
        for (const prefix of required) {
            assert.ok(
                prefixes.has(prefix),
                `snippets/eta.json is missing prefix \`${prefix}\``,
            );
        }
    });

    it('does not advertise non-existent macro forms (e.g. defmacro)', () => {
        // Eta's macro system is `(define-syntax NAME (syntax-rules () ...))`
        // — there is no `defmacro`. Make sure no snippet references it.
        const raw = fs.readFileSync(SNIPPETS, 'utf8');
        assert.ok(!/defmacro/.test(raw), 'snippets must not mention defmacro');
    });

    it('every snippet has a description', () => {
        const snippets = loadSnippets();
        for (const [name, entry] of Object.entries(snippets)) {
            assert.ok(
                typeof entry.description === 'string'
                && entry.description.length > 0,
                `snippet ${name} is missing a description`,
            );
        }
    });
});

