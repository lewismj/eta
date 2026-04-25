/**
 * disassemblyTreeView.ts — function-grouped bytecode disassembly in the
 * Debug sidebar (B4-2 of docs/dap_vs_plan.md).
 *
 * Two-level tree:
 *   ▾ === function-name ===          (DisasmFunctionNode, collapsible)
 *       ▸ Constant pool (N)          (DisasmGroupNode, collapsible)
 *         [0] 42
 *         [1] "hello"
 *       0: LoadConst 0  ; 42         (DisasmLineNode, leaf — current PC = ◀)
 *       1: Add
 *       …
 *
 * The function block containing the current PC is auto-expanded; all others
 * stay collapsed for fast triage on large modules. Call / TailCall lines
 * carry a `command` that jumps to the callee's `=== name ===` header in the
 * companion virtual document (B4-3).
 */
import {
    TreeDataProvider,
    TreeItem,
    TreeItemCollapsibleState,
    EventEmitter,
    Event,
    debug,
    ThemeIcon,
    Command,
} from 'vscode';
import {
    DisassemblyContentProvider,
    inferCalleeFuncIndex,
    findFunctionHeaderLine,
    type DisassemblyResult,
} from './disassemblyView';

// ── Node kinds ────────────────────────────────────────────────────────────────

type Node = DisasmFunctionNode | DisasmGroupNode | DisasmLineNode;

export class DisasmFunctionNode {
    readonly kind = 'function' as const;
    constructor(
        public readonly name: string,
        public readonly funcIndex: number,
        public readonly headerLines: string[],
        public readonly constantPool: string[],
        public readonly instructions: DisasmLineNode[],
        public readonly containsPc: boolean,
    ) {}
}

export class DisasmGroupNode {
    readonly kind = 'group' as const;
    constructor(
        public readonly label: string,
        public readonly children: DisasmLineNode[],
    ) {}
}

export class DisasmLineNode {
    readonly kind = 'line' as const;
    constructor(
        public readonly text: string,
        public readonly instructionIndex: number | undefined,
        public readonly isCurrentPC: boolean,
        public readonly isHeader: boolean,
        public readonly callTarget?: number, // func index for Call/TailCall lines
    ) {}
}

// ── Parsing ───────────────────────────────────────────────────────────────────

const FUNC_HEADER_RE = /^=== (.+?) ===$/;
const CONST_POOL_RE = /^\s*-- constant pool --/;
const CODE_RE = /^\s*-- code --/;
const INSTR_RE = /^\s*(\d+):\s+(\S+)/;
const MAX_TREE_TEXT_BYTES = 400_000;

function parseFunctions(text: string, currentPC: number, currentFunction?: string): DisasmFunctionNode[] {
    const lines = text.split('\n');
    const functions: DisasmFunctionNode[] = [];

    /** Buffer state for the function currently being assembled. */
    let funcIdx = 0;
    let curName = '';
    let curHeader: string[] = [];
    let curConsts: string[] = [];
    let curInstrs: DisasmLineNode[] = [];
    let mode: 'header' | 'consts' | 'code' = 'header';
    /** Have we seen a `=== name ===` header yet? Lines before the first
     *  one (e.g. the leading `; N function(s)` comment emitted by the
     *  disassembler) must not be folded into a phantom unnamed function. */
    let sawHeader = false;
    /** Once we've marked a PC line in any function we don't mark another;
     *  PC indices are per-function and would otherwise spuriously match
     *  the same index in every later function. When `currentFunction` is
     *  supplied we restrict marking to that function only. */
    let pcAssigned = false;

    const flush = () => {
        if (!sawHeader) return;
        if (!curName && curHeader.length === 0 && curInstrs.length === 0) return;
        const containsPc = curInstrs.some((n) => n.isCurrentPC);
        functions.push(new DisasmFunctionNode(
            curName,
            funcIdx,
            curHeader.slice(),
            curConsts.slice(),
            curInstrs.slice(),
            containsPc,
        ));
        funcIdx++;
        curName = '';
        curHeader = [];
        curConsts = [];
        curInstrs = [];
        mode = 'header';
    };

    for (const raw of lines) {
        const line = raw.trimEnd();
        if (!line) continue;

        const fh = line.match(FUNC_HEADER_RE);
        if (fh) {
            flush();
            sawHeader = true;
            curName = fh[1];
            curHeader.push(line);
            mode = 'header';
            continue;
        }
        if (!sawHeader) continue; // skip preamble (e.g. `; N function(s)`)
        if (CONST_POOL_RE.test(line)) { mode = 'consts'; continue; }
        if (CODE_RE.test(line))       { mode = 'code';   continue; }

        if (mode === 'header') {
            curHeader.push(line);
            continue;
        }
        if (mode === 'consts') {
            curConsts.push(line.replace(/^\s+/, ''));
            continue;
        }
        // mode === 'code'
        const im = line.match(INSTR_RE);
        if (!im) continue;
        const instructionIndex = parseInt(im[1], 10);
        const opcode = im[2];
        const inTargetFunction = currentFunction === undefined
            || curName === currentFunction
            || curName.endsWith('.' + currentFunction);
        const isPc = !pcAssigned
            && currentPC >= 0
            && instructionIndex === currentPC
            && inTargetFunction;
        if (isPc) pcAssigned = true;
        const isCallish = opcode === 'Call' || opcode === 'TailCall';
        let callTarget: number | undefined;
        if (isCallish) {
            // Walk back through the in-progress instruction buffer to find the
            // most recent LoadConst <func:M> annotation.
            for (let k = curInstrs.length - 1; k >= 0; k--) {
                const m = curInstrs[k].text.match(
                    /^\s*\d+:\s+LoadConst\s+\d+\s*;\s*<func:(\d+)>/,
                );
                if (m) { callTarget = parseInt(m[1], 10); break; }
            }
        }
        curInstrs.push(new DisasmLineNode(line, instructionIndex, isPc, false, callTarget));
    }
    flush();
    return functions;
}

// ── Tree provider ─────────────────────────────────────────────────────────────

export class DisassemblyTreeProvider implements TreeDataProvider<Node> {
    private _onDidChangeTreeData = new EventEmitter<Node | undefined | void>();
    readonly onDidChangeTreeData: Event<Node | undefined | void> = this._onDidChangeTreeData.event;

    private functions: DisasmFunctionNode[] = [];
    private flatLines: DisasmLineNode[] | undefined; // fallback when parsing yields no functions
    private latest: DisassemblyResult | undefined;

    constructor(private readonly contentProvider?: DisassemblyContentProvider) {}

    refresh(): void {
        this.fetchDisassembly().then(() => this._onDidChangeTreeData.fire());
    }

    applyResult(result: DisassemblyResult | undefined): void {
        this.setFromResult(result);
        this._onDidChangeTreeData.fire();
    }

    notifyStopped(): void {
        this.refresh();
    }

    private async fetchDisassembly(): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.functions = [];
            this.flatLines = [
                new DisasmLineNode('; No active Eta debug session.', undefined, false, true),
            ];
            this.latest = undefined;
            return;
        }
        try {
            const result = await session.customRequest('eta/disassemble', {
                scope: 'current',
            }) as DisassemblyResult;
            this.setFromResult(result);
        } catch {
            this.functions = [];
            this.flatLines = [
                new DisasmLineNode('; Failed to fetch disassembly.', undefined, false, true),
            ];
            this.latest = undefined;
        }
    }

    private setFromResult(result: DisassemblyResult | undefined): void {
        if (!result) {
            this.functions = [];
            this.flatLines = [
                new DisasmLineNode('; No disassembly available.', undefined, false, true),
            ];
            this.latest = undefined;
            return;
        }
        const text = result.text || '; (empty disassembly)';
        if (text.length > MAX_TREE_TEXT_BYTES) {
            this.functions = [];
            this.flatLines = [
                new DisasmLineNode('; Disassembly too large for the sidebar tree view.', undefined, false, true),
                new DisasmLineNode('; Use "Eta: Show Disassembly" for the full text output.', undefined, false, true),
            ];
            this.latest = undefined;
            return;
        }
        this.latest = result;
        const funcs = parseFunctions(text, result.currentPC ?? -1, result.functionName);
        if (funcs.length === 0) {
            // Fall back to flat rendering when the response carried no
            // function header (older adapters).
            this.functions = [];
            this.flatLines = text.split('\n')
                .map((l) => l.trimEnd())
                .filter((l) => l.length > 0)
                .map((l) => {
                    const im = l.match(INSTR_RE);
                    const idx = im ? parseInt(im[1], 10) : undefined;
                    const isPc = idx !== undefined && idx === result.currentPC;
                    const isHeader = l.startsWith(';') || l.startsWith('==');
                    return new DisasmLineNode(l, idx, isPc, isHeader);
                });
        } else {
            this.functions = funcs;
            this.flatLines = undefined;
        }
    }

    getTreeItem(element: Node): TreeItem {
        switch (element.kind) {
            case 'function': {
                const expanded = element.containsPc;
                const item = new TreeItem(
                    `=== ${element.name || '<anonymous>'} ===`,
                    expanded ? TreeItemCollapsibleState.Expanded : TreeItemCollapsibleState.Collapsed,
                );
                item.iconPath = new ThemeIcon(element.containsPc ? 'debug-stackframe' : 'symbol-function');
                item.description = `${element.instructions.length} instr` + (element.containsPc ? '  ◀ PC' : '');
                item.tooltip = element.headerLines.join('\n');
                item.contextValue = 'etaDisasm.function';
                return item;
            }
            case 'group': {
                const item = new TreeItem(
                    `${element.label} (${element.children.length})`,
                    TreeItemCollapsibleState.Collapsed,
                );
                item.iconPath = new ThemeIcon('symbol-array');
                return item;
            }
            case 'line': {
                const item = new TreeItem(element.text, TreeItemCollapsibleState.None);
                if (element.isCurrentPC) {
                    item.iconPath = new ThemeIcon('debug-stackframe');
                    item.description = '◀ PC';
                } else if (element.isHeader) {
                    item.iconPath = new ThemeIcon('symbol-function');
                } else if (element.callTarget !== undefined) {
                    item.iconPath = new ThemeIcon('arrow-right');
                } else {
                    item.iconPath = new ThemeIcon('circle-outline');
                }
                item.tooltip = element.isCurrentPC
                    ? `Current instruction (PC)\n${element.text}`
                    : element.text;
                if (element.callTarget !== undefined) {
                    const cmd: Command = {
                        command: 'eta.disassembly.gotoCallee',
                        title: 'Go to callee',
                        arguments: [element.callTarget],
                    };
                    item.command = cmd;
                    item.contextValue = 'etaDisasm.call';
                }
                return item;
            }
        }
    }

    getChildren(element?: Node): Node[] {
        if (!element) {
            if (this.functions.length > 0) return this.functions;
            return this.flatLines ?? [];
        }
        if (element.kind === 'function') {
            const out: Node[] = [];
            if (element.constantPool.length > 0) {
                const constLines = element.constantPool.map(
                    (t) => new DisasmLineNode(t, undefined, false, true),
                );
                out.push(new DisasmGroupNode('constant pool', constLines));
            }
            out.push(...element.instructions);
            return out;
        }
        if (element.kind === 'group') return element.children;
        return [];
    }

    /** Return the func index whose header should be revealed in the document. */
    findCalleeHeaderLine(funcIndex: number): { uri: string; line: number } | undefined {
        if (!this.contentProvider || !this.latest) return undefined;
        const line = findFunctionHeaderLine(this.latest.text, funcIndex);
        if (line < 0) return undefined;
        return {
            uri: DisassemblyContentProvider.uri(this.contentProvider.currentScope()).toString(),
            line,
        };
    }
}

// Re-export for tests
export { parseFunctions, inferCalleeFuncIndex };

