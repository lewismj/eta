import {
    TreeDataProvider,
    TreeItem,
    TreeItemCollapsibleState,
    EventEmitter,
    Event,
    debug,
    ThemeIcon,
} from 'vscode';

// ── Node types ────────────────────────────────────────────────────────────────

/** A single bytecode instruction line in the disassembly tree. */
export class DisasmLineNode {
    constructor(
        public readonly text: string,
        public readonly lineIndex: number,
        public readonly instructionIndex: number | undefined,
        public readonly isCurrentPC: boolean,
        public readonly isHeader: boolean,
    ) {}
}

// ── Disassembly response type ─────────────────────────────────────────────────

interface DisassemblyResult {
    text: string;
    functionName: string;
    currentPC: number;
}

// ── Tree data provider ───────────────────────────────────────────────────────

export class DisassemblyTreeProvider implements TreeDataProvider<DisasmLineNode> {
    private _onDidChangeTreeData = new EventEmitter<DisasmLineNode | undefined | void>();
    readonly onDidChangeTreeData: Event<DisasmLineNode | undefined | void> = this._onDidChangeTreeData.event;

    private lines: DisasmLineNode[] = [];

    refresh(): void {
        this.fetchDisassembly().then(() => {
            this._onDidChangeTreeData.fire();
        });
    }

    notifyStopped(): void {
        this.refresh();
    }

    private parseInstructionIndex(line: string): number | undefined {
        const m = line.match(/^\s*(\d+):\s/);
        return m ? parseInt(m[1], 10) : undefined;
    }

    private async fetchDisassembly(): Promise<void> {
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            this.lines = [new DisasmLineNode('; No active Eta debug session.', 0, undefined, false, false)];
            return;
        }
        try {
            const result = await session.customRequest('eta/disassemble', {
                scope: 'current',
            }) as DisassemblyResult;

            const text = result.text || '; (empty disassembly)';
            const rawLines = text.split('\n');
            const pcInstruction = result.currentPC ?? -1;

            this.lines = rawLines.map((line, i) => {
                const trimmed = line.trimEnd();
                if (!trimmed) return null; // skip blank lines
                const instructionIndex = this.parseInstructionIndex(trimmed);
                const isPC = instructionIndex !== undefined && instructionIndex === pcInstruction;
                const isHeader = trimmed.startsWith(';') || trimmed.startsWith('==');
                return new DisasmLineNode(trimmed, i, instructionIndex, isPC, isHeader);
            }).filter((n): n is DisasmLineNode => n !== null);
        } catch {
            this.lines = [new DisasmLineNode('; Failed to fetch disassembly.', 0, undefined, false, false)];
        }
    }

    getTreeItem(element: DisasmLineNode): TreeItem {
        const item = new TreeItem(element.text, TreeItemCollapsibleState.None);

        if (element.isCurrentPC) {
            item.iconPath = new ThemeIcon('debug-stackframe');
            item.description = '◀ PC';
        } else if (element.isHeader) {
            item.iconPath = new ThemeIcon('symbol-function');
        } else {
            item.iconPath = new ThemeIcon('circle-outline');
        }

        item.tooltip = element.isCurrentPC
            ? `Current instruction (PC)\n${element.text}`
            : element.text;

        return item;
    }

    getChildren(element?: DisasmLineNode): DisasmLineNode[] {
        if (element) return []; // flat list, no children
        return this.lines;
    }
}

