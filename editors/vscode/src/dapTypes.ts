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
}

export interface LocalMemorySnapshot {
    threadId: number;
    frameIndex: number;
    frameName: string;
    moduleName: string;
    locals: DebugVariable[];
    upvalues: DebugVariable[];
    moduleGlobals: DebugVariable[];
    localsTotal: number;
    upvaluesTotal: number;
    moduleGlobalsTotal: number;
    localsTruncated: boolean;
    upvaluesTruncated: boolean;
    moduleGlobalsTruncated: boolean;
}

export interface ChildProcessInfo {
    pid: number;
    endpoint: string;
    modulePath: string;
    alive: boolean;
}
