(function () {
    'use strict';

    const vscode = acquireVsCodeApi();
    const persisted = vscode.getState() || {};

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

    const openEnvironments = new Set(Array.isArray(persisted.openEnvironments) ? persisted.openEnvironments : []);
    const expandedRefs = new Set(Array.isArray(persisted.expandedRefs) ? persisted.expandedRefs : []);
    const expandedValues = new Set(Array.isArray(persisted.expandedValues) ? persisted.expandedValues : []);
    const loadingRefs = new Set();
    const childrenByRef = new Map();
    const expandErrors = new Map();
    let changedKeys = new Set();
    let idleText = 'Start or pause an Eta debug session to inspect lexical environments.';
    let hideEmpty = persisted.hideEmpty !== undefined ? !!persisted.hideEmpty : true;
    let bindingFilterText = typeof persisted.bindingFilterText === 'string' ? persisted.bindingFilterText : '';
    let nextRequestId = 1;
    let lastScrollTop = Number.isFinite(persisted.scrollTop) ? persisted.scrollTop : 0;

    function el(id) {
        return document.getElementById(id);
    }

    function escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = String(text ?? '');
        return div.innerHTML;
    }

    function saveState() {
        vscode.setState({
            openEnvironments: Array.from(openEnvironments),
            expandedRefs: Array.from(expandedRefs),
            expandedValues: Array.from(expandedValues),
            hideEmpty,
            bindingFilterText,
            scrollTop: getScrollTop(),
        });
    }

    function getScrollTop() {
        const root = document.scrollingElement || document.documentElement;
        return root ? root.scrollTop : 0;
    }

    function restoreScroll() {
        const root = document.scrollingElement || document.documentElement;
        if (!root) return;
        requestAnimationFrame(() => {
            root.scrollTop = Math.max(0, Math.min(lastScrollTop, root.scrollHeight));
        });
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

    function setStatus(text) {
        el('status').textContent = text || '';
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
        el('hideEmpty').checked = hideEmpty;
        el('bindingFilter').value = bindingFilterText;
    }

    function hideEmptyState() {
        el('emptyState').style.display = 'none';
    }

    function showEmptyState(title, body) {
        el('emptyTitle').textContent = title;
        el('emptyBody').textContent = body;
        el('emptyState').style.display = '';
    }

    function showFrameSummary() {
        const frame = parseFrame(snapshot);
        el('frameMain').textContent = frame.main;
        el('frameLocation').textContent = frame.location;
        el('frameSummary').style.display = '';
    }

    function hideFrameSummary() {
        el('frameSummary').style.display = 'none';
    }

    function parseFrame(snap) {
        const raw = String(snap?.frameName || '<anonymous>');
        const inner = raw.startsWith('<') && raw.endsWith('>') ? raw.slice(1, -1) : raw;
        let symbol = raw || '<anonymous>';
        let location = 'Source location unavailable';
        const at = inner.indexOf('@');
        if (at >= 0) {
            const namePart = inner.slice(0, at).trim();
            symbol = namePart ? `<${namePart}>` : '<anonymous>';
            const sourcePart = inner.slice(at + 1).trim();
            if (sourcePart) location = sourcePart;
        }
        return {
            main: `Thread ${snap.threadId} · Frame ${snap.frameIndex} · ${symbol}`,
            location,
        };
    }

    function environmentKey(env) {
        return `${env.kind}:${env.depth}:${env.label}`;
    }

    function bindingKey(env, variable) {
        return `${env.kind}:${env.depth}:${variable.name}`;
    }

    function normalize(text) {
        return String(text || '').toLowerCase();
    }

    function activeQuery() {
        return bindingFilterText.trim().toLowerCase();
    }

    function bindingMatchesQuery(variable, query) {
        if (!query) return true;
        return normalize(variable.name).includes(query) || normalize(variable.value).includes(query);
    }

    function valueNeedsExpansion(value) {
        const text = String(value ?? '');
        return text.length > 96 || text.includes('\n');
    }

    function truncateValue(value) {
        const text = String(value ?? '');
        if (text.includes('\n')) {
            const first = text.split('\n', 1)[0];
            return `${first} …`;
        }
        return text.length > 96 ? `${text.slice(0, 96)}…` : text;
    }

    function formatCount(count) {
        return count === 1 ? '1 binding' : `${count} bindings`;
    }

    function formatKind(variable) {
        const t = normalize(variable.type);
        if (t === 'procedure') return 'fn';
        if (t === 'builtin') return 'bi';
        if (t === 'continuation') return 'k';
        if (t === 'pair') return 'list';
        if (t === 'vector') return 'vec';
        if (t === 'hashmap') return 'map';
        if (t === 'hashset') return 'set';
        if (t === 'string') return 'str';
        if (t === 'symbol') return 'sym';
        if (t === 'integer' || t === 'number') return '#';
        if (t === 'boolean') return 'bool';
        if (t === 'char') return 'ch';
        if (t === 'tensor') return 'ten';
        if (t === 'nil') return 'nil';
        return 'val';
    }

    function variableMatchesRecursive(variable, query, ancestors) {
        if (bindingMatchesQuery(variable, query)) {
            return true;
        }
        const ref = variable.variablesReference || 0;
        if (ref <= 0) {
            return false;
        }
        const children = childrenByRef.get(ref);
        if (!children || children.length === 0) {
            return false;
        }
        const objectId = Number.isFinite(variable.objectId) ? variable.objectId : undefined;
        if (typeof objectId === 'number' && ancestors.has(objectId)) {
            return false;
        }
        const nextAncestors = new Set(ancestors);
        if (typeof objectId === 'number') {
            nextAncestors.add(objectId);
        }
        for (const child of children) {
            if (variableMatchesRecursive(child, query, nextAncestors)) {
                return true;
            }
        }
        return false;
    }

    function collectRefsForExpand(variable, ancestors, refsToLoad) {
        const ref = variable.variablesReference || 0;
        const objectId = Number.isFinite(variable.objectId) ? variable.objectId : undefined;
        if (typeof objectId === 'number' && ancestors.has(objectId)) {
            return;
        }
        const nextAncestors = new Set(ancestors);
        if (typeof objectId === 'number') {
            nextAncestors.add(objectId);
        }
        if (ref > 0) {
            expandedRefs.add(ref);
            if (!childrenByRef.has(ref) && !loadingRefs.has(ref)) {
                refsToLoad.push(ref);
            }
            const loadedChildren = childrenByRef.get(ref) || [];
            for (const child of loadedChildren) {
                collectRefsForExpand(child, nextAncestors, refsToLoad);
            }
        }
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

    function copyText(text) {
        const plain = String(text ?? '');
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(plain).catch(() => {
                fallbackCopy(plain);
            });
            return;
        }
        fallbackCopy(plain);
    }

    function fallbackCopy(text) {
        const input = document.createElement('textarea');
        input.value = text;
        input.style.position = 'fixed';
        input.style.left = '-9999px';
        document.body.appendChild(input);
        input.focus();
        input.select();
        document.execCommand('copy');
        document.body.removeChild(input);
    }

    function renderRow(variable, depth, ancestors, env, path, topLevelChanged) {
        const query = activeQuery();
        if (query && !variableMatchesRecursive(variable, query, ancestors)) {
            return '';
        }

        const hasChildren = (variable.variablesReference || 0) > 0;
        const ref = variable.variablesReference || 0;
        const isExpanded = expandedRefs.has(ref);
        const objectId = Number.isFinite(variable.objectId) ? variable.objectId : undefined;
        const isCycle = typeof objectId === 'number' && ancestors.has(objectId);
        const rowDepth = Math.min(depth, 5);
        const valueRaw = isCycle ? `${variable.value} (cycle)` : String(variable.value ?? '');
        const valueKey = `${path}|${variable.name}|${depth}|${ref}`;
        const canExpandValue = valueNeedsExpansion(valueRaw);
        const isValueExpanded = expandedValues.has(valueKey);
        const displayedValue = canExpandValue && !isValueExpanded ? truncateValue(valueRaw) : valueRaw;
        const disclosure = hasChildren && !isCycle ? (isExpanded ? '▾' : '▸') : '';
        const changedClass = topLevelChanged ? ' changed' : '';

        const actions = [];
        actions.push(`<button class="icon-btn" data-action="copy-name" data-name="${escapeHtml(variable.name)}" title="Copy name">Copy name</button>`);
        actions.push(`<button class="icon-btn" data-action="copy-value" data-value="${escapeHtml(valueRaw)}" title="Copy value">Copy value</button>`);
        if (canExpandValue) {
            actions.push(`<button class="icon-btn" data-action="toggle-value" data-value-key="${escapeHtml(valueKey)}">${isValueExpanded ? 'Collapse' : 'Expand'}</button>`);
        }
        if (typeof objectId === 'number' && objectId > 0) {
            actions.push(`<button class="icon-btn" data-action="inspect" data-object-id="${objectId}" title="Inspect heap object">Heap ↗</button>`);
        }
        if (variable.canDisassemble) {
            actions.push('<button class="icon-btn" data-action="disasm" data-scope="current" title="Show disassembly">Disasm</button>');
        }

        let html = '';
        html += `<div class="row depth-${rowDepth}${changedClass}" data-ref="${ref}">`;
        if (hasChildren && !isCycle) {
            html += `<button class="expand-btn" data-action="toggle" data-ref="${ref}" title="${isExpanded ? 'Collapse' : 'Expand'}">${disclosure}</button>`;
        } else {
            html += '<span class="expand-btn"></span>';
        }
        html += `<span class="kind">${escapeHtml(formatKind(variable))}</span>`;
        html += `<span class="name" title="${escapeHtml(variable.name)}">${escapeHtml(variable.name)}</span>`;
        html += `<span class="value" title="${escapeHtml(valueRaw)}">${escapeHtml(displayedValue)}</span>`;
        html += `<span class="actions">${actions.join('')}</span>`;
        html += '</div>';

        if (canExpandValue && isValueExpanded) {
            html += `<pre class="value-expanded depth-${rowDepth}">${escapeHtml(valueRaw)}</pre>`;
        }

        if (hasChildren && isExpanded && !isCycle) {
            if (loadingRefs.has(ref)) {
                html += `<div class="hint depth-${Math.min(depth + 1, 5)}">Loading...</div>`;
            } else {
                const err = expandErrors.get(ref);
                if (err) {
                    html += `<div class="hint depth-${Math.min(depth + 1, 5)}">${escapeHtml(err)}</div>`;
                } else {
                    const children = childrenByRef.get(ref) || [];
                    const nextAncestors = new Set(ancestors);
                    if (typeof objectId === 'number') {
                        nextAncestors.add(objectId);
                    }
                    const renderedChildren = [];
                    children.forEach((child, index) => {
                        const childHtml = renderRow(
                            child,
                            depth + 1,
                            nextAncestors,
                            env,
                            `${path}/${index}`,
                            false,
                        );
                        if (childHtml) renderedChildren.push(childHtml);
                    });
                    if (renderedChildren.length === 0) {
                        html += `<div class="hint depth-${Math.min(depth + 1, 5)}">No children</div>`;
                    } else {
                        html += renderedChildren.join('');
                    }
                }
            }
        }
        return html;
    }

    function bindRowActions() {
        document.querySelectorAll('[data-action="toggle-env"]').forEach((node) => {
            node.addEventListener('click', () => {
                const key = node.getAttribute('data-env');
                if (!key) return;
                if (openEnvironments.has(key)) openEnvironments.delete(key);
                else openEnvironments.add(key);
                saveState();
                render();
            });
        });

        document.querySelectorAll('[data-action="toggle"]').forEach((node) => {
            node.addEventListener('click', () => {
                const ref = Number(node.getAttribute('data-ref'));
                if (!Number.isFinite(ref) || ref <= 0) return;
                if (expandedRefs.has(ref)) {
                    expandedRefs.delete(ref);
                    saveState();
                    render();
                    return;
                }
                expandedRefs.add(ref);
                if (!childrenByRef.has(ref) && !loadingRefs.has(ref)) {
                    requestExpand(ref);
                }
                saveState();
                render();
            });
        });

        document.querySelectorAll('[data-action="toggle-value"]').forEach((node) => {
            node.addEventListener('click', (ev) => {
                ev.stopPropagation();
                const valueKey = node.getAttribute('data-value-key');
                if (!valueKey) return;
                if (expandedValues.has(valueKey)) expandedValues.delete(valueKey);
                else expandedValues.add(valueKey);
                saveState();
                render();
            });
        });

        document.querySelectorAll('[data-action="copy-name"]').forEach((node) => {
            node.addEventListener('click', (ev) => {
                ev.stopPropagation();
                copyText(node.getAttribute('data-name') || '');
            });
        });

        document.querySelectorAll('[data-action="copy-value"]').forEach((node) => {
            node.addEventListener('click', (ev) => {
                ev.stopPropagation();
                copyText(node.getAttribute('data-value') || '');
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

    function renderIdleState() {
        hideFrameSummary();
        el('environments').innerHTML = '';
        setStatus('');
        const hasSessionHint = /pause the vm|paused eta frame|breakpoint|step/i.test(idleText);
        if (hasSessionHint) {
            showEmptyState(
                'No paused Eta frame',
                'Pause an Eta debug session to inspect lexical environments.',
            );
        } else {
            showEmptyState(
                'No active Eta debug session',
                'Start or pause an Eta debug session to inspect lexical environments.',
            );
        }
        saveState();
    }

    function render() {
        lastScrollTop = getScrollTop();

        if (!snapshot) {
            renderIdleState();
            restoreScroll();
            return;
        }

        hideEmptyState();
        showFrameSummary();
        setStatus('');

        const container = el('environments');
        const envs = Array.isArray(snapshot.environments) ? snapshot.environments : [];
        if (envs.length === 0) {
            container.innerHTML = '<div class="hint">No lexical environments available for current filters.</div>';
            saveState();
            restoreScroll();
            return;
        }

        const query = activeQuery();
        const sections = [];
        for (const env of envs) {
            const allBindings = Array.isArray(env.bindings) ? env.bindings : [];
            const visibleBindings = allBindings
                .filter(binding => !settings.showChangedOnly || changedKeys.has(bindingKey(env, binding)))
                .filter(binding => !query || variableMatchesRecursive(binding, query, new Set()));

            const isEmpty = allBindings.length === 0;
            if (hideEmpty && isEmpty) {
                continue;
            }

            const key = environmentKey(env);
            const isOpen = openEnvironments.has(key);
            const countText = isEmpty
                ? 'empty'
                : (query || settings.showChangedOnly || env.truncated
                    ? `${visibleBindings.length}/${allBindings.length} ${formatCount(visibleBindings.length).split(' ')[1]}`
                    : formatCount(allBindings.length));
            const title = isEmpty ? `${env.label} — empty` : env.label;
            const chevron = isOpen ? '▾' : '▸';

            let sectionHtml = '<section class="env">';
            sectionHtml += `<button class="env-header${isOpen ? ' open' : ''}${isEmpty ? ' empty' : ''}" data-action="toggle-env" data-env="${escapeHtml(key)}">`;
            sectionHtml += `<span class="chevron">${chevron}</span>`;
            sectionHtml += `<span class="env-title">${escapeHtml(title)}</span>`;
            sectionHtml += `<span class="env-meta">${escapeHtml(countText)}</span>`;
            sectionHtml += '</button>';

            if (isOpen && !isEmpty) {
                sectionHtml += '<div class="env-body">';
                sectionHtml += '<div class="binding-columns"><span></span><span>Kind</span><span>Name</span><span>Value</span><span></span></div>';
                if (visibleBindings.length === 0) {
                    sectionHtml += settings.showChangedOnly
                        ? '<div class="hint">No changed bindings</div>'
                        : '<div class="hint">No bindings match current filter</div>';
                } else {
                    visibleBindings.forEach((binding, index) => {
                        sectionHtml += renderRow(
                            binding,
                            0,
                            new Set(),
                            env,
                            `${key}/${index}`,
                            changedKeys.has(bindingKey(env, binding)),
                        );
                    });
                }
                sectionHtml += '</div>';
            }
            sectionHtml += '</section>';
            sections.push(sectionHtml);
        }

        if (sections.length === 0) {
            container.innerHTML = hideEmpty
                ? '<div class="hint">No non-empty scopes available with current options.</div>'
                : '<div class="hint">No lexical environments available for current filters.</div>';
            saveState();
            restoreScroll();
            return;
        }

        container.innerHTML = sections.join('');
        bindRowActions();
        saveState();
        restoreScroll();
    }

    function expandAll() {
        if (!snapshot) return;
        const refsToLoad = [];
        const query = activeQuery();
        const envs = Array.isArray(snapshot.environments) ? snapshot.environments : [];

        for (const env of envs) {
            const allBindings = Array.isArray(env.bindings) ? env.bindings : [];
            if (hideEmpty && allBindings.length === 0) continue;
            const key = environmentKey(env);
            openEnvironments.add(key);

            const visibleBindings = allBindings
                .filter(binding => !settings.showChangedOnly || changedKeys.has(bindingKey(env, binding)))
                .filter(binding => !query || variableMatchesRecursive(binding, query, new Set()));
            visibleBindings.forEach((binding) => collectRefsForExpand(binding, new Set(), refsToLoad));
        }

        refsToLoad.forEach(requestExpand);
        saveState();
        render();
    }

    function collapseAll() {
        openEnvironments.clear();
        expandedRefs.clear();
        expandedValues.clear();
        saveState();
        render();
    }

    function bindToolbar() {
        el('refreshBtn').addEventListener('click', () => {
            vscode.postMessage({ command: 'refresh' });
        });

        el('expandBtn').addEventListener('click', () => {
            expandAll();
        });

        el('collapseBtn').addEventListener('click', () => {
            collapseAll();
        });

        el('bindingFilter').addEventListener('input', (ev) => {
            bindingFilterText = String(ev.target.value || '');
            saveState();
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
            saveState();
            render();
        });

        el('followActiveFrame').addEventListener('change', (ev) => {
            const value = !!ev.target.checked;
            settings.followActiveFrame = value;
            vscode.postMessage({ command: 'setFollowActiveFrame', value });
            saveState();
        });

        el('hideEmpty').addEventListener('change', (ev) => {
            hideEmpty = !!ev.target.checked;
            saveState();
            render();
        });

        el('runFileBtn').addEventListener('click', () => {
            vscode.postMessage({ command: 'runFile' });
        });

        el('debugFileBtn').addEventListener('click', () => {
            vscode.postMessage({ command: 'debugFile' });
        });

        window.addEventListener('scroll', () => {
            lastScrollTop = getScrollTop();
            saveState();
        }, { passive: true });
    }

    window.addEventListener('message', (event) => {
        const msg = event.data;
        switch (msg.command) {
            case 'settings':
                settings = { ...settings, ...(msg.data || {}) };
                applySettings();
                saveState();
                break;
            case 'snapshot':
                snapshot = msg.data || undefined;
                changedKeys = new Set(Array.isArray(msg.changedKeys) ? msg.changedKeys : []);
                setError('');
                render();
                break;
            case 'idle':
                snapshot = undefined;
                changedKeys = new Set();
                idleText = msg.text || 'No paused Eta frame available.';
                setError('');
                render();
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
    applySettings();
    render();
    vscode.postMessage({ command: 'ready' });
})();
