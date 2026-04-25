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
 * parseTap.test.ts — unit tests for the TAP parser used by the test controller.
 */
const assert = __importStar(require("assert"));
const testController_1 = require("../../src/testController");
describe('parseTap', () => {
    it('parses an empty plan as no results', () => {
        const out = (0, testController_1.parseTap)('TAP version 13\n1..0\n');
        assert.strictEqual(out.length, 0);
    });
    it('parses a single passing test', () => {
        const tap = [
            'TAP version 13',
            '1..1',
            'ok 1 - addition works',
        ].join('\n');
        const r = (0, testController_1.parseTap)(tap);
        assert.strictEqual(r.length, 1);
        assert.strictEqual(r[0].ok, true);
        assert.strictEqual(r[0].num, 1);
        assert.strictEqual(r[0].description, 'addition works');
    });
    it('parses a failing test with YAML diagnostics', () => {
        const tap = [
            'TAP version 13',
            '1..2',
            'ok 1 - first',
            'not ok 2 - second',
            '  ---',
            '  message: assertion failed',
            '  severity: fail',
            '  at: sample.test.eta:14:3',
            '  expected: 42',
            '  actual: 41',
            '  ...',
        ].join('\n');
        const r = (0, testController_1.parseTap)(tap);
        assert.strictEqual(r.length, 2);
        assert.strictEqual(r[1].ok, false);
        assert.strictEqual(r[1].message, 'assertion failed');
        assert.strictEqual(r[1].severity, 'fail');
        assert.strictEqual(r[1].at, 'sample.test.eta:14:3');
        assert.strictEqual(r[1].expected, '42');
        assert.strictEqual(r[1].actual, '41');
    });
    it('handles missing dash before description', () => {
        const r = (0, testController_1.parseTap)('ok 7 just a description');
        assert.strictEqual(r.length, 1);
        assert.strictEqual(r[0].num, 7);
        assert.strictEqual(r[0].description, 'just a description');
    });
    it('ignores YAML keys outside YAML blocks', () => {
        const tap = [
            'not ok 1 - failed',
            '  message: should be ignored (no --- yet)',
            '  ---',
            '  message: real message',
            '  ...',
        ].join('\n');
        const r = (0, testController_1.parseTap)(tap);
        assert.strictEqual(r[0].message, 'real message');
    });
    it('handles CRLF line endings', () => {
        const tap = 'TAP version 13\r\n1..1\r\nok 1 - cr lf\r\n';
        const r = (0, testController_1.parseTap)(tap);
        assert.strictEqual(r.length, 1);
        assert.strictEqual(r[0].description, 'cr lf');
    });
});
//# sourceMappingURL=parseTap.test.js.map