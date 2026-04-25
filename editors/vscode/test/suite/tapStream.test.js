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
 * tapStream.test.ts — unit tests for the streaming TAP 13 parser and the
 * `at:` location helper used by the Eta Test Explorer (B5).
 */
const assert = __importStar(require("assert"));
const path = __importStar(require("path"));
const vscode = __importStar(require("vscode"));
const testController_1 = require("../../src/testController");
describe('TapStreamParser', () => {
    it('emits each `ok` line as soon as its terminating newline arrives', () => {
        const p = new testController_1.TapStreamParser();
        const a = p.feed('TAP version 13\n1..2\nok 1 - first\n');
        // Result for test 1 is in flight (no terminator yet other than the
        // newline). Since the next test would release it via releaseCurrent,
        // and we have not yet seen a follow-up line, it stays buffered.
        assert.strictEqual(a.length, 0);
        const b = p.feed('ok 2 - second\n');
        // Feeding the next `ok` releases the previously-buffered test.
        assert.strictEqual(b.length, 1);
        assert.strictEqual(b[0].num, 1);
        assert.strictEqual(b[0].ok, true);
        // The second test is still in-flight until flush() or the next test.
        const tail = p.flush();
        assert.strictEqual(tail.length, 1);
        assert.strictEqual(tail[0].num, 2);
        assert.strictEqual(tail[0].description, 'second');
    });
    it('handles chunks split mid-line', () => {
        const p = new testController_1.TapStreamParser();
        p.feed('TAP version');
        p.feed(' 13\nok 1 - ');
        p.feed('split\nok 2');
        p.feed(' - second\n');
        const all = p.flush();
        assert.strictEqual(all.length, 2);
        assert.strictEqual(all[0].description, 'split');
        assert.strictEqual(all[1].description, 'second');
    });
    it('parses YAML diagnostics that arrive across chunks', () => {
        const p = new testController_1.TapStreamParser();
        p.feed('not ok 1 - boom\n  ---\n  mess');
        p.feed('age: kaboom\n  severity: fail\n  at: a.test.eta:9:2\n');
        p.feed('  expected: 1\n  actual: 2\n  ...\n');
        const all = p.flush();
        assert.strictEqual(all.length, 1);
        assert.strictEqual(all[0].ok, false);
        assert.strictEqual(all[0].message, 'kaboom');
        assert.strictEqual(all[0].severity, 'fail');
        assert.strictEqual(all[0].at, 'a.test.eta:9:2');
        assert.strictEqual(all[0].expected, '1');
        assert.strictEqual(all[0].actual, '2');
    });
    it('all() returns the full history including the in-flight test after flush', () => {
        const p = new testController_1.TapStreamParser();
        p.feed('ok 1 - a\nok 2 - b\n');
        p.flush();
        const all = p.all();
        assert.strictEqual(all.length, 2);
        assert.deepStrictEqual(all.map((r) => r.num), [1, 2]);
    });
    it('ignores plan and version lines', () => {
        const p = new testController_1.TapStreamParser();
        p.feed('TAP version 13\n1..3\nok 1 - one\n');
        p.flush();
        assert.strictEqual(p.all().length, 1);
    });
});
describe('parseTapAtLocation', () => {
    const fallback = vscode.Uri.file(path.resolve('/tmp/dir/sample.test.eta'));
    it('parses file:line:col into a vscode.Location', () => {
        const loc = (0, testController_1.parseTapAtLocation)('foo.eta:12:5', fallback);
        assert.ok(loc);
        assert.strictEqual(loc.range.start.line, 11);
        assert.strictEqual(loc.range.start.character, 4);
    });
    it('parses file:line (no column) defaulting column to 1', () => {
        const loc = (0, testController_1.parseTapAtLocation)('foo.eta:12', fallback);
        assert.ok(loc);
        assert.strictEqual(loc.range.start.line, 11);
        assert.strictEqual(loc.range.start.character, 0);
    });
    it('resolves a relative path against the fallback URI directory', () => {
        const loc = (0, testController_1.parseTapAtLocation)('foo.eta:1:1', fallback);
        assert.ok(loc);
        assert.strictEqual(path.normalize(loc.uri.fsPath), path.normalize(path.resolve('/tmp/dir/foo.eta')));
    });
    it('returns undefined for unparsable strings', () => {
        assert.strictEqual((0, testController_1.parseTapAtLocation)('not a location', fallback), undefined);
    });
});
//# sourceMappingURL=tapStream.test.js.map