import {
    DebugSession,
    debug,
    Event,
    EventEmitter,
    MarkdownString,
    ThemeColor,
    ThemeIcon,
    workspace,
    TreeDataProvider,
    TreeItem,
    TreeItemCollapsibleState,
} from 'vscode';
import type { DebugVariable, EnvironmentLevel, EnvironmentSnapshot } from './dapTypes';

export interface EnvironmentSettings {
    followActiveFrame: boolean;
    showLocals: boolean;
    showClosures: boolean;
    showGlobals: boolean;
    showBuiltins: boolean;
    showInternal: boolean;
    showNil: boolean;
    showChangedOnly: boolean;
}

export interface EnvironmentSelection {
    threadId: number;
    frameIndex: number;
}

type StackItemLike = {
    session?: { id: string };
    threadId?: number;
    frameId?: number;
} | undefined;

export type EnvironmentNode =
    | EnvironmentScopeNode
    | EnvironmentVariableNode
    | EnvironmentMessageNode;

export class EnvironmentScopeNode {
    constructor(public readonly environment: EnvironmentLevel) {}
}

export class EnvironmentVariableNode {
    constructor(public readonly variable: DebugVariable) {}
}

export class EnvironmentMessageNode {
    constructor(public readonly text: string) {}
}

export function getEnvironmentSettings(): EnvironmentSettings {
    const cfg = workspace.getConfiguration('eta.debug.environment');
    return {
        followActiveFrame: cfg.get<boolean>('followActiveFrame', true),
        showLocals: cfg.get<boolean>('showLocals', true),
        showClosures: cfg.get<boolean>('showClosures', true),
        showGlobals: cfg.get<boolean>('showGlobals', false),
        showBuiltins: cfg.get<boolean>('showBuiltins', false),
        showInternal: cfg.get<boolean>('showInternal', false),
        showNil: cfg.get<boolean>('showNil', false),
        showChangedOnly: cfg.get<boolean>('showChangedOnly', false),
    };
}

export function resolveEnvironmentSelection(
    session: DebugSession,
    followActiveFrame: boolean,
    stackItem: StackItemLike = debug.activeStackItem as StackItemLike,
): EnvironmentSelection {
    if (!followActiveFrame) {
        return { threadId: 1, frameIndex: 0 };
    }
    if (!stackItem || stackItem.session?.id !== session.id) {
        return { threadId: 1, frameIndex: 0 };
    }
    const threadId = Number.isInteger(stackItem.threadId) && (stackItem.threadId ?? 0) > 0
        ? (stackItem.threadId as number)
        : 1;
    const frameIndex = Number.isInteger(stackItem.frameId)
        ? Math.max(0, (stackItem.frameId as number) & 0xFFFF)
        : 0;
    return { threadId, frameIndex };
}

export function buildEnvironmentRequestArgs(session: DebugSession): {
    threadId: number;
    frameIndex: number;
    include: {
        locals: boolean;
        closures: boolean;
        globals: boolean;
        builtins: boolean;
        internal: boolean;
        nil: boolean;
    };
    limits: {
        maxLocals: number;
        maxClosures: number;
        maxGlobals: number;
        maxBuiltins: number;
    };
} {
    const settings = getEnvironmentSettings();
    const sel = resolveEnvironmentSelection(session, settings.followActiveFrame);
    return {
        threadId: sel.threadId,
        frameIndex: sel.frameIndex,
        include: {
            locals: settings.showLocals,
            closures: settings.showClosures,
            globals: settings.showGlobals,
            builtins: settings.showBuiltins,
            internal: settings.showInternal,
            nil: settings.showNil,
        },
        limits: {
            maxLocals: 200,
            maxClosures: 200,
            maxGlobals: 200,
            maxBuiltins: 200,
        },
    };
}

function scopeIcon(scopeKind: string): ThemeIcon {
    switch (scopeKind) {
        case 'locals':
            return new ThemeIcon('bracket', new ThemeColor('symbolIcon.variableForeground'));
        case 'closure':
        case 'closures':
        case 'upvalues':
            return new ThemeIcon('link', new ThemeColor('symbolIcon.referenceForeground'));
        case 'module':
        case 'globals':
            return new ThemeIcon('symbol-module', new ThemeColor('symbolIcon.moduleForeground'));
        case 'builtins':
            return new ThemeIcon('library', new ThemeColor('symbolIcon.functionForeground'));
        default:
            return new ThemeIcon('symbol-namespace', new ThemeColor('symbolIcon.namespaceForeground'));
    }
}

const TYPE_ICON: Record<string, [string, string?]> = {
    procedure: ['symbol-method', 'symbolIcon.functionForeground'],
    builtin: ['zap', 'symbolIcon.functionForeground'],
    continuation: ['debug-step-back', 'symbolIcon.eventForeground'],
    pair: ['list-tree', 'symbolIcon.arrayForeground'],
    vector: ['symbol-array', 'symbolIcon.arrayForeground'],
    hashmap: ['symbol-structure', 'symbolIcon.structForeground'],
    hashset: ['symbol-structure', 'symbolIcon.structForeground'],
    string: ['symbol-string', 'symbolIcon.stringForeground'],
    symbol: ['symbol-key', 'symbolIcon.keyForeground'],
    integer: ['symbol-number', 'symbolIcon.numberForeground'],
    number: ['symbol-number', 'symbolIcon.numberForeground'],
    boolean: ['symbol-boolean', 'symbolIcon.booleanForeground'],
    char: ['symbol-text', 'symbolIcon.stringForeground'],
    port: ['plug', 'symbolIcon.interfaceForeground'],
    tensor: ['graph', 'symbolIcon.numberForeground'],
    nil: ['circle-slash', 'disabledForeground'],
    object: ['symbol-misc', 'symbolIcon.colorForeground'],
};

function sniffType(value: string): string {
    const text = value.trim();
    if (text === '#t' || text === '#f' || text === 'true' || text === 'false') return 'boolean';
    if (text === '()' || text === '#nil' || text === 'nil') return 'nil';
    if (/^-?\d/.test(text)) return 'number';
    if (text.startsWith('"')) return 'string';
    if (text.startsWith('#<tensor')) return 'tensor';
    if (text.startsWith('#<vector')) return 'vector';
    if (text.startsWith('#<hashmap')) return 'hashmap';
    if (text.startsWith('#<hashset')) return 'hashset';
    if (text.startsWith('<procedure') || text.startsWith('#<closure')) return 'procedure';
    if (text.startsWith('(')) return 'pair';
    return 'object';
}

function variableIcon(v: DebugVariable): ThemeIcon {
    const declared = typeof v.type === 'string' ? v.type.trim().toLowerCase() : '';
    const key = declared && TYPE_ICON[declared] ? declared : sniffType(v.value ?? '');
    const [icon, color] = TYPE_ICON[key] ?? TYPE_ICON.object;
    return color ? new ThemeIcon(icon, new ThemeColor(color)) : new ThemeIcon(icon);
}

function variableTooltip(v: DebugVariable): MarkdownString {
    const md = new MarkdownString(undefined, true);
    md.isTrusted = false;
    md.appendMarkdown(`**${v.name}**`);
    md.appendCodeblock(v.value ?? '', 'eta');
    if (v.type) {
        md.appendMarkdown(`\n_type:_ \`${v.type}\``);
    }
    const id = v.objectId ?? undefined;
    if (typeof id === 'number' && Number.isFinite(id) && id > 0) {
        md.appendMarkdown(`\n_id:_ \`#${id}\``);
    }
    return md;
}

export class EnvironmentTreeProvider implements TreeDataProvider<EnvironmentNode> {
    private readonly _onDidChangeTreeData = new EventEmitter<EnvironmentNode | undefined | void>();
    readonly onDidChangeTreeData: Event<EnvironmentNode | undefined | void> = this._onDidChangeTreeData.event;

    private snapshot: EnvironmentSnapshot | undefined;
    private statusText = 'Pause an Eta debug session to inspect environments.';

    applyEnvironment(snapshot: EnvironmentSnapshot | undefined): void {
        this.snapshot = snapshot;
        if (snapshot) {
            this.statusText =
                `Thread ${snapshot.threadId} Frame ${snapshot.frameIndex}: `
                + `${snapshot.frameName || '<anonymous>'}`;
        } else if (debug.activeDebugSession?.type === 'eta') {
            this.statusText = 'No paused Eta frame available.';
        } else {
            this.statusText = 'Pause an Eta debug session to inspect environments.';
        }
        this._onDidChangeTreeData.fire();
    }

    refresh(): void {
        void this.refreshFromSession();
    }

    async refreshFromSession(session?: DebugSession): Promise<void> {
        const activeSession = session ?? debug.activeDebugSession;
        if (!activeSession || activeSession.type !== 'eta') {
            this.applyEnvironment(undefined);
            return;
        }
        try {
            const args = buildEnvironmentRequestArgs(activeSession);
            const snap = await activeSession.customRequest('eta/environment', args) as EnvironmentSnapshot;
            this.applyEnvironment(snap);
        } catch (err: any) {
            const text = err?.message ?? String(err);
            this.snapshot = undefined;
            this.statusText = `Environment unavailable: ${text}`;
            this._onDidChangeTreeData.fire();
        }
    }

    getTreeItem(element: EnvironmentNode): TreeItem {
        if (element instanceof EnvironmentScopeNode) {
            const env = element.environment;
            const item = new TreeItem(env.label, TreeItemCollapsibleState.Collapsed);
            item.iconPath = scopeIcon(env.kind);
            const shown = env.bindings.length;
            item.description = env.truncated ? `${shown}/${env.total}` : `${env.total}`;
            item.tooltip = env.truncated
                ? `${env.label}: showing ${shown} of ${env.total}`
                : `${env.label}: ${env.total}`;
            return item;
        }

        if (element instanceof EnvironmentVariableNode) {
            const v = element.variable;
            const hasChildren = (v.variablesReference ?? 0) > 0;
            const item = new TreeItem(
                v.name,
                hasChildren ? TreeItemCollapsibleState.Collapsed : TreeItemCollapsibleState.None,
            );
            item.iconPath = variableIcon(v);
            const id = v.objectId ?? undefined;
            item.description = typeof id === 'number'
                ? `${v.value}   #${id}`
                : v.value;
            item.tooltip = variableTooltip(v);
            return item;
        }

        const item = new TreeItem(element.text, TreeItemCollapsibleState.None);
        item.iconPath = new ThemeIcon('info');
        return item;
    }

    getChildren(element?: EnvironmentNode): EnvironmentNode[] | Thenable<EnvironmentNode[]> {
        if (!element) {
            return this.rootNodes();
        }
        if (element instanceof EnvironmentScopeNode) {
            return element.environment.bindings.map(v => new EnvironmentVariableNode(v));
        }
        if (element instanceof EnvironmentVariableNode) {
            return this.expandVariable(element.variable);
        }
        return [];
    }

    private rootNodes(): EnvironmentNode[] {
        const snap = this.snapshot;
        if (!snap) {
            return [new EnvironmentMessageNode(this.statusText)];
        }
        const environments = Array.isArray(snap.environments) ? snap.environments : [];
        if (environments.length === 0) {
            return [new EnvironmentMessageNode('No lexical environments enabled by current filters.')];
        }
        return environments.map(env => new EnvironmentScopeNode(env));
    }

    private async expandVariable(variable: DebugVariable): Promise<EnvironmentNode[]> {
        const ref = variable.variablesReference ?? 0;
        if (ref <= 0) {
            return [];
        }
        const session = debug.activeDebugSession;
        if (!session || session.type !== 'eta') {
            return [new EnvironmentMessageNode('No active Eta debug session.')];
        }

        try {
            const resp = await session.customRequest('variables', {
                variablesReference: ref,
                start: 0,
                count: 0,
            }) as { variables?: DebugVariable[] };
            const children = Array.isArray(resp?.variables) ? resp.variables : [];
            return children.map(v => new EnvironmentVariableNode(v));
        } catch (err: any) {
            const text = err?.message ?? String(err);
            return [new EnvironmentMessageNode(`Failed to expand: ${text}`)];
        }
    }
}
