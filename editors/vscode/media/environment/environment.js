(function () {
    'use strict';

    const vscode = acquireVsCodeApi();

    /** @type {any | undefined} */
    let snapshot;
    /** @type {Record<string, boolean>} */
    let settings = {
        followActiveFrame: true,
        showLocals: true,
        showClosures: true,
        showGlobals: false,
        showBuiltins: false,
        showInternal: false,
        showNil: false,
        showChangedOnly: false,
    };

    const openEnvironments = new Set();
    const expandedRefs = new Set();
    const loadingRefs = new Set();
    const childrenByRef = new Map();
    const expandErrors = new Map();
    let changedKeys = new Set();
    let nextRequestId = 1;

    function el(id) {
        return document.getElementById(id);
    }

    function escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = String(text ?? '');
        return div.innerHTML;
    }

    function setError(text) {
        const errorEl = el('error');
        if (!text) {
            errorEl.style.display = 'none';
            errorEl.textContent = '';
            return;
        }
        errorEl.style.display = '';
        errorEl.textContent = text;
    }

    function applySettings() {
        el('followActiveFrame').checked = !!settings.followActiveFrame;
        el('showLocals').checked = !!settings.showLocals;
        el('showClosures').checked = !!settings.showClosures;
        el('showGlobals').checked = !!settings.showGlobals;
        el('showBuiltins').checked = !!settings.showBuiltins;
        el('showInternal').checked = !!settings.showInternal;
        el('showNil').checked = !!settings.showNil;
        el('showChangedOnly').checked = !!settings.showChangedOnly;
    }

    function iconForType(type) {
        const t = (type || '').toLowerCase();
        if (t === 'procedure' || t === 'builtin') return 'f';
        if (t === 'continuation') return 'k';
        if (t === 'pair') return 'l';
        if (t === 'vector') return 'v';
        if (t === 'hashmap' || t === 'hashset') return 'm';
        if (t === 'string') return 's';
        if (t === 'symbol') return '$';
        if (t === 'integer' || t === 'number') return '#';
        if (t === 'boolean') return '?';
        if (t === 'char') return 'c';
        if (t === 'tensor') return 't';
        if (t === 'nil') return '-';
        return 'o';
    }

    function environmentKey(env) {
        return `${env.kind}:${env.depth}`;
    }

    function bindingKey(env, variable) {
        return `${env.kind}:${env.depth}:${variable.name}`;
    }

    function requestExpand(ref) {
        const requestId = `expand-${nextRequestId++}`;
        loadingRefs.add(ref);
        expandErrors.delete(ref);
        vscode.postMessage({
            command: 'expandVariable',
            requestId,
            variablesReference: ref,
        });
    }

    function renderRow(variable, depth, ancestors, isChanged) {
        const hasChildren = (variable.variablesReference || 0) > 0;
        const ref = variable.variablesReference || 0;
        const isExpanded = expandedRefs.has(ref);
        const objectId = Number.isFinite(variable.objectId) ? variable.objectId : undefined;
        const isCycle = typeof objectId === 'number' && ancestors.has(objectId);
        const rowDepth = Math.min(depth, 5);
        const value = isCycle ? `${variable.value} (cycle)` : variable.value;
        const expandGlyph = hasChildren ? (isExpanded ? 'v' : '>') : '';

        const actions = [];
        if (typeof objectId === 'number' && objectId > 0) {
            actions.push(`<button class="icon-btn" data-action="inspect" data-object-id="${objectId}" title="Inspect in Heap Inspector">Heap</button>`);
        }
        if (variable.canDisassemble) {
            actions.push('<button class="icon-btn" data-action="disasm" data-scope="current" title="Show disassembly">Disasm</button>');
        }

        let html = '';
        const changedClass = isChanged ? ' changed' : '';
        html += `<div class="row depth-${rowDepth}${changedClass}" data-ref="${ref}">`;
        if (hasChildren && !isCycle) {
            html += `<button class="expand-btn" data-action="toggle" data-ref="${ref}" title="Expand">${expandGlyph}</button>`;
        } else {
            html += '<span class="expand-btn"></span>';
        }
        html += `<span class="icon">${escapeHtml(iconForType(variable.type))}</span>`;
        html += `<span class="name" title="${escapeHtml(variable.name)}">${escapeHtml(variable.name)}</span>`;
        html += `<span class="value" title="${escapeHtml(value)}">${escapeHtml(value)}</span>`;
        html += `<span class="actions">${actions.join('')}</span>`;
        html += '</div>';

        if (hasChildren && isExpanded && !isCycle) {
            if (loadingRefs.has(ref)) {
                html += `<div class="hint depth-${Math.min(depth + 1, 5)}">Loading...</div>`;
            } else {
                const err = expandErrors.get(ref);
                if (err) {
                    html += `<div class="hint depth-${Math.min(depth + 1, 5)}">${escapeHtml(err)}</div>`;
                } else {
                    const children = childrenByRef.get(ref) || [];
                    if (children.length === 0) {
                        html += `<div class="hint depth-${Math.min(depth + 1, 5)}">No children</div>`;
                    } else {
                        const nextAncestors = new Set(ancestors);
                        if (typeof objectId === 'number') {
                            nextAncestors.add(objectId);
                        }
                        for (const child of children) {
                            html += renderRow(child, depth + 1, nextAncestors, false);
                        }
                    }
                }
            }
        }
        return html;
    }

    function render() {
        const statusEl = el('status');
        const container = el('environments');

        if (!snapshot) {
            container.innerHTML = '';
            return;
        }

        const frameName = snapshot.frameName || '<anonymous>';
        statusEl.textContent = `Thread ${snapshot.threadId} Frame ${snapshot.frameIndex}: ${frameName}`;

        const envs = Array.isArray(snapshot.environments) ? snapshot.environments : [];
        if (envs.length === 0) {
            container.innerHTML = '<div class="hint">No lexical environments available for current filters.</div>';
            return;
        }

        let html = '';
        for (const env of envs) {
            const allBindings = Array.isArray(env.bindings) ? env.bindings : [];
            const changedCount = allBindings.reduce((count, binding) => (
                changedKeys.has(bindingKey(env, binding)) ? count + 1 : count
            ), 0);
            if (settings.showChangedOnly && changedCount === 0) {
                continue;
            }

            const key = environmentKey(env);
            const isOpen = openEnvironments.has(key);
            const shown = allBindings.length;
            const badge = settings.showChangedOnly
                ? `${changedCount}/${env.total}`
                : (env.truncated ? `${shown}/${env.total}` : `${env.total}`);
            const glyph = isOpen ? 'v' : '>';

            html += '<section class="env">';
            html += `<button class="env-header" data-action="toggle-env" data-env="${escapeHtml(key)}">`;
            html += `<span class="chevron">${glyph}</span>`;
            html += `<span class="env-title">${escapeHtml(env.label)}</span>`;
            html += `<span class="badge" title="${env.truncated ? `showing ${shown} of ${env.total}` : `${env.total}`}">${escapeHtml(badge)}</span>`;
            html += '</button>';
            if (isOpen) {
                html += '<div class="env-body">';
                const bindings = settings.showChangedOnly
                    ? allBindings.filter(binding => changedKeys.has(bindingKey(env, binding)))
                    : allBindings;
                if (bindings.length === 0) {
                    html += settings.showChangedOnly
                        ? '<div class="hint">No changed bindings</div>'
                        : '<div class="hint">No bindings</div>';
                } else {
                    for (const binding of bindings) {
                        html += renderRow(binding, 0, new Set(), changedKeys.has(bindingKey(env, binding)));
                    }
                }
                html += '</div>';
            }
            html += '</section>';
        }

        if (html.length === 0) {
            container.innerHTML = settings.showChangedOnly
                ? '<div class="hint">No binding changes since the previous stop.</div>'
                : '<div class="hint">No lexical environments available for current filters.</div>';
            return;
        }

        container.innerHTML = html;
        bindRowActions();
    }

    function bindRowActions() {
        document.querySelectorAll('[data-action="toggle-env"]').forEach((node) => {
            node.addEventListener('click', () => {
                const key = node.getAttribute('data-env');
                if (!key) return;
                if (openEnvironments.has(key)) openEnvironments.delete(key);
                else openEnvironments.add(key);
                render();
            });
        });

        document.querySelectorAll('[data-action="toggle"]').forEach((node) => {
            node.addEventListener('click', () => {
                const ref = Number(node.getAttribute('data-ref'));
                if (!Number.isFinite(ref) || ref <= 0) return;
                if (expandedRefs.has(ref)) {
                    expandedRefs.delete(ref);
                    render();
                    return;
                }
                expandedRefs.add(ref);
                if (!childrenByRef.has(ref) && !loadingRefs.has(ref)) {
                    requestExpand(ref);
                }
                render();
            });
        });

        document.querySelectorAll('[data-action="inspect"]').forEach((node) => {
            node.addEventListener('click', (ev) => {
                ev.stopPropagation();
                const objectId = Number(node.getAttribute('data-object-id'));
                if (!Number.isFinite(objectId) || objectId <= 0) return;
                vscode.postMessage({ command: 'inspectObject', objectId });
            });
        });

        document.querySelectorAll('[data-action="disasm"]').forEach((node) => {
            node.addEventListener('click', (ev) => {
                ev.stopPropagation();
                const scope = node.getAttribute('data-scope') || 'current';
                vscode.postMessage({ command: 'showDisassembly', scope });
            });
        });
    }

    function bindToolbar() {
        el('refreshBtn').addEventListener('click', () => {
            vscode.postMessage({ command: 'refresh' });
        });

        el('collapseBtn').addEventListener('click', () => {
            openEnvironments.clear();
            expandedRefs.clear();
            render();
        });

        const filterKeys = [
            'showLocals',
            'showClosures',
            'showGlobals',
            'showBuiltins',
            'showInternal',
            'showNil',
        ];
        for (const key of filterKeys) {
            el(key).addEventListener('change', (ev) => {
                const value = !!ev.target.checked;
                vscode.postMessage({ command: 'setFilter', key, value });
            });
        }

        el('showChangedOnly').addEventListener('change', (ev) => {
            const value = !!ev.target.checked;
            settings.showChangedOnly = value;
            vscode.postMessage({ command: 'setShowChangedOnly', value });
            render();
        });

        el('followActiveFrame').addEventListener('change', (ev) => {
            vscode.postMessage({
                command: 'setFollowActiveFrame',
                value: !!ev.target.checked,
            });
        });
    }

    window.addEventListener('message', (event) => {
        const msg = event.data;
        switch (msg.command) {
            case 'settings':
                settings = { ...settings, ...(msg.data || {}) };
                applySettings();
                break;
            case 'snapshot':
                snapshot = msg.data || undefined;
                changedKeys = new Set(Array.isArray(msg.changedKeys) ? msg.changedKeys : []);
                setError('');
                render();
                break;
            case 'idle':
                if (!snapshot) {
                    el('status').textContent = msg.text || 'No paused Eta frame available.';
                }
                break;
            case 'error':
                setError(msg.text || 'Request failed.');
                break;
            case 'expanded': {
                const ref = Number(msg.variablesReference);
                if (!Number.isFinite(ref) || ref <= 0) break;
                loadingRefs.delete(ref);
                if (msg.error) {
                    expandErrors.set(ref, msg.error);
                    childrenByRef.delete(ref);
                } else {
                    expandErrors.delete(ref);
                    childrenByRef.set(ref, Array.isArray(msg.variables) ? msg.variables : []);
                }
                render();
                break;
            }
        }
    });

    bindToolbar();
    vscode.postMessage({ command: 'ready' });
})();
