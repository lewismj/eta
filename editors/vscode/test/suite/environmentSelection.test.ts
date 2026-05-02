import * as assert from 'assert';
import type { DebugSession } from 'vscode';
import { resolveEnvironmentSelection } from '../../src/gcRootsTreeView';

describe('environment selection', () => {
    const session = { id: 'eta-session' } as DebugSession;

    it('falls back to thread 1 frame 0 when follow is disabled', () => {
        const sel = resolveEnvironmentSelection(session, false, {
            session: { id: 'eta-session' },
            threadId: 9,
            frameId: 42,
        });
        assert.deepStrictEqual(sel, { threadId: 1, frameIndex: 0 });
    });

    it('falls back when active stack item is missing or from another session', () => {
        assert.deepStrictEqual(resolveEnvironmentSelection(session, true, undefined), {
            threadId: 1,
            frameIndex: 0,
        });
        assert.deepStrictEqual(resolveEnvironmentSelection(session, true, {
            session: { id: 'other-session' },
            threadId: 3,
            frameId: 12,
        }), {
            threadId: 1,
            frameIndex: 0,
        });
    });

    it('extracts threadId and low 16 bits of frameId', () => {
        const sel = resolveEnvironmentSelection(session, true, {
            session: { id: 'eta-session' },
            threadId: 4,
            frameId: 0x12345,
        });
        assert.deepStrictEqual(sel, { threadId: 4, frameIndex: 0x2345 });
    });
});
