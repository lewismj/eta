// Eta Heap Inspector - webview script.
// Communicates with HeapInspectorPanel (heapView.ts) via postMessage.

(function () {
    'use strict';

    const vscode = acquireVsCodeApi();

    // ─── State ────────────────────────────────────────────────────────────
    /** @type {object|undefined} */ let snapshot;
    /** @type {object|undefined} */ let baseline;
    /** @type {object|undefined} */ let inspected;
    let kindSort = { key: 'bytes', dir: 'desc' };
    let kindFilter = '';
    let diffMode = false;
    const kindLabels = {
        HashMap: 'HashMap',
        HashSet: 'HashSet',
    };

    // ─── Helpers ──────────────────────────────────────────────────────────
    function fmt(bytes) {
        if (bytes < 1024) return bytes + ' B';
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
        return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }
    function asNumber(v) {
        const n = Number(v);
        return Number.isFinite(n) ? n : 0;
    }
    function asPct(part, whole) {
        if (whole <= 0) return 0;
        return Math.max(0, Math.min(100, (part / whole) * 100));
    }
    function visibleFillPct(pct) {
        if (pct <= 0) return 0;
        return Math.max(2.5, Math.min(100, pct));
    }
    function gaugeColor(cls) {
        if (cls === 'crit') return '#f85149';
        if (cls === 'warn') return '#d29922';
        return '#2f81f7';
    }
    function gaugeSvg(pct, cls) {
        const fill = Math.max(0, Math.min(100, pct));
        const kind = cls && cls.length > 0 ? (' ' + cls) : '';
        const color = gaugeColor(cls);
        return ''
            + '<svg class="gauge-svg" viewBox="0 0 100 10" preserveAspectRatio="none" aria-hidden="true">'
            + '  <rect class="gauge-track" x="0" y="0" width="100" height="10" rx="0" ry="0"></rect>'
            + '  <rect class="gauge-fill' + kind + '" x="0" y="0" width="' + fill.toFixed(2) + '" height="10" rx="0" ry="0" fill="' + color + '"></rect>'
            + '</svg>';
    }
    function fmtSigned(n, formatter) {
        const abs = Math.abs(n);
        const sign = n >= 0 ? '+' : '-';
        const cls = n > 0 ? 'delta-pos' : n < 0 ? 'delta-neg' : 'delta-zero';
        const value = formatter ? formatter(abs) : abs.toLocaleString();
        return '<span class="' + cls + '">' + sign + value + '</span>';
    }
    function esc(s) {
        const d = document.createElement('div');
        d.textContent = String(s == null ? '' : s);
        return d.innerHTML;
    }
    function kindLabel(kind) {
        return kindLabels[kind] || kind;
    }

    // ─── Header / toolbar wiring ──────────────────────────────────────────
    document.getElementById('refreshBtn').addEventListener('click', () => {
        vscode.postMessage({ command: 'refresh' });
    });
    const baselineBtn = document.getElementById('baselineBtn');
    const diffBtn = document.getElementById('diffBtn');
    const clearBaselineBtn = document.getElementById('clearBaselineBtn');

    baselineBtn.addEventListener('click', () => {
        if (!snapshot) return;
        baseline = JSON.parse(JSON.stringify(snapshot));
        diffMode = true;
        updateToolbarState();
        renderSnapshot();
    });
    diffBtn.addEventListener('click', () => {
        if (!baseline) return;
        diffMode = !diffMode;
        updateToolbarState();
        renderSnapshot();
    });
    clearBaselineBtn.addEventListener('click', () => {
        baseline = undefined;
        diffMode = false;
        updateToolbarState();
        renderSnapshot();
    });

    const filterInput = document.getElementById('kindFilter');
    filterInput.addEventListener('input', () => {
        kindFilter = filterInput.value.trim().toLowerCase();
        renderKindTable();
    });

    function updateToolbarState() {
        diffBtn.disabled = !baseline;
        diffBtn.classList.toggle('active', diffMode && !!baseline);
        clearBaselineBtn.disabled = !baseline;
        baselineBtn.textContent = baseline ? 'Update Baseline' : 'Capture Baseline';
    }

    // ─── Rendering ────────────────────────────────────────────────────────
    function renderSnapshot() {
        if (!snapshot) return;
        const snap = snapshot;
        const placeholder = document.getElementById('placeholder');
        if (placeholder) placeholder.style.display = 'none';

        renderGauges();
        renderKindTable();
        renderRoots();
    }

    function renderGauges() {
        const snap = snapshot;
        const totalBytes = asNumber(snap.totalBytes);
        const softLimit = asNumber(snap.softLimit);
        const pct = asPct(totalBytes, softLimit);
        const fillPct = visibleFillPct(pct);
        const cls = pct >= 90 ? 'crit' : pct >= 70 ? 'warn' : '';
        let baseDelta = '';
        if (diffMode && baseline) {
            const delta = totalBytes - asNumber(baseline.totalBytes);
            baseDelta = ' | d ' + fmtSigned(delta, fmt);
        }

        const safeSoftLimit = Math.max(0, softLimit);
        const freeHeadroom = Math.max(0, safeSoftLimit - totalBytes);
        let html = '<div class="section"><h2>Memory</h2>';
        html += '<div class="gauge-container">';
        html += '  <div class="gauge-bar">' + gaugeSvg(fillPct, cls) + '</div>';
        html += '  <span class="gauge-label">' + pct.toFixed(1) + '%</span>';
        html += '</div>';
        if (safeSoftLimit > 0) {
            html += '<div class="muted gauge-stats">'
                  + fmt(totalBytes) + ' used of soft limit ' + fmt(safeSoftLimit)
                  + ' | Free headroom: ' + fmt(freeHeadroom)
                  + baseDelta
                  + '</div>';
        } else {
            html += '<div class="muted gauge-stats">'
                  + fmt(totalBytes) + ' used'
                  + baseDelta
                  + ' | Soft limit unavailable'
                  + '</div>';
        }
        html += '</div>';

        if (snap.consPool && snap.consPool.capacity > 0) {
            const pool = snap.consPool;
            const poolLive = asNumber(pool.live);
            const poolCapacity = asNumber(pool.capacity);
            const poolFree = asNumber(pool.free);
            const poolBytes = asNumber(pool.bytes);
            const poolPct = asPct(poolLive, poolCapacity);
            const poolFillPct = visibleFillPct(poolPct);
            const poolCls = poolPct >= 90 ? 'crit' : poolPct >= 70 ? 'warn' : '';
            html += '<div class="section"><h2>Cons Pool</h2>';
            html += '<div class="gauge-container">';
            html += '  <div class="gauge-bar">' + gaugeSvg(poolFillPct, poolCls) + '</div>';
            html += '  <span class="gauge-label">' + poolPct.toFixed(1) + '%</span>';
            html += '</div>';
            html += '<div class="muted gauge-stats">'
                  + poolLive.toLocaleString() + ' / ' + poolCapacity.toLocaleString()
                  + ' | Free: ' + poolFree.toLocaleString()
                  + ' | ' + fmt(poolBytes)
                  + '</div></div>';
        }
        document.getElementById('gauges').innerHTML = html;
    }

    function renderKindTable() {
        if (!snapshot) return;
        const cur = new Map(snapshot.kinds.map((k) => [k.kind, k]));
        const base = baseline ? new Map(baseline.kinds.map((k) => [k.kind, k])) : new Map();
        const allKeys = new Set([...cur.keys(), ...base.keys()]);

        let rows = [];
        for (const kind of allKeys) {
            const c = cur.get(kind) || { kind, count: 0, bytes: 0 };
            const b = base.get(kind) || { kind, count: 0, bytes: 0 };
            const status = !cur.has(kind) ? 'removed'
                         : !base.has(kind) && diffMode && baseline ? 'new'
                         : 'same';
            rows.push({
                kind,
                count: c.count,
                bytes: c.bytes,
                dCount: c.count - b.count,
                dBytes: c.bytes - b.bytes,
                status,
            });
        }

        if (kindFilter) {
            rows = rows.filter((r) => r.kind.toLowerCase().includes(kindFilter));
        }

        const dir = kindSort.dir === 'asc' ? 1 : -1;
        rows.sort((a, b) => {
            const k = kindSort.key;
            let av = a[k], bv = b[k];
            if (typeof av === 'string') return av.localeCompare(bv) * dir;
            return (av - bv) * dir;
        });

        let html = '<h2>Object Kinds <span class="muted">(' + rows.length + ')</span></h2>';
        html += '<table>';
        const cols = diffMode && baseline
            ? [['kind', 'Kind'], ['count', 'Count'], ['dCount', 'Δ Count'], ['bytes', 'Bytes'], ['dBytes', 'Δ Bytes']]
            : [['kind', 'Kind'], ['count', 'Count'], ['bytes', 'Bytes']];
        html += '<tr>';
        for (const [key, label] of cols) {
            const sorted = kindSort.key === key ? (' sorted ' + kindSort.dir) : '';
            const align = key === 'kind' ? '' : ' class="num sortable' + sorted + '"';
            const cls = key === 'kind' ? ' class="sortable' + sorted + '"' : '';
            const attr = key === 'kind' ? cls : align;
            html += '<th data-sort="' + key + '"' + attr + '>' + label + '</th>';
        }
        html += '</tr>';
        for (const r of rows) {
            const cls = r.status === 'new' ? ' class="row-new"' : r.status === 'removed' ? ' class="row-removed"' : '';
            html += '<tr' + cls + '>';
            html += '<td>' + esc(kindLabel(r.kind)) + '</td>';
            html += '<td class="num">' + r.count.toLocaleString() + '</td>';
            if (diffMode && baseline) {
                html += '<td class="num">' + fmtSigned(r.dCount) + '</td>';
            }
            html += '<td class="num">' + fmt(r.bytes) + '</td>';
            if (diffMode && baseline) {
                html += '<td class="num">' + fmtSigned(r.dBytes, fmt) + '</td>';
            }
            html += '</tr>';
        }
        html += '</table>';
        document.getElementById('kinds').innerHTML = html;

        document.querySelectorAll('#kinds th[data-sort]').forEach((th) => {
            th.addEventListener('click', () => {
                const key = th.getAttribute('data-sort');
                if (kindSort.key === key) {
                    kindSort.dir = kindSort.dir === 'asc' ? 'desc' : 'asc';
                } else {
                    kindSort.key = key;
                    kindSort.dir = key === 'kind' ? 'asc' : 'desc';
                }
                renderKindTable();
            });
        });
    }

    function renderRoots() {
        const snap = snapshot;
        let html = '<h2>GC Roots</h2><ul class="tree">';
        for (const root of snap.roots) {
            if (root.objectIds.length === 0) continue;
            const displayedCount = root.objectIds.length;
            const totalCount = root.totalCount || displayedCount;

            if (root.name === 'Globals' && root.labels && root.labels.length > 0) {
                html += '<li>';
                html += '<span class="toggle" data-root="Globals">Globals';
                html += ' <span class="badge">' + totalCount + '</span></span>';
                html += '<ul class="children" style="display:none">';

                const groups = {};
                for (let i = 0; i < root.objectIds.length; i++) {
                    const oid = root.objectIds[i];
                    const label = root.labels[i] || ('Object #' + oid);
                    const dotIdx = label.lastIndexOf('.');
                    const mod = dotIdx > 0 ? label.substring(0, dotIdx) : '(top-level)';
                    if (!groups[mod]) groups[mod] = [];
                    groups[mod].push({ oid, label });
                }
                const modNames = Object.keys(groups).sort((a, b) => {
                    if (a === '(top-level)') return 1;
                    if (b === '(top-level)') return -1;
                    return a.localeCompare(b);
                });
                for (const mod of modNames) {
                    const items = groups[mod];
                    html += '<li>';
                    html += '<span class="toggle">' + esc(mod);
                    html += ' <span class="badge">' + items.length + '</span></span>';
                    html += '<ul class="children" style="display:none">';
                    for (const it of items) {
                        const shortName = it.label.includes('.')
                            ? it.label.substring(it.label.lastIndexOf('.') + 1)
                            : it.label;
                        html += '<li><span class="obj-link" data-oid="' + it.oid + '">'
                              + esc(shortName) + ' <span class="badge">#' + it.oid + '</span></span></li>';
                    }
                    html += '</ul></li>';
                }
                if (root.truncated && totalCount > displayedCount) {
                    html += '<li class="muted">Showing first '
                        + displayedCount + ' of ' + totalCount + ' globals.</li>';
                }
                html += '</ul></li>';
                continue;
            }

            html += '<li>';
            html += '<span class="toggle">' + esc(root.name);
            html += ' <span class="badge">' + totalCount + '</span></span>';
            html += '<ul class="children" style="display:none">';
            for (let i = 0; i < root.objectIds.length; i++) {
                const oid = root.objectIds[i];
                const label = (root.labels && root.labels[i]) ? root.labels[i] : ('Object #' + oid);
                html += '<li><span class="obj-link" data-oid="' + oid + '">'
                      + esc(label) + ' <span class="badge">#' + oid + '</span></span></li>';
            }
            if (root.truncated && totalCount > displayedCount) {
                html += '<li class="muted">Showing first ' + displayedCount
                    + ' of ' + totalCount + ' root objects.</li>';
            }
            html += '</ul></li>';
        }
        html += '</ul>';
        document.getElementById('roots').innerHTML = html;

        document.querySelectorAll('.toggle').forEach((el) => {
            el.addEventListener('click', () => {
                el.classList.toggle('open');
                const ul = el.nextElementSibling;
                if (ul) ul.style.display = ul.style.display === 'none' ? '' : 'none';
            });
        });
        document.querySelectorAll('.obj-link').forEach((el) => {
            el.addEventListener('click', () => {
                const oid = parseInt(el.getAttribute('data-oid'), 10);
                vscode.postMessage({ command: 'inspectObject', objectId: oid });
            });
        });
    }

    function renderInspect(obj) {
        inspected = obj;
        let html = '<h3>Object #' + obj.objectId + '</h3>';
        html += '<div class="detail-row"><span class="detail-label">Kind:</span> ' + esc(obj.kind) + '</div>';
        html += '<div class="detail-row"><span class="detail-label">Size:</span> ' + fmt(obj.size) + '</div>';
        html += '<div class="detail-row"><span class="detail-label">Preview:</span> <code>' + esc(obj.preview) + '</code></div>';
        html += '<div class="detail-actions">';
        html += '  <button class="btn secondary" id="findPathsBtn">Find paths to root</button>';
        html += '</div>';
        html += '<div id="paths"></div>';

        if (obj.children && obj.children.length > 0) {
            html += '<h3 style="margin-top:8px;">Children (' + obj.children.length + ')</h3><ul class="tree">';
            for (const c of obj.children) {
                html += '<li><span class="obj-link" data-oid="' + c.objectId + '">';
                html += '#' + c.objectId + ' <span class="badge">' + esc(c.kind) + '</span> ' + esc(c.preview);
                html += '</span></li>';
            }
            html += '</ul>';
        } else {
            html += '<div class="muted" style="margin-top:4px;">No heap children.</div>';
        }

        const detail = document.getElementById('detail');
        detail.innerHTML = html;
        detail.style.display = '';

        detail.querySelectorAll('.obj-link').forEach((el) => {
            el.addEventListener('click', () => {
                const oid = parseInt(el.getAttribute('data-oid'), 10);
                vscode.postMessage({ command: 'inspectObject', objectId: oid });
            });
        });
        document.getElementById('findPathsBtn').addEventListener('click', () => {
            const target = inspected ? inspected.objectId : undefined;
            if (typeof target !== 'number') return;
            document.getElementById('paths').innerHTML =
                '<div class="path-status"><span class="spinner"></span> Searching… (BFS over GC roots)</div>';
            vscode.postMessage({ command: 'findPaths', objectId: target });
        });
    }

    function renderPaths(payload) {
        const el = document.getElementById('paths');
        if (!el) return;
        if (payload.error) {
            el.innerHTML = '<div class="error">' + esc(payload.error) + '</div>';
            return;
        }
        const paths = payload.paths || [];
        if (paths.length === 0) {
            const trunc = payload.truncated ? ' (search exhausted limit of ' + payload.visited + ' nodes)' : '';
            el.innerHTML = '<div class="path-status muted">No retaining path found from any GC root' + trunc + '.</div>';
            return;
        }
        let html = '<h3 style="margin-top:8px;">Paths from GC roots <span class="muted">('
                 + paths.length + (payload.truncated ? '+' : '') + ')</span></h3>';
        for (const p of paths) {
            html += '<ul class="path-list">';
            html += '<li class="muted">root: ' + esc(p.rootName) + '</li>';
            for (let i = 0; i < p.nodes.length; i++) {
                const n = p.nodes[i];
                const arrow = i === 0 ? '' : '<span class="path-arrow">→</span>';
                html += '<li>' + arrow
                      + '<span class="obj-link" data-oid="' + n.objectId + '">'
                      + '#' + n.objectId + ' <span class="badge">' + esc(n.kind) + '</span> '
                      + esc(n.preview || '')
                      + '</span></li>';
            }
            html += '</ul>';
        }
        if (payload.truncated) {
            html += '<div class="path-status muted">Search stopped after visiting '
                  + payload.visited + ' nodes; further paths may exist.</div>';
        }
        el.innerHTML = html;
        el.querySelectorAll('.obj-link').forEach((linkEl) => {
            linkEl.addEventListener('click', () => {
                const oid = parseInt(linkEl.getAttribute('data-oid'), 10);
                vscode.postMessage({ command: 'inspectObject', objectId: oid });
            });
        });
    }

    function renderIdle(text) {
        snapshot = undefined;
        document.getElementById('gauges').innerHTML = '';
        document.getElementById('kinds').innerHTML = '';
        document.getElementById('roots').innerHTML = '';
        const ph = document.getElementById('placeholder');
        if (ph) {
            ph.textContent = text;
            ph.style.display = '';
        }
        const detail = document.getElementById('detail');
        detail.style.display = 'none';
        detail.innerHTML = '';
    }

    // ─── Message dispatch ─────────────────────────────────────────────────
    window.addEventListener('message', (e) => {
        const msg = e.data;
        switch (msg.command) {
            case 'snapshot':
                snapshot = msg.data;
                renderSnapshot();
                break;
            case 'inspectResult':
                renderInspect(msg.data);
                break;
            case 'pathsResult':
                renderPaths(msg.data);
                break;
            case 'idle':
                renderIdle(msg.text || 'Pause the VM to inspect the heap.');
                break;
            case 'error':
                document.getElementById('roots').innerHTML =
                    '<div class="error">' + esc(msg.text) + '</div>';
                break;
        }
    });

    updateToolbarState();
})();

