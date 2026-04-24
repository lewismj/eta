/**
 * parseTap.test.ts — unit tests for the TAP parser used by the test controller.
 */
import * as assert from 'assert';
import { parseTap } from '../../src/testController';

describe('parseTap', () => {
    it('parses an empty plan as no results', () => {
        const out = parseTap('TAP version 13\n1..0\n');
        assert.strictEqual(out.length, 0);
    });

    it('parses a single passing test', () => {
        const tap = [
            'TAP version 13',
            '1..1',
            'ok 1 - addition works',
        ].join('\n');
        const r = parseTap(tap);
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
        const r = parseTap(tap);
        assert.strictEqual(r.length, 2);
        assert.strictEqual(r[1].ok, false);
        assert.strictEqual(r[1].message, 'assertion failed');
        assert.strictEqual(r[1].severity, 'fail');
        assert.strictEqual(r[1].at, 'sample.test.eta:14:3');
        assert.strictEqual(r[1].expected, '42');
        assert.strictEqual(r[1].actual, '41');
    });

    it('handles missing dash before description', () => {
        const r = parseTap('ok 7 just a description');
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
        const r = parseTap(tap);
        assert.strictEqual(r[0].message, 'real message');
    });

    it('handles CRLF line endings', () => {
        const tap = 'TAP version 13\r\n1..1\r\nok 1 - cr lf\r\n';
        const r = parseTap(tap);
        assert.strictEqual(r.length, 1);
        assert.strictEqual(r[0].description, 'cr lf');
    });
});

