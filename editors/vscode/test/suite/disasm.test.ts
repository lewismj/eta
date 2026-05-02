/**
 * disasm.test.ts — exercises the pure parser helpers used by the grouped
 * disassembly tree (B4-2) and the jump-to-callee definition provider (B4-3).
 */
import * as assert from 'assert';
import {
    findPcLine,
    findFunctionHeaderLine,
    inferCalleeFuncIndex,
    parseInstructionIndex,
    computeDisassemblyLineRangeForSource,
} from '../../src/disassemblyView';
import { parseFunctions, styleDisassemblyTreeLine } from '../../src/disassemblyTreeView';

const SAMPLE = [
    '; 2 function(s)',
    '',
    '=== outer ===',
    '  arity:      0',
    '  -- constant pool --',
    '    [0] <func:1>',
    '    [1] 42',
    '  -- code --',
    '       0: LoadConst              0  ; <func:1>',
    '       1: Call                   0',
    '       2: Return',
    '',
    '=== inner ===',
    '  arity:      0',
    '  -- constant pool --',
    '    [0] 7',
    '  -- code --',
    '       0: LoadConst              0  ; 7',
    '       1: Return',
    '',
].join('\n');

describe('disassembly parser (B4)', () => {
    it('groups by function header', () => {
        const funcs = parseFunctions(SAMPLE, /* currentPC */ 1);
        assert.strictEqual(funcs.length, 2);
        assert.strictEqual(funcs[0].name, 'outer');
        assert.strictEqual(funcs[1].name, 'inner');
        assert.strictEqual(funcs[0].instructions.length, 3);
        assert.strictEqual(funcs[1].instructions.length, 2);
        assert.deepStrictEqual(funcs[0].constantPool, ['[0] <func:1>', '[1] 42']);
        assert.strictEqual(funcs[0].containsPc, true);
        assert.strictEqual(funcs[1].containsPc, false);
    });

    it('marks the PC line and tags Call lines with their callee index', () => {
        const funcs = parseFunctions(SAMPLE, 1);
        const call = funcs[0].instructions.find((i) => /Call\b/.test(i.text))!;
        assert.strictEqual(call.callTarget, 1, 'Call target should match preceding LoadConst <func:1>');
        const pcLine = funcs[0].instructions.find((i) => i.isCurrentPC);
        assert.ok(pcLine, 'one instruction should be marked as PC');
        assert.strictEqual(pcLine!.instructionIndex, 1);
    });

    it('findPcLine resolves the buffer line of the current PC', () => {
        const line = findPcLine(SAMPLE, 1, 'outer');
        assert.ok(line >= 0);
        assert.match(SAMPLE.split('\n')[line], /^\s*1:\s+Call/);
    });

    it('findFunctionHeaderLine indexes by registry order', () => {
        const lines = SAMPLE.split('\n');
        const outerLine = findFunctionHeaderLine(SAMPLE, 0);
        const innerLine = findFunctionHeaderLine(SAMPLE, 1);
        assert.strictEqual(lines[outerLine], '=== outer ===');
        assert.strictEqual(lines[innerLine], '=== inner ===');
        assert.strictEqual(findFunctionHeaderLine(SAMPLE, 2), -1);
    });

    it('inferCalleeFuncIndex walks back to the nearest LoadConst <func:N>', () => {
        const lines = SAMPLE.split('\n');
        const callLineIdx = lines.findIndex((l) => /Call\b/.test(l));
        assert.strictEqual(inferCalleeFuncIndex(SAMPLE, callLineIdx), 1);
        // Non-Call line returns undefined.
        const returnIdx = lines.findIndex((l) => /Return\b/.test(l));
        assert.strictEqual(inferCalleeFuncIndex(SAMPLE, returnIdx), undefined);
        // Crossing a function boundary returns undefined.
        const innerCall = lines.findIndex((l, i) => i > callLineIdx && /Call\b/.test(l));
        if (innerCall >= 0) {
            assert.strictEqual(inferCalleeFuncIndex(SAMPLE, innerCall), undefined);
        }
    });

    it('classifies disassembly lines for sidebar syntax-like styling', () => {
        const call = styleDisassemblyTreeLine('       1: Call                   0', { callTarget: 1 });
        assert.strictEqual(call.iconId, 'arrow-right');
        assert.strictEqual(call.colorId, 'symbolIcon.functionForeground');

        const constant = styleDisassemblyTreeLine('       0: LoadConst              0  ; 7');
        assert.strictEqual(constant.iconId, 'symbol-number');
        assert.strictEqual(constant.colorId, 'symbolIcon.numberForeground');

        const control = styleDisassemblyTreeLine('       2: Return');
        assert.strictEqual(control.iconId, 'debug-step-over');
        assert.strictEqual(control.colorId, 'symbolIcon.keywordForeground');

        const current = styleDisassemblyTreeLine('       1: Call                   0', { isCurrentPC: true });
        assert.strictEqual(current.iconId, 'debug-stackframe');
        assert.strictEqual(current.colorId, 'editorInfo.foreground');
        assert.strictEqual(current.description, 'PC');
    });

    it('extracts instruction indices and computes source-correlated bytecode ranges', () => {
        assert.strictEqual(parseInstructionIndex('       7: Add'), 7);
        assert.strictEqual(parseInstructionIndex('not an instruction'), undefined);

        const range = computeDisassemblyLineRangeForSource(SAMPLE, [0, 1]);
        assert.ok(range);
        assert.strictEqual(range!.startLine, 8);
        assert.strictEqual(range!.endLine, 18);
    });
});

