/**
 * disassemblyView.ts — virtual `eta-disasm:` document provider.
 *
 * Exposes the latest disassembly response so that:
 *   - `showDisassembly` opens it in a side editor and reveals the PC line.
 *   - `EtaDisassemblyDefinitionProvider` (below) can resolve Call / TailCall
 *     instructions to the target function header (B4-3 jump-to-callee).
 */
import {
    window,
    workspace,
    debug,
    Uri,
    Range,
    Position,
    Selection,
    TextDocumentContentProvider,
    DefinitionProvider,
    Location,
    TextDocument,
    EventEmitter,
    ViewColumn,
    TextEditorRevealType,
} from 'vscode';

// ── Disassembly response type ─────────────────────────────────────────────────

export interface DisassemblyResult {
    text: string;
    functionName: string;
    /** Bytecode index of the next instruction to execute, or -1 when not paused. */
    currentPC: number;
}

// ── Shared parsing helpers ────────────────────────────────────────────────────

const FUNC_HEADER_RE = /^=== (.+?) ===$/;
/** A `LoadConst N  ; <func:M>` line — second capture is the function index. */
const LOADCONST_FUNC_RE = /^\s*\d+:\s+LoadConst\s+\d+\s*;\s*<func:(\d+)>/;
const CALL_RE = /^\s*\d+:\s+(?:Call|TailCall)\b/;
const INSTR_RE = /^\s*(\d+):\s/;

/** Find the first instruction line whose index equals currentPC, scoped to functionName. */
export function findPcLine(text: string, currentPC: number, functionName: string): number {
    if (currentPC < 0) return -1;
    const lines = text.split('\n');
    let inTargetFunc = !functionName; // when no name we accept any function
    for (let i = 0; i < lines.length; i++) {
        const m = lines[i].match(FUNC_HEADER_RE);
        if (m) {
            inTargetFunc = !functionName || m[1] === functionName
                || m[1].endsWith('.' + functionName);
            continue;
        }
        if (!inTargetFunc) continue;
        const im = lines[i].match(INSTR_RE);
        if (im && parseInt(im[1], 10) === currentPC) {
            return i;
        }
    }
    return -1;
}

/** Find the line of the Nth `=== ... ===` header in the text. */
export function findFunctionHeaderLine(text: string, funcIndex: number): number {
    const lines = text.split('\n');
    let count = 0;
    for (let i = 0; i < lines.length; i++) {
        if (FUNC_HEADER_RE.test(lines[i])) {
            if (count === funcIndex) return i;
            count++;
        }
    }
    return -1;
}

/**
 * Given a click line inside a Call / TailCall instruction, walk upward to the
 * most recent `LoadConst N  ; <func:M>` annotation in the same function block.
 * Returns the decoded `<func:M>` index, or undefined when no callee can be
 * inferred (e.g. dynamic calls).
 */
export function inferCalleeFuncIndex(text: string, clickLine: number): number | undefined {
    const lines = text.split('\n');
    if (clickLine < 0 || clickLine >= lines.length) return undefined;
    if (!CALL_RE.test(lines[clickLine])) return undefined;
    for (let i = clickLine - 1; i >= 0; i--) {
        if (FUNC_HEADER_RE.test(lines[i])) return undefined; // crossed function boundary
        const m = lines[i].match(LOADCONST_FUNC_RE);
        if (m) return parseInt(m[1], 10);
    }
    return undefined;
}

// ── Virtual document provider ────────────────────────────────────────────────

export class DisassemblyContentProvider implements TextDocumentContentProvider {
    private _onDidChange = new EventEmitter<Uri>();
    readonly onDidChange = this._onDidChange.event;

    private content: string = '; No disassembly available. Pause the VM first.';
    private scope: string = 'current';
    private latest: DisassemblyResult | undefined;

    setScope(scope: string): void {
        this.scope = scope;
    }

    /** Latest disassembly response (for definition / reveal helpers). */
    getLatest(): DisassemblyResult | undefined {
        return this.latest;
    }

    /** Currently displayed scope ('current' | 'all'). */
    currentScope(): string {
        return this.scope;
    }

    applyResult(result: DisassemblyResult | undefined, scope?: string): void {
        if (scope) {
            this.scope = scope;
        }
        if (!result) {
            this.content = '; No disassembly available.';
            this.latest = undefined;
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return;
        }
        this.content = result.text || '; (empty disassembly)';
        this.latest = result;
        this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
    }

    async refresh(): Promise<string> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.content = '; No active Eta debug session.';
            this.latest = undefined;
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return this.content;
        }

        try {
            const result = await session.customRequest('eta/disassemble', {
                scope: this.scope,
            }) as DisassemblyResult;
            this.content = result.text || '; (empty disassembly)';
            this.latest = result;
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return this.content;
        } catch (err: any) {
            this.content = `; Error: ${err?.message ?? String(err)}`;
            this.latest = undefined;
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return this.content;
        }
    }

    provideTextDocumentContent(_uri: Uri): string {
        return this.content;
    }

    static uri(scope: string): Uri {
        return Uri.parse(`eta-disasm:disassembly-${scope}.eta-bytecode`);
    }
}

// ── Open / reveal helpers ─────────────────────────────────────────────────────

/**
 * Open the disassembly document in a side column. When `revealPc` is true,
 * scroll the editor to the line of the current PC (if any).
 */
export async function showDisassembly(
    provider: DisassemblyContentProvider,
    scope: string = 'current',
    opts: { revealPc?: boolean; column?: ViewColumn; preserveFocus?: boolean } = {},
): Promise<void> {
    provider.setScope(scope);
    await provider.refresh();
    const uri = DisassemblyContentProvider.uri(scope);
    const doc = await workspace.openTextDocument(uri);
    await window.showTextDocument(doc, {
        preview: true,
        preserveFocus: opts.preserveFocus ?? false,
        viewColumn: opts.column ?? ViewColumn.Beside,
    });
    if (opts.revealPc !== false) {
        revealPcInDisassembly(provider);
    }
}

/**
 * If the disassembly document is currently visible, re-reveal the PC line.
 * Called on every `stopped` event so the current instruction stays in view.
 */
export function revealPcInDisassembly(provider: DisassemblyContentProvider): void {
    const latest = provider.getLatest();
    if (!latest || latest.currentPC < 0) return;
    const uri = DisassemblyContentProvider.uri(provider.currentScope());
    for (const ed of window.visibleTextEditors) {
        if (ed.document.uri.toString() !== uri.toString()) continue;
        const line = findPcLine(ed.document.getText(), latest.currentPC, latest.functionName);
        if (line < 0) continue;
        const range = new Range(line, 0, line, ed.document.lineAt(line).text.length);
        ed.revealRange(range, TextEditorRevealType.InCenterIfOutsideViewport);
        ed.selection = new Selection(range.start, range.start);
    }
}

/**
 * If the user has opted in via `eta.debug.autoShowDisassembly`, open the
 * disassembly side-by-side on every `stopped` event. Idempotent — when the
 * document is already visible, this just refreshes and re-reveals the PC line.
 */
export async function autoShowDisassemblyOnStop(
    provider: DisassemblyContentProvider,
    autoShow: boolean,
): Promise<void> {
    if (!autoShow) {
        revealPcInDisassembly(provider);
        return;
    }
    const uri = DisassemblyContentProvider.uri(provider.currentScope());
    const visible = window.visibleTextEditors.some(
        (ed) => ed.document.uri.toString() === uri.toString(),
    );
    if (visible) {
        revealPcInDisassembly(provider);
        return;
    }
    await showDisassembly(provider, provider.currentScope(), {
        revealPc: true,
        preserveFocus: true,
    });
}

// ── Definition provider — jump-to-callee on Call / TailCall lines ────────────

export class EtaDisassemblyDefinitionProvider implements DefinitionProvider {
    provideDefinition(document: TextDocument, position: Position): Location | undefined {
        if (document.uri.scheme !== 'eta-disasm') return undefined;
        const text = document.getText();
        const callee = inferCalleeFuncIndex(text, position.line);
        if (callee === undefined) return undefined;
        const targetLine = findFunctionHeaderLine(text, callee);
        if (targetLine < 0) return undefined;
        return new Location(document.uri, new Position(targetLine, 0));
    }
}

