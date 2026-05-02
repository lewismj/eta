// Shared DAP protocol types used by heapView.ts and gcRootsTreeView.ts

export interface KindStat {
    kind: string;
    count: number;
    bytes: number;
}

export interface GCRoot {
    name: string;
    objectIds: number[];
    labels?: string[];
    totalCount?: number;
    truncated?: boolean;
}

export interface ConsPoolStats {
    capacity: number;
    live: number;
    free: number;
    bytes: number;
}

export interface HeapSnapshot {
    totalBytes: number;
    softLimit: number;
    kinds: KindStat[];
    roots: GCRoot[];
    consPool?: ConsPoolStats;
    truncated?: boolean;
    scannedObjects?: number;
    kindsTotal?: number;
    kindsShown?: number;
    kindsTruncated?: boolean;
}

export interface ObjectChild {
    objectId: number;
    kind: string;
    size: number;
    preview: string;
}

export interface ObjectInspection {
    objectId: number;
    kind: string;
    size: number;
    preview: string;
    children: ObjectChild[];
}

export interface DebugVariable {
    name: string;
    value: string;
    variablesReference: number;
    indexedVariables?: number;
    namedVariables?: number;
    type?: string;
    objectId?: number;
    canInspectHeap?: boolean;
    canDisassemble?: boolean;
}

export interface EnvironmentLevel {
    kind: string;
    label: string;
    depth: number;
    total: number;
    truncated: boolean;
    bindings: DebugVariable[];
}

export interface EnvironmentSnapshot {
    threadId: number;
    frameIndex: number;
    frameName: string;
    moduleName: string;
    environments: EnvironmentLevel[];
}

export interface ChildProcessInfo {
    pid: number;
    endpoint: string;
    modulePath: string;
    alive: boolean;
}
