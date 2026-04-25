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
 * snippets.test.ts — verifies the snippets file is valid JSON and contains
 * the prefixes promised by Track B7 of docs/dap_vs_plan.md.
 */
const assert = __importStar(require("assert"));
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
const SNIPPETS = path.resolve(__dirname, '..', '..', '..', 'snippets', 'eta.json');
function loadSnippets() {
    const raw = fs.readFileSync(SNIPPETS, 'utf8');
    return JSON.parse(raw);
}
describe('Eta snippets (B7)', () => {
    it('is valid JSON', () => {
        assert.doesNotThrow(loadSnippets);
    });
    it('exposes every prefix promised by Track B7', () => {
        const snippets = loadSnippets();
        const prefixes = new Set();
        for (const entry of Object.values(snippets)) {
            const p = entry.prefix;
            if (Array.isArray(p)) {
                p.forEach((x) => prefixes.add(x));
            }
            else {
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
            assert.ok(prefixes.has(prefix), `snippets/eta.json is missing prefix \`${prefix}\``);
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
            assert.ok(typeof entry.description === 'string'
                && entry.description.length > 0, `snippet ${name} is missing a description`);
        }
    });
});
//# sourceMappingURL=snippets.test.js.map