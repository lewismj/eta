import type { DebugVariable, EnvironmentLevel, EnvironmentSnapshot } from './dapTypes';

type BindingSignature = {
    value: string;
    type: string;
    objectId: number;
    variablesReference: number;
};

export function environmentBindingKey(
    env: Pick<EnvironmentLevel, 'kind' | 'depth'>,
    variable: Pick<DebugVariable, 'name'>,
): string {
    return `${env.kind}:${env.depth}:${variable.name}`;
}

function bindingSignature(variable: DebugVariable): BindingSignature {
    return {
        value: String(variable.value ?? ''),
        type: String(variable.type ?? ''),
        objectId: Number.isFinite(variable.objectId) ? Number(variable.objectId) : -1,
        variablesReference: Number.isFinite(variable.variablesReference)
            ? Number(variable.variablesReference)
            : 0,
    };
}

function signaturesEqual(a: BindingSignature | undefined, b: BindingSignature): boolean {
    if (!a) return false;
    return a.value === b.value
        && a.type === b.type
        && a.objectId === b.objectId
        && a.variablesReference === b.variablesReference;
}

function buildSignatureIndex(snapshot: EnvironmentSnapshot | undefined): Map<string, BindingSignature> {
    const index = new Map<string, BindingSignature>();
    if (!snapshot || !Array.isArray(snapshot.environments)) {
        return index;
    }
    for (const env of snapshot.environments) {
        const bindings = Array.isArray(env.bindings) ? env.bindings : [];
        for (const variable of bindings) {
            index.set(environmentBindingKey(env, variable), bindingSignature(variable));
        }
    }
    return index;
}

/**
 * Return keys for bindings that changed between consecutive stop snapshots.
 * A first snapshot (no previous baseline) marks all current bindings as changed.
 */
export function computeChangedBindingKeys(
    previous: EnvironmentSnapshot | undefined,
    current: EnvironmentSnapshot | undefined,
): Set<string> {
    const changed = new Set<string>();
    if (!current || !Array.isArray(current.environments)) {
        return changed;
    }

    const previousIndex = buildSignatureIndex(previous);
    const hasPrevious = previousIndex.size > 0;

    for (const env of current.environments) {
        const bindings = Array.isArray(env.bindings) ? env.bindings : [];
        for (const variable of bindings) {
            const key = environmentBindingKey(env, variable);
            if (!hasPrevious) {
                changed.add(key);
                continue;
            }
            const now = bindingSignature(variable);
            if (!signaturesEqual(previousIndex.get(key), now)) {
                changed.add(key);
            }
        }
    }
    return changed;
}
