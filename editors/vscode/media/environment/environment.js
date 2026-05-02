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
    };

    const openEnvironments = new Set();
    const expandedRefs = new Set();
    const loadingRefs = new Set();
    const childrenByRef = new Map();
    const expandErrors = new Map();
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

    function renderRow(variable, depth, ancestors) {
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
        html += `<div class="row depth-${rowDepth}" data-ref="${ref}">`;
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
                            html += renderRow(child, depth + 1, nextAncestors);
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
            const key = environmentKey(env);
            if (!openEnvironments.has(key) && env.depth < 2) {
                openEnvironments.add(key);
            }
            const isOpen = openEnvironments.has(key);
            const shown = (env.bindings || []).length;
            const badge = env.truncated ? `${shown}/${env.total}` : `${env.total}`;
            const glyph = isOpen ? 'v' : '>';

            html += '<section class="env">';
            html += `<button class="env-header" data-action="toggle-env" data-env="${escapeHtml(key)}">`;
            html += `<span class="chevron">${glyph}</span>`;
            html += `<span class="env-title">${escapeHtml(env.label)}</span>`;
            html += `<span class="badge" title="${env.truncated ? `showing ${shown} of ${env.total}` : `${env.total}`}">${escapeHtml(badge)}</span>`;
            html += '</button>';
            if (isOpen) {
                html += '<div class="env-body">';
                const bindings = Array.isArray(env.bindings) ? env.bindings : [];
                if (bindings.length === 0) {
                    html += '<div class="hint">No bindings</div>';
                } else {
                    for (const binding of bindings) {
                        html += renderRow(binding, 0, new Set());
                    }
                }
                html += '</div>';
            }
            html += '</section>';
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

        el('expandNearBtn').addEventListener('click', () => {
            openEnvironments.clear();
            if (snapshot && Array.isArray(snapshot.environments)) {
                for (const env of snapshot.environments) {
                    if ((env.depth || 0) < 2) {
                        openEnvironments.add(environmentKey(env));
                    }
                }
            }
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
