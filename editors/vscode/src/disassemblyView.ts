/**
 * disassemblyView.ts - virtual `eta-disasm:` document provider and helpers.
 *
 * Exposes the latest disassembly response so that:
 *   - `showDisassembly` opens it in a side editor and reveals the PC line.
 *   - definition providers can resolve bytecode <-> source locations.
 */
import {
    window,
    workspace,
    languages,
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
    type DebugSession,
} from 'vscode';

export interface DisassemblyResult {
    text: string;
    functionName: string;
    /** Bytecode index of the next instruction to execute, or -1 when not paused. */
    currentPC: number;
}

export interface DisassemblySourceLocation {
    path: string;
    line: number; // 1-based source line
    name?: string;
}

interface StandardDisassembleInstruction {
    address?: string;
    line?: number;
    location?: {
        path?: string;
        name?: string;
    };
}

interface StandardDisassembleResponse {
    instructions?: StandardDisassembleInstruction[];
}

const FUNC_HEADER_RE = /^=== (.+?) ===$/;
/** A `LoadConst N  ; <func:M>` line - second capture is the function index. */
const LOADCONST_FUNC_RE = /^\s*\d+\s*:\s+LoadConst\s+\d+\s*;\s*<func:(\d+)>/;
const CALL_RE = /^\s*\d+\s*:\s+(?:Call|TailCall)\b/;
const INSTR_RE = /^\s*(\d+)\s*:\s/;

function normalizeSourcePath(pathText: string): string {
    const normalized = pathText.replace(/\\/g, '/');
    return process.platform === 'win32'
        ? normalized.toLowerCase()
        : normalized;
}

function sourceLookupKey(uri: Uri, sourceLine1: number): string {
    return `${normalizeSourcePath(uri.fsPath)}:${sourceLine1}`;
}

export function parseInstructionIndex(lineText: string): number | undefined {
    const m = lineText.match(INSTR_RE);
    if (!m) return undefined;
    const n = parseInt(m[1], 10);
    return Number.isFinite(n) ? n : undefined;
}

export function indexDisassemblyInstructionLines(text: string): Map<number, number[]> {
    const out = new Map<number, number[]>();
    const lines = text.split('\n');
    for (let i = 0; i < lines.length; i++) {
        const idx = parseInstructionIndex(lines[i]);
        if (idx === undefined) continue;
        const bucket = out.get(idx);
        if (bucket) bucket.push(i);
        else out.set(idx, [i]);
    }
    return out;
}

export function computeDisassemblyLineRangeForSource(
    disassemblyText: string,
    instructionIndices: number[],
): { startLine: number; endLine: number } | undefined {
    if (!instructionIndices.length) return undefined;
    const byInstr = indexDisassemblyInstructionLines(disassemblyText);
    const lines: number[] = [];
    for (const idx of instructionIndices) {
        const hits = byInstr.get(idx);
        if (hits && hits.length > 0) {
            lines.push(...hits);
        }
    }
    if (!lines.length) return undefined;
    lines.sort((a, b) => a - b);
    return {
        startLine: lines[0],
        endLine: lines[lines.length - 1],
    };
}

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

export class DisassemblyContentProvider implements TextDocumentContentProvider {
    private _onDidChange = new EventEmitter<Uri>();
    readonly onDidChange = this._onDidChange.event;

    private content: string = '; No disassembly available. Pause the VM first.';
    private scope: string = 'current';
    private latest: DisassemblyResult | undefined;

    private sourceByInstruction = new Map<number, DisassemblySourceLocation>();
    private instructionBySource = new Map<string, number[]>();

    setScope(scope: string): void {
        if (this.scope !== scope) {
            this.scope = scope;
            this.clearSourceCorrelation();
        } else {
            this.scope = scope;
        }
    }

    /** Latest disassembly response (for definition / reveal helpers). */
    getLatest(): DisassemblyResult | undefined {
        return this.latest;
    }

    /** Currently displayed scope ('current' | 'all'). */
    currentScope(): string {
        return this.scope;
    }

    currentText(): string {
        return this.content;
    }

    sourceForInstruction(instructionIndex: number): DisassemblySourceLocation | undefined {
        return this.sourceByInstruction.get(instructionIndex);
    }

    instructionIndicesForSource(sourceUri: Uri, sourceLine1: number): number[] {
        const key = sourceLookupKey(sourceUri, sourceLine1);
        return this.instructionBySource.get(key) ?? [];
    }

    applyResult(result: DisassemblyResult | undefined, scope?: string): void {
        if (scope) {
            this.scope = scope;
        }
        if (this.scope === 'current') {
            this.clearSourceCorrelation();
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
            this.clearSourceCorrelation();
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return this.content;
        }

        try {
            const result = await session.customRequest('eta/disassemble', {
                scope: this.scope,
            }) as DisassemblyResult;
            this.content = result.text || '; (empty disassembly)';
            this.latest = result;
            if (this.scope === 'current') {
                await this.refreshSourceCorrelation(session);
            } else {
                this.clearSourceCorrelation();
            }
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return this.content;
        } catch (err: any) {
            this.content = `; Error: ${err?.message ?? String(err)}`;
            this.latest = undefined;
            this.clearSourceCorrelation();
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return this.content;
        }
    }

    async ensureSourceCorrelation(): Promise<void> {
        if (this.scope !== 'current') {
            return;
        }
        if (this.sourceByInstruction.size > 0) {
            return;
        }
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            return;
        }
        await this.refreshSourceCorrelation(session);
    }

    private clearSourceCorrelation(): void {
        this.sourceByInstruction.clear();
        this.instructionBySource.clear();
    }

    private async refreshSourceCorrelation(session: DebugSession): Promise<void> {
        this.clearSourceCorrelation();
        const resp = await session.customRequest('disassemble', {
            memoryReference: 'current',
            instructionOffset: 0,
            instructionCount: 200000,
        }) as StandardDisassembleResponse;
        const instructions = Array.isArray(resp?.instructions) ? resp.instructions : [];
        for (const inst of instructions) {
            if (!inst || typeof inst.address !== 'string') continue;
            const instructionIndex = parseInt(inst.address, 10);
            if (!Number.isFinite(instructionIndex)) continue;
            if (!inst.location || !inst.location.path) continue;
            const line = Number(inst.line);
            if (!Number.isFinite(line) || line <= 0) continue;

            const location: DisassemblySourceLocation = {
                path: inst.location.path,
                line,
                name: inst.location.name,
            };
            this.sourceByInstruction.set(instructionIndex, location);

            const srcKey = `${normalizeSourcePath(location.path)}:${location.line}`;
            const bucket = this.instructionBySource.get(srcKey);
            if (bucket) bucket.push(instructionIndex);
            else this.instructionBySource.set(srcKey, [instructionIndex]);
        }

        for (const [key, indices] of this.instructionBySource) {
            const uniq = Array.from(new Set(indices)).sort((a, b) => a - b);
            this.instructionBySource.set(key, uniq);
        }
    }

    provideTextDocumentContent(_uri: Uri): string {
        return this.content;
    }

    static uri(scope: string): Uri {
        return Uri.parse(`eta-disasm:disassembly-${scope}.eta-bytecode`);
    }
}

/**
 * Resolve source location for an instruction line in a disassembly document.
 */
export async function resolveSourceLocationForDisassemblyLine(
    provider: DisassemblyContentProvider,
    document: TextDocument,
    disassemblyLine: number,
): Promise<DisassemblySourceLocation | undefined> {
    if (document.uri.scheme !== 'eta-disasm') return undefined;
    if (disassemblyLine < 0 || disassemblyLine >= document.lineCount) return undefined;
    const instructionIndex = parseInstructionIndex(document.lineAt(disassemblyLine).text);
    if (instructionIndex === undefined) return undefined;
    await provider.ensureSourceCorrelation();
    return provider.sourceForInstruction(instructionIndex);
}

/**
 * Resolve disassembly line range for a source location.
 */
export async function resolveDisassemblyRangeForSourceLine(
    provider: DisassemblyContentProvider,
    sourceUri: Uri,
    sourceLine1: number,
): Promise<{ startLine: number; endLine: number } | undefined> {
    await provider.ensureSourceCorrelation();
    const instructionIndices = provider.instructionIndicesForSource(sourceUri, sourceLine1);
    if (!instructionIndices.length) return undefined;
    return computeDisassemblyLineRangeForSource(provider.currentText(), instructionIndices);
}

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
    let doc = await workspace.openTextDocument(uri);
    if (doc.languageId !== 'eta-bytecode') {
        try {
            doc = await languages.setTextDocumentLanguage(doc, 'eta-bytecode');
        } catch {
        }
    }
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
 * disassembly side-by-side on every `stopped` event. Idempotent - when the
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

export class EtaDisassemblyDefinitionProvider implements DefinitionProvider {
    constructor(private readonly provider: DisassemblyContentProvider) {}

    async provideDefinition(document: TextDocument, position: Position): Promise<Location | undefined> {
        if (document.uri.scheme !== 'eta-disasm') return undefined;
        const text = document.getText();
        const callee = inferCalleeFuncIndex(text, position.line);
        if (callee !== undefined) {
            const targetLine = findFunctionHeaderLine(text, callee);
            if (targetLine >= 0) {
                return new Location(document.uri, new Position(targetLine, 0));
            }
        }

        const source = await resolveSourceLocationForDisassemblyLine(
            this.provider,
            document,
            position.line,
        );
        if (!source) return undefined;
        const sourceUri = Uri.file(source.path);
        const line = Math.max(0, source.line - 1);
        return new Location(sourceUri, new Position(line, 0));
    }
}
