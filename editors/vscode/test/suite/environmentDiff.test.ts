import * as assert from 'assert';
import { computeChangedBindingKeys } from '../../src/environmentDiff';
import type { EnvironmentSnapshot } from '../../src/dapTypes';

function snapshotWithBindings(values: Record<string, string>): EnvironmentSnapshot {
    return {
        threadId: 1,
        frameIndex: 0,
        frameName: 'f',
        moduleName: 'm',
        environments: [
            {
                kind: 'locals',
                label: 'Frame locals',
                depth: 0,
                total: Object.keys(values).length,
                truncated: false,
                bindings: Object.entries(values).map(([name, value]) => ({
                    name,
                    value,
                    variablesReference: 0,
                })),
            },
        ],
    };
}

describe('environment diff', () => {
    it('marks all bindings as changed on first snapshot', () => {
        const current = snapshotWithBindings({ a: '1', b: '2' });
        const changed = computeChangedBindingKeys(undefined, current);
        assert.strictEqual(changed.size, 2);
        assert.ok(changed.has('locals:0:a'));
        assert.ok(changed.has('locals:0:b'));
    });

    it('returns no changes when snapshots are identical', () => {
        const before = snapshotWithBindings({ a: '1', b: '2' });
        const after = snapshotWithBindings({ a: '1', b: '2' });
        const changed = computeChangedBindingKeys(before, after);
        assert.strictEqual(changed.size, 0);
    });

    it('marks value changes and new bindings', () => {
        const before = snapshotWithBindings({ a: '1', b: '2' });
        const after = snapshotWithBindings({ a: '1', b: '3', c: '9' });
        const changed = computeChangedBindingKeys(before, after);
        assert.strictEqual(changed.size, 2);
        assert.ok(changed.has('locals:0:b'));
        assert.ok(changed.has('locals:0:c'));
    });
});
