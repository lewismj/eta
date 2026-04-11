import {
    window,
    workspace,
    debug,
    Uri,
    TextDocumentContentProvider,
    EventEmitter,
} from 'vscode';

// ── Disassembly response type ─────────────────────────────────────────────────

interface DisassemblyResult {
    text: string;
    functionName: string;
    currentPC: number;
}

// ── Virtual document provider for eta-disasm: URIs ────────────────────────────

export class DisassemblyContentProvider implements TextDocumentContentProvider {
    private _onDidChange = new EventEmitter<Uri>();
    readonly onDidChange = this._onDidChange.event;

    private content: string = '; No disassembly available. Pause the VM first.';
    private scope: string = 'current';

    setScope(scope: string): void {
        this.scope = scope;
    }

    async refresh(): Promise<string> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.content = '; No active Eta debug session.';
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return this.content;
        }

        try {
            const result = await session.customRequest('eta/disassemble', {
                scope: this.scope,
            }) as DisassemblyResult;
            this.content = result.text || '; (empty disassembly)';
            this._onDidChange.fire(DisassemblyContentProvider.uri(this.scope));
            return this.content;
        } catch (err: any) {
            this.content = `; Error: ${err?.message ?? String(err)}`;
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

// ── Commands ──────────────────────────────────────────────────────────────────

export async function showDisassembly(
    provider: DisassemblyContentProvider,
    scope: string = 'current',
): Promise<void> {
    provider.setScope(scope);
    await provider.refresh();
    const uri = DisassemblyContentProvider.uri(scope);
    const doc = await workspace.openTextDocument(uri);
    await window.showTextDocument(doc, { preview: true, preserveFocus: false });
}
