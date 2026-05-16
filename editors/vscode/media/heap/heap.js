(function () {
    'use strict';

    const vscode = acquireVsCodeApi();
    const persisted = vscode.getState() || {};

    const KIND_LABELS = {
        HashMap: 'HashMap',
        HashSet: 'HashSet',
    };
    const FILTER_MODES = new Set(['all', 'kinds', 'roots', 'objectIds']);
    const MAX_META_INFLIGHT = 6;

    const refs = {
        refreshBtn: document.getElementById('refreshBtn'),
        emptyRefreshBtn: document.getElementById('emptyRefreshBtn'),
        baselineBtn: document.getElementById('baselineBtn'),
        diffBtn: document.getElementById('diffBtn'),
        clearBaselineBtn: document.getElementById('clearBaselineBtn'),
        baselineState: document.getElementById('baselineState'),
        heapFilter: document.getElementById('heapFilter'),
        filterMode: document.getElementById('filterMode'),
        rootsFilter: document.getElementById('rootsFilter'),
        gaugeCards: document.getElementById('gaugeCards'),
        kindsSummary: document.getElementById('kindsSummary'),
        kindsTable: document.getElementById('kindsTable'),
        rootsSummary: document.getElementById('rootsSummary'),
        rootsTree: document.getElementById('rootsTree'),
        expandRootsBtn: document.getElementById('expandRootsBtn'),
        collapseRootsBtn: document.getElementById('collapseRootsBtn'),
        kindFocus: document.getElementById('kindFocus'),
        kindFocusText: document.getElementById('kindFocusText'),
        clearKindFocusBtn: document.getElementById('clearKindFocusBtn'),
        rootsPanel: document.getElementById('rootsPanel'),
        detailContent: document.getElementById('detailContent'),
        emptyState: document.getElementById('emptyState'),
        emptyTitle: document.getElementById('emptyTitle'),
        emptyBody: document.getElementById('emptyBody'),
        mainContent: document.getElementById('mainContent'),
    };

    /** @type {any | undefined} */
    let snapshot;
    /** @type {any | undefined} */
    let baseline;

    let baselineCapturedAt = typeof persisted.baselineCapturedAt === 'string'
        ? persisted.baselineCapturedAt
        : '';
    let diffMode = !!persisted.diffMode;
    let kindSort = normalizeKindSort(persisted.kindSort);
    let heapFilterText = typeof persisted.heapFilterText === 'string' ? persisted.heapFilterText : '';
    let filterMode = FILTER_MODES.has(persisted.filterMode) ? persisted.filterMode : 'all';
    let rootsFilterText = typeof persisted.rootsFilterText === 'string' ? persisted.rootsFilterText : '';
    let kindFocus = typeof persisted.kindFocus === 'string' ? persisted.kindFocus : '';
    let selectedObjectId = Number.isFinite(persisted.selectedObjectId) ? Number(persisted.selectedObjectId) : undefined;
    let selectedPath = Array.isArray(persisted.selectedPath) ? persisted.selectedPath.map(String) : [];
    let idleMessage = typeof persisted.idleMessage === 'string'
        ? persisted.idleMessage
        : 'Pause the VM (breakpoint or step) to inspect the heap.';
    let uiState = typeof persisted.uiState === 'string' ? persisted.uiState : 'idle';
    let lastScrollTop = Number.isFinite(persisted.scrollTop) ? persisted.scrollTop : 0;
    let didInitializeExpansion = !!persisted.didInitializeExpansion;

    const expandedKeys = new Set(Array.isArray(persisted.expandedKeys) ? persisted.expandedKeys.map(String) : []);
    const inspectCache = new Map();
    const rootsModel = [];
    const defaultPathByObjectId = new Map();
    const pathsByObjectId = new Map();

    const metaCache = new Map();
    const metaFailures = new Set();
    const metaQueue = [];
    const metaPendingByRequest = new Map();
    const metaRequestedIds = new Set();
    let nextMetaRequestId = 1;
    let metaInflight = 0;
    let rootsRenderScheduled = false;

    refs.heapFilter.value = heapFilterText;
    refs.filterMode.value = filterMode;
    refs.rootsFilter.value = rootsFilterText;

    function normalizeKindSort(raw) {
        const key = raw && typeof raw.key === 'string' ? raw.key : 'bytes';
        const dir = raw && raw.dir === 'asc' ? 'asc' : 'desc';
        if (key !== 'kind' && key !== 'count' && key !== 'bytes' && key !== 'dCount' && key !== 'dBytes') {
            return { key: 'bytes', dir: 'desc' };
        }
        return { key, dir };
    }

    function asNumber(value) {
        const n = Number(value);
        return Number.isFinite(n) ? n : 0;
    }

    function fmtInt(value) {
        return asNumber(value).toLocaleString();
    }

    function fmtBytes(bytes) {
        const n = asNumber(bytes);
        if (n < 1024) return n.toFixed(0) + ' B';
        if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
        return (n / (1024 * 1024)).toFixed(2) + ' MB';
    }

    function fmtSigned(value, formatter) {
        const n = asNumber(value);
        const abs = Math.abs(n);
        const cls = n > 0 ? 'delta-pos' : (n < 0 ? 'delta-neg' : 'delta-zero');
        const sign = n > 0 ? '+' : (n < 0 ? '-' : '');
        const text = formatter ? formatter(abs) : abs.toLocaleString();
        return '<span class="' + cls + '">' + (sign || '0') + (sign ? text : '') + '</span>';
    }

    function pct(part, whole) {
        const p = asNumber(part);
        const w = asNumber(whole);
        if (w <= 0) return 0;
        return Math.max(0, Math.min(100, (p / w) * 100));
    }

    function statusForPct(value) {
        if (value >= 95) return { label: 'Critical', cls: 'status-critical' };
        if (value >= 85) return { label: 'Near limit', cls: 'status-near' };
        if (value >= 65) return { label: 'Elevated', cls: 'status-elevated' };
        return { label: 'Healthy', cls: 'status-healthy' };
    }

    function kindLabel(kind) {
        return KIND_LABELS[kind] || String(kind || '');
    }

    function esc(text) {
        const el = document.createElement('div');
        el.textContent = String(text == null ? '' : text);
        return el.innerHTML;
    }

    function escAttr(text) {
        return esc(text).replace(/"/g, '&quot;');
    }

    function normalizeQuery(text) {
        return String(text || '').trim().toLowerCase();
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

    function saveState() {
        vscode.setState({
            baselineCapturedAt,
            diffMode,
            kindSort,
            heapFilterText,
            filterMode,
            rootsFilterText,
            kindFocus,
            selectedObjectId: typeof selectedObjectId === 'number' ? selectedObjectId : undefined,
            selectedPath,
            expandedKeys: Array.from(expandedKeys),
            didInitializeExpansion,
            uiState,
            idleMessage,
            scrollTop: getScrollTop(),
        });
    }

    function setViewState(mode, detailText) {
        uiState = mode;
        if (mode === 'ready' && snapshot) {
            refs.emptyState.style.display = 'none';
            refs.mainContent.hidden = false;
            saveState();
            return;
        }

        refs.mainContent.hidden = true;
        refs.emptyState.style.display = '';

        if (mode === 'loading') {
            refs.emptyTitle.textContent = 'Loading heap snapshot';
            refs.emptyBody.innerHTML = '<span class="spinner"></span> Requesting heap data from debugger...';
            refs.emptyRefreshBtn.disabled = true;
        } else if (mode === 'error') {
            refs.emptyTitle.textContent = 'Unable to load heap snapshot';
            refs.emptyBody.textContent = detailText || 'The debugger did not return heap data.';
            refs.emptyRefreshBtn.disabled = false;
        } else {
            refs.emptyTitle.textContent = 'No heap snapshot available';
            refs.emptyBody.textContent = detailText || idleMessage;
            refs.emptyRefreshBtn.disabled = false;
        }
        saveState();
    }

    function updateBaselineControls() {
        const hasBaseline = !!baseline;
        refs.diffBtn.disabled = !hasBaseline;
        refs.clearBaselineBtn.disabled = !hasBaseline;
        refs.diffBtn.classList.toggle('active', hasBaseline && diffMode);

        if (!hasBaseline) {
            refs.baselineBtn.textContent = 'Capture';
            refs.baselineState.textContent = 'Baseline: none captured. Capture baseline to enable diff.';
            return;
        }

        refs.baselineBtn.textContent = 'Recapture';
        const when = baselineCapturedAt || 'unknown time';
        if (diffMode) {
            refs.baselineState.textContent = 'Diff: comparing current heap to baseline from ' + when + '.';
        } else {
            refs.baselineState.textContent = 'Baseline: captured ' + when + '. Diff is available.';
        }
    }

    function requestRefresh() {
        setViewState('loading');
        vscode.postMessage({ command: 'refresh' });
    }

    function captureBaseline() {
        if (!snapshot) return;
        baseline = JSON.parse(JSON.stringify(snapshot));
        baselineCapturedAt = new Date().toLocaleTimeString();
        diffMode = true;
        updateBaselineControls();
        renderGauges();
        renderKinds();
        saveState();
    }

    function toggleDiff() {
        if (!baseline) return;
        diffMode = !diffMode;
        updateBaselineControls();
        renderGauges();
        renderKinds();
        saveState();
    }

    function clearBaseline() {
        baseline = undefined;
        baselineCapturedAt = '';
        diffMode = false;
        updateBaselineControls();
        renderGauges();
        renderKinds();
        saveState();
    }

    function renderSnapshot() {
        if (!snapshot) return;
        setViewState('ready');
        buildRootsModel(snapshot);
        renderGauges();
        renderKinds();
        renderRoots();
        renderDetail();
        restoreScroll();
        saveState();

        if (typeof selectedObjectId === 'number') {
            requestInspectObject(selectedObjectId);
        }
    }

    function renderGauges() {
        if (!snapshot) {
            refs.gaugeCards.innerHTML = '';
            return;
        }

        const cards = [];

        const totalBytes = asNumber(snapshot.totalBytes);
        const softLimit = asNumber(snapshot.softLimit);
        const memoryPct = pct(totalBytes, softLimit);
        const memoryStatus = statusForPct(memoryPct);
        const freeHeadroom = Math.max(0, softLimit - totalBytes);
        const memoryDelta = diffMode && baseline
            ? totalBytes - asNumber(baseline.totalBytes)
            : undefined;

        cards.push(renderGaugeCard({
            name: 'Memory',
            pct: memoryPct,
            status: memoryStatus,
            line1: softLimit > 0
                ? (fmtBytes(totalBytes) + ' used / ' + fmtBytes(softLimit) + ' soft limit')
                : (fmtBytes(totalBytes) + ' used / soft limit unavailable'),
            line2: softLimit > 0
                ? ('Free headroom: ' + fmtBytes(freeHeadroom))
                : '',
            delta: memoryDelta,
            deltaFormatter: fmtBytes,
        }));

        if (snapshot.consPool && asNumber(snapshot.consPool.capacity) > 0) {
            const pool = snapshot.consPool;
            const poolPct = pct(pool.live, pool.capacity);
            const poolStatus = statusForPct(poolPct);
            const poolDelta = diffMode && baseline && baseline.consPool
                ? asNumber(pool.live) - asNumber(baseline.consPool.live)
                : undefined;

            cards.push(renderGaugeCard({
                name: 'Cons pool',
                pct: poolPct,
                status: poolStatus,
                line1: fmtInt(pool.live) + ' / ' + fmtInt(pool.capacity) + ' cells used',
                line2: 'Free: ' + fmtInt(pool.free) + ' cells | ' + fmtBytes(pool.bytes),
                delta: poolDelta,
                deltaFormatter: fmtInt,
                deltaSuffix: ' cells',
            }));
        }

        refs.gaugeCards.innerHTML = cards.join('');
    }

    function renderGaugeCard(data) {
        const deltaLine = typeof data.delta === 'number'
            ? '<div class="gauge-detail">Delta since baseline: '
                + fmtSigned(data.delta, (value) => {
                    const rendered = data.deltaFormatter(value);
                    return data.deltaSuffix ? (rendered + data.deltaSuffix) : rendered;
                })
                + '</div>'
            : '';
        return ''
            + '<section class="panel gauge-card">'
            + '  <div class="gauge-header">'
            + '    <div class="gauge-name">' + esc(data.name) + '</div>'
            + '    <div class="gauge-meta">'
            + '      <span class="gauge-pct">' + data.pct.toFixed(1) + '%</span>'
            + '      <span class="status-chip ' + data.status.cls + '">' + esc(data.status.label) + '</span>'
            + '    </div>'
            + '  </div>'
            + '  <div class="gauge-detail">' + esc(data.line1) + '</div>'
            + (data.line2 ? ('  <div class="gauge-detail">' + esc(data.line2) + '</div>') : '')
            + deltaLine
            + '  <div class="gauge-track"><div class="gauge-fill ' + data.status.cls + '" style="width:' + Math.max(0, Math.min(100, data.pct)).toFixed(2) + '%"></div></div>'
            + '</section>';
    }

    function buildKindRows() {
        if (!snapshot) return [];
        const currentMap = new Map(snapshot.kinds.map((row) => [row.kind, row]));
        const baselineMap = baseline ? new Map(baseline.kinds.map((row) => [row.kind, row])) : new Map();
        const keys = new Set([...currentMap.keys(), ...baselineMap.keys()]);
        const rows = [];

        for (const key of keys) {
            const cur = currentMap.get(key) || { kind: key, count: 0, bytes: 0 };
            const base = baselineMap.get(key) || { kind: key, count: 0, bytes: 0 };
            const status = !currentMap.has(key) ? 'removed'
                : (!baselineMap.has(key) && diffMode && baseline ? 'new' : 'same');
            rows.push({
                kind: key,
                count: asNumber(cur.count),
                bytes: asNumber(cur.bytes),
                dCount: asNumber(cur.count) - asNumber(base.count),
                dBytes: asNumber(cur.bytes) - asNumber(base.bytes),
                status,
            });
        }
        return rows;
    }

    function activeKindsQuery() {
        if (filterMode !== 'all' && filterMode !== 'kinds') return '';
        return normalizeQuery(heapFilterText);
    }

    function renderKinds() {
        if (!snapshot) {
            refs.kindsSummary.textContent = '';
            refs.kindsTable.innerHTML = '';
            return;
        }

        let rows = buildKindRows();
        const query = activeKindsQuery();
        if (query) {
            rows = rows.filter((row) => kindLabel(row.kind).toLowerCase().includes(query));
        }

        const dir = kindSort.dir === 'asc' ? 1 : -1;
        rows.sort((a, b) => {
            const key = kindSort.key;
            if (key === 'kind') {
                return a.kind.localeCompare(b.kind) * dir;
            }
            const av = asNumber(a[key]);
            const bv = asNumber(b[key]);
            return (av - bv) * dir;
        });

        const totalKinds = snapshot.kinds.length;
        const totalObjects = snapshot.kinds.reduce((sum, item) => sum + asNumber(item.count), 0);
        const totalBytes = snapshot.kinds.reduce((sum, item) => sum + asNumber(item.bytes), 0);
        const filteredSuffix = query ? (' | Filtered: ' + rows.length) : '';
        const truncSuffix = snapshot.kindsTruncated && Number.isFinite(snapshot.kindsTotal)
            ? (' | Showing ' + asNumber(snapshot.kindsShown || totalKinds) + ' of ' + asNumber(snapshot.kindsTotal) + ' kinds')
            : '';
        refs.kindsSummary.textContent =
            'Object kinds: ' + fmtInt(totalKinds)
            + ' | Objects: ' + fmtInt(totalObjects)
            + ' | Bytes: ' + fmtBytes(totalBytes)
            + filteredSuffix
            + truncSuffix;

        const showDiff = !!(diffMode && baseline);
        const cols = showDiff
            ? [
                { key: 'kind', label: 'Kind' },
                { key: 'count', label: 'Count', num: true },
                { key: 'dCount', label: 'Delta count', num: true },
                { key: 'bytes', label: 'Bytes', num: true },
                { key: 'dBytes', label: 'Delta bytes', num: true },
                { key: 'action', label: 'Action' },
            ]
            : [
                { key: 'kind', label: 'Kind' },
                { key: 'count', label: 'Count', num: true },
                { key: 'bytes', label: 'Bytes', num: true },
                { key: 'action', label: 'Action' },
            ];

        let html = '<table><thead><tr>';
        for (const col of cols) {
            if (col.key === 'action') {
                html += '<th class="num">Action</th>';
                continue;
            }
            const sortClass = kindSort.key === col.key
                ? (' sortable sorted-' + (kindSort.dir === 'asc' ? 'asc' : 'desc'))
                : ' sortable';
            const numClass = col.num ? ' num' : '';
            html += '<th class="' + sortClass + numClass + '" data-sort="' + col.key + '" title="Sort by ' + escAttr(col.label.toLowerCase()) + '">' + esc(col.label) + '</th>';
        }
        html += '</tr></thead><tbody>';

        for (const row of rows) {
            const rowClass = row.status === 'new'
                ? 'row-new'
                : (row.status === 'removed' ? 'row-removed' : '');
            html += '<tr class="' + rowClass + '">';
            html += '<td class="kind-name">' + esc(kindLabel(row.kind)) + '</td>';
            html += '<td class="num">' + fmtInt(row.count) + '</td>';
            if (showDiff) {
                html += '<td class="num">' + fmtSigned(row.dCount, fmtInt) + '</td>';
            }
            html += '<td class="num">' + fmtBytes(row.bytes) + '</td>';
            if (showDiff) {
                html += '<td class="num">' + fmtSigned(row.dBytes, fmtBytes) + '</td>';
            }
            html += '<td class="kind-action"><button class="kind-open" data-kind="' + escAttr(row.kind) + '" title="Focus roots to kind ' + escAttr(kindLabel(row.kind)) + '">View objects ></button></td>';
            html += '</tr>';
        }
        html += '</tbody></table>';

        refs.kindsTable.innerHTML = html;
    }

    function buildRootsModel(snap) {
        rootsModel.length = 0;
        defaultPathByObjectId.clear();
        if (!snap || !Array.isArray(snap.roots)) return;

        for (let rootIndex = 0; rootIndex < snap.roots.length; rootIndex++) {
            const root = snap.roots[rootIndex];
            const ids = Array.isArray(root.objectIds) ? root.objectIds : [];
            const labels = Array.isArray(root.labels) ? root.labels : [];
            const displayedCount = ids.length;
            const totalCount = asNumber(root.totalCount || displayedCount);
            const rootNode = {
                type: 'group',
                key: 'root:' + rootIndex + ':' + String(root.name),
                name: String(root.name || 'Root'),
                count: totalCount,
                leafCount: 0,
                hint: root.truncated && totalCount > displayedCount
                    ? ('Showing first ' + fmtInt(displayedCount) + ' of ' + fmtInt(totalCount) + ' roots.')
                    : '',
                children: [],
            };

            if (rootNode.name === 'Globals' && labels.length > 0) {
                const groups = {};
                for (let i = 0; i < ids.length; i++) {
                    const objectId = asNumber(ids[i]);
                    const label = String(labels[i] || ('Object #' + objectId));
                    const dotIndex = label.lastIndexOf('.');
                    const moduleName = dotIndex > 0 ? label.slice(0, dotIndex) : '(top-level)';
                    const shortName = dotIndex > 0 ? label.slice(dotIndex + 1) : label;
                    if (!groups[moduleName]) groups[moduleName] = [];
                    groups[moduleName].push({ objectId, label, shortName });
                }

                const modules = Object.keys(groups).sort((a, b) => {
                    if (a === '(top-level)') return 1;
                    if (b === '(top-level)') return -1;
                    return a.localeCompare(b);
                });

                modules.forEach((moduleName, moduleIndex) => {
                    const moduleNode = {
                        type: 'group',
                        key: rootNode.key + ':module:' + moduleIndex + ':' + moduleName,
                        name: moduleName,
                        count: groups[moduleName].length,
                        leafCount: groups[moduleName].length,
                        hint: '',
                        children: [],
                    };
                    groups[moduleName].forEach((item, itemIndex) => {
                        const pathSegments = moduleName === '(top-level)'
                            ? [rootNode.name, item.shortName, '#' + item.objectId]
                            : [rootNode.name, moduleName, item.shortName, '#' + item.objectId];
                        const objectNode = {
                            type: 'object',
                            key: moduleNode.key + ':object:' + itemIndex + ':' + item.objectId,
                            objectId: item.objectId,
                            label: item.shortName,
                            fullLabel: item.label,
                            pathSegments,
                            pathText: pathSegments.join(' > '),
                        };
                        if (!defaultPathByObjectId.has(item.objectId)) {
                            defaultPathByObjectId.set(item.objectId, pathSegments.slice());
                        }
                        moduleNode.children.push(objectNode);
                    });
                    rootNode.children.push(moduleNode);
                });
            } else {
                for (let i = 0; i < ids.length; i++) {
                    const objectId = asNumber(ids[i]);
                    const label = String(labels[i] || ('Object #' + objectId));
                    const pathSegments = [rootNode.name, label, '#' + objectId];
                    const objectNode = {
                        type: 'object',
                        key: rootNode.key + ':object:' + i + ':' + objectId,
                        objectId,
                        label,
                        fullLabel: label,
                        pathSegments,
                        pathText: pathSegments.join(' > '),
                    };
                    if (!defaultPathByObjectId.has(objectId)) {
                        defaultPathByObjectId.set(objectId, pathSegments.slice());
                    }
                    rootNode.children.push(objectNode);
                }
            }

            rootNode.leafCount = countLeafNodes(rootNode);
            rootsModel.push(rootNode);
        }

        if (!didInitializeExpansion) {
            rootsModel.forEach((group) => expandedKeys.add(group.key));
            didInitializeExpansion = true;
        }

        if (typeof selectedObjectId === 'number' && (!selectedPath || selectedPath.length === 0)) {
            const fallbackPath = defaultPathByObjectId.get(selectedObjectId);
            if (fallbackPath) {
                selectedPath = fallbackPath.slice();
            }
        }
    }

    function countLeafNodes(groupNode) {
        let count = 0;
        for (const child of groupNode.children) {
            if (child.type === 'object') {
                count++;
            } else {
                child.leafCount = countLeafNodes(child);
                count += child.leafCount;
            }
        }
        return count;
    }

    function activeRootsFilterQuery() {
        return normalizeQuery(rootsFilterText);
    }

    function activeHeapRootsQuery() {
        if (filterMode === 'kinds') return '';
        return normalizeQuery(heapFilterText);
    }

    function rootFiltersActive() {
        return !!kindFocus || !!activeRootsFilterQuery() || !!activeHeapRootsQuery();
    }

    function objectMatchesFilters(node) {
        const rootQuery = activeRootsFilterQuery();
        const heapQuery = activeHeapRootsQuery();
        const meta = metaCache.get(node.objectId);

        if (kindFocus) {
            if (meta && meta.kind !== kindFocus) {
                return false;
            }
            if (!meta && !metaFailures.has(node.objectId)) {
                queueMetaRequest(node.objectId);
            }
        }

        if (rootQuery && !objectMatchesText(node, rootQuery, true)) {
            return false;
        }

        if (!heapQuery) {
            return true;
        }

        if (filterMode === 'objectIds') {
            const idText = String(node.objectId);
            const compact = heapQuery.startsWith('#') ? heapQuery.slice(1) : heapQuery;
            return idText.includes(compact) || ('#' + idText).includes(heapQuery);
        }

        return objectMatchesText(node, heapQuery, true);
    }

    function objectMatchesText(node, query, includeMeta) {
        const values = [
            node.label,
            node.fullLabel,
            node.pathText,
            '#' + node.objectId,
            String(node.objectId),
        ];
        if (includeMeta) {
            const meta = metaCache.get(node.objectId);
            if (meta) {
                values.push(meta.kind || '');
                values.push(meta.preview || '');
                values.push(String(meta.size || ''));
            }
        }
        return values.some((value) => String(value || '').toLowerCase().includes(query));
    }

    function renderRoots() {
        if (!snapshot) {
            refs.rootsSummary.textContent = '';
            refs.rootsTree.innerHTML = '';
            return;
        }

        let html = '';
        let shownObjects = 0;
        const filtersActive = rootFiltersActive();
        for (const group of rootsModel) {
            const rendered = renderGroupNode(group, 0, filtersActive);
            if (!rendered.visible) continue;
            html += rendered.html;
            shownObjects += rendered.objectCount;
        }

        if (!html) {
            refs.rootsTree.innerHTML = '<div class="empty-roots">No roots match the active filters.</div>';
        } else {
            refs.rootsTree.innerHTML = html;
        }

        const totalRoots = rootsModel.reduce((sum, group) => sum + asNumber(group.count), 0);
        refs.rootsSummary.textContent = filtersActive
            ? (fmtInt(shownObjects) + ' shown of ' + fmtInt(totalRoots) + ' roots')
            : (fmtInt(totalRoots) + ' roots');
    }

    function renderGroupNode(groupNode, level, forceOpenForFilter) {
        const forcedOpen = !!forceOpenForFilter;
        const isOpen = forcedOpen || expandedKeys.has(groupNode.key);
        const shouldTraverseChildren = isOpen || forcedOpen;

        let childrenHtml = '';
        let objectCount = 0;
        let hasVisibleChildren = false;

        if (shouldTraverseChildren) {
            for (const child of groupNode.children) {
                if (child.type === 'group') {
                    const renderedChild = renderGroupNode(child, level + 1, forceOpenForFilter);
                    if (!renderedChild.visible) continue;
                    hasVisibleChildren = true;
                    objectCount += renderedChild.objectCount;
                    childrenHtml += renderedChild.html;
                } else if (objectMatchesFilters(child)) {
                    hasVisibleChildren = true;
                    objectCount++;
                    childrenHtml += renderObjectNode(child, level + 1);
                }
            }
        } else {
            hasVisibleChildren = true;
            objectCount = asNumber(groupNode.leafCount);
        }

        if (!hasVisibleChildren) {
            return { visible: false, html: '', objectCount: 0 };
        }

        let html = ''
            + '<button class="tree-row tree-group ' + (isOpen ? 'open' : '') + '"'
            + ' data-toggle-key="' + escAttr(groupNode.key) + '"'
            + ' style="--level:' + level + ';">'
            + '  <span class="cell-name"><span class="chevron">' + (isOpen ? '&#x25BE;' : '&#x25B8;') + '</span><span class="group-label">' + esc(groupNode.name) + '</span><span class="group-count">' + fmtInt(groupNode.count) + '</span></span>'
            + '  <span class="cell-object"></span>'
            + '  <span class="cell-kind"></span>'
            + '  <span class="cell-size"></span>'
            + '  <span class="cell-actions"></span>'
            + '</button>';

        if (isOpen) {
            html += '<div class="tree-children">' + childrenHtml;
            if (groupNode.hint) {
                html += '<div class="root-hint">' + esc(groupNode.hint) + '</div>';
            }
            html += '</div>';
        }

        return { visible: true, html, objectCount };
    }

    function renderObjectNode(node, level) {
        const meta = metaCache.get(node.objectId);
        if (!meta && !metaFailures.has(node.objectId)) {
            queueMetaRequest(node.objectId);
        }
        const kindText = meta ? esc(meta.kind) : '<span class="muted">...</span>';
        const sizeText = meta ? fmtBytes(meta.size) : '<span class="muted">-</span>';
        const selectedClass = selectedObjectId === node.objectId ? ' selected' : '';
        const pathText = node.pathText;
        const filterKindDisabled = !meta || !meta.kind ? ' disabled' : '';
        return ''
            + '<div class="tree-row tree-object' + selectedClass + '"'
            + ' data-object-id="' + node.objectId + '"'
            + ' data-path="' + escAttr(pathText) + '"'
            + ' style="--level:' + level + ';">'
            + '  <span class="cell-name">' + esc(node.label) + '</span>'
            + '  <span class="cell-object"><button class="id-pill" data-action="inspect" data-object-id="' + node.objectId + '" title="Inspect object #' + node.objectId + '">#' + node.objectId + ' &#x2197;</button></span>'
            + '  <span class="cell-kind">' + kindText + '</span>'
            + '  <span class="cell-size">' + sizeText + '</span>'
            + '  <span class="cell-actions">'
            + '    <button class="action-btn" data-action="inspect" data-object-id="' + node.objectId + '" title="Inspect object">Open</button>'
            + '    <button class="action-btn" data-action="copy-id" data-object-id="' + node.objectId + '" title="Copy object id">ID</button>'
            + '    <button class="action-btn" data-action="copy-path" data-path="' + escAttr(pathText) + '" title="Copy root path">Path</button>'
            + '    <button class="action-btn" data-action="filter-kind" data-kind="' + escAttr(meta && meta.kind ? meta.kind : '') + '"' + filterKindDisabled + ' title="Filter to this kind">Kind</button>'
            + '  </span>'
            + '</div>';
    }

    function setKindFocus(kind) {
        kindFocus = String(kind || '');
        if (!kindFocus) {
            updateKindFocusChip();
            saveState();
            renderRoots();
            return;
        }

        filterMode = 'roots';
        refs.filterMode.value = filterMode;
        expandAllRootGroups();
        updateKindFocusChip();
        renderRoots();
        if (refs.rootsPanel && typeof refs.rootsPanel.scrollIntoView === 'function') {
            refs.rootsPanel.scrollIntoView({ block: 'start' });
        }
        saveState();
    }

    function clearKindFocus() {
        kindFocus = '';
        updateKindFocusChip();
        renderRoots();
        saveState();
    }

    function updateKindFocusChip() {
        if (kindFocus) {
            refs.kindFocus.hidden = false;
            refs.kindFocusText.textContent = 'Kind focus: ' + kindLabel(kindFocus);
        } else {
            refs.kindFocus.hidden = true;
            refs.kindFocusText.textContent = '';
        }
    }

    function renderDetail() {
        if (typeof selectedObjectId !== 'number') {
            refs.detailContent.innerHTML = '<div class="detail-placeholder">Select an object id from GC roots to inspect details.</div>';
            return;
        }

        const obj = inspectCache.get(selectedObjectId);
        if (!obj) {
            refs.detailContent.innerHTML =
                '<div class="detail-placeholder"><span class="spinner"></span> Loading object #' + selectedObjectId + '...</div>';
            return;
        }

        const path = selectedPath && selectedPath.length > 0
            ? selectedPath
            : (defaultPathByObjectId.get(selectedObjectId) || ['#' + selectedObjectId]);
        const pathsState = pathsByObjectId.get(selectedObjectId);

        let html = ''
            + '<div class="detail-breadcrumb">' + esc(path.join(' > ')) + '</div>'
            + '<div class="detail-title">Object #' + obj.objectId + '</div>'
            + '<div class="detail-grid">'
            + '  <span class="label">Kind</span><span>' + esc(obj.kind) + '</span>'
            + '  <span class="label">Size</span><span>' + fmtBytes(obj.size) + '</span>';

        if (obj.nativeTypeName) {
            html += '  <span class="label">Native type</span><span>' + esc(obj.nativeTypeName) + '</span>';
        }
        html += '  <span class="label">Preview</span><span><code>' + esc(obj.preview || '') + '</code></span>';
        if (obj.nativeDisplay) {
            html += '  <span class="label">Native display</span><span><code>' + esc(obj.nativeDisplay) + '</code></span>';
        }
        html += '</div>';

        html += ''
            + '<div class="detail-actions">'
            + '  <button class="btn secondary" data-action="find-paths" data-object-id="' + obj.objectId + '">Find paths to root</button>'
            + '  <button class="btn tertiary" data-action="copy-id" data-object-id="' + obj.objectId + '">Copy #' + obj.objectId + '</button>'
            + '</div>';

        html += '<section class="detail-section"><h4>Children (' + fmtInt(Array.isArray(obj.children) ? obj.children.length : 0) + ')</h4>';
        if (Array.isArray(obj.children) && obj.children.length > 0) {
            html += '<div class="child-list">';
            for (const child of obj.children) {
                html += ''
                    + '<div class="child-row">'
                    + '  <button class="obj-link" data-action="inspect" data-object-id="' + child.objectId + '">#' + child.objectId + '</button>'
                    + '  <span class="child-kind">' + esc(child.kind || '') + '</span>'
                    + '  <span class="child-size">' + fmtBytes(child.size) + '</span>'
                    + '  <span class="child-preview">' + esc(child.preview || '') + '</span>'
                    + '</div>';
            }
            html += '</div>';
        } else {
            html += '<div class="muted">No heap children.</div>';
        }
        html += '</section>';

        html += '<section class="detail-section"><h4>Retaining paths</h4>';
        if (!pathsState) {
            html += '<div class="muted">Run "Find paths to root" to inspect retaining chains.</div>';
        } else if (pathsState.status === 'loading') {
            html += '<div class="muted"><span class="spinner"></span> Searching BFS over GC roots...</div>';
        } else if (pathsState.status === 'error') {
            html += '<div class="error">' + esc(pathsState.error || 'Unable to find paths.') + '</div>';
        } else {
            html += renderPaths(pathsState.payload);
        }
        html += '</section>';

        refs.detailContent.innerHTML = html;
    }

    function renderPaths(payload) {
        if (!payload) {
            return '<div class="muted">No path data.</div>';
        }
        if (payload.error) {
            return '<div class="error">' + esc(payload.error) + '</div>';
        }
        const paths = Array.isArray(payload.paths) ? payload.paths : [];
        if (paths.length === 0) {
            const trunc = payload.truncated ? (' Search hit limit of ' + fmtInt(payload.visited) + ' nodes.') : '';
            return '<div class="muted">No retaining path found from GC roots.' + esc(trunc) + '</div>';
        }

        let html = '<ul class="paths-list">';
        for (const path of paths) {
            html += '<li>';
            html += '<div class="path-root">root: ' + esc(path.rootName || '') + '</div>';
            const nodes = Array.isArray(path.nodes) ? path.nodes : [];
            for (let i = 0; i < nodes.length; i++) {
                const node = nodes[i];
                if (i > 0) {
                    html += '<span class="path-arrow">-></span>';
                }
                html += '<span class="path-node"><button class="obj-link" data-action="inspect" data-object-id="' + node.objectId + '">#' + node.objectId + '</button></span>';
            }
            html += '</li>';
        }
        html += '</ul>';
        if (payload.truncated) {
            html += '<div class="muted">Search stopped after visiting ' + fmtInt(payload.visited) + ' nodes; more paths may exist.</div>';
        }
        return html;
    }

    function requestInspectObject(objectId) {
        if (!Number.isFinite(objectId)) return;
        vscode.postMessage({ command: 'inspectObject', objectId });
    }

    function queueMetaRequest(objectId) {
        const oid = asNumber(objectId);
        if (!oid) return;
        if (metaCache.has(oid) || metaFailures.has(oid) || metaRequestedIds.has(oid)) return;
        metaRequestedIds.add(oid);
        metaQueue.push(oid);
        pumpMetaQueue();
    }

    function pumpMetaQueue() {
        while (metaInflight < MAX_META_INFLIGHT && metaQueue.length > 0) {
            const objectId = metaQueue.shift();
            const requestId = nextMetaRequestId++;
            metaPendingByRequest.set(requestId, objectId);
            metaInflight++;
            vscode.postMessage({
                command: 'inspectObjectMeta',
                objectId,
                requestId,
            });
        }
    }

    function handleMetaResult(msg) {
        const requestId = asNumber(msg.requestId);
        const objectId = asNumber(msg.objectId || metaPendingByRequest.get(requestId));
        if (requestId) {
            metaPendingByRequest.delete(requestId);
        }
        metaInflight = Math.max(0, metaInflight - 1);

        if (!objectId) {
            pumpMetaQueue();
            return;
        }

        if (msg.error) {
            metaFailures.add(objectId);
        } else if (msg.data) {
            metaCache.set(objectId, {
                objectId,
                kind: String(msg.data.kind || 'unknown'),
                size: asNumber(msg.data.size),
                preview: String(msg.data.preview || ''),
            });
        }
        scheduleRootsRender();
        pumpMetaQueue();
    }

    function scheduleRootsRender() {
        if (rootsRenderScheduled || !snapshot) return;
        rootsRenderScheduled = true;
        requestAnimationFrame(() => {
            rootsRenderScheduled = false;
            renderRoots();
        });
    }

    function gatherGroupKeys(nodes, out) {
        nodes.forEach((node) => {
            if (node.type !== 'group') return;
            out.push(node.key);
            gatherGroupKeys(node.children.filter((child) => child.type === 'group'), out);
        });
    }

    function expandAllRootGroups() {
        const keys = [];
        gatherGroupKeys(rootsModel, keys);
        keys.forEach((key) => expandedKeys.add(key));
        renderRoots();
        saveState();
    }

    function collapseAllRootGroups() {
        expandedKeys.clear();
        renderRoots();
        saveState();
    }

    function toggleRootGroup(key) {
        if (!key) return;
        if (expandedKeys.has(key)) {
            expandedKeys.delete(key);
        } else {
            expandedKeys.add(key);
        }
        renderRoots();
        saveState();
    }

    function selectObject(objectId, pathText) {
        const oid = asNumber(objectId);
        if (!oid) return;
        selectedObjectId = oid;
        if (pathText) {
            selectedPath = String(pathText).split(' > ').map((part) => part.trim()).filter((part) => part.length > 0);
        } else if (defaultPathByObjectId.has(oid)) {
            selectedPath = defaultPathByObjectId.get(oid).slice();
        } else {
            selectedPath = ['#' + oid];
        }
        renderRoots();
        renderDetail();
        requestInspectObject(oid);
        saveState();
    }

    async function copyText(text) {
        const value = String(text || '');
        if (!value) return;
        try {
            await navigator.clipboard.writeText(value);
            return;
        } catch {
            // Fallback for environments where navigator clipboard is unavailable.
        }

        const area = document.createElement('textarea');
        area.value = value;
        area.style.position = 'fixed';
        area.style.opacity = '0';
        area.style.pointerEvents = 'none';
        document.body.appendChild(area);
        area.focus();
        area.select();
        try {
            document.execCommand('copy');
        } finally {
            document.body.removeChild(area);
        }
    }

    function handleKindsClick(event) {
        const target = event.target;
        if (!(target instanceof HTMLElement)) return;

        const sortable = target.closest('[data-sort]');
        if (sortable && refs.kindsTable.contains(sortable)) {
            const key = sortable.getAttribute('data-sort');
            if (!key) return;
            if (kindSort.key === key) {
                kindSort.dir = kindSort.dir === 'asc' ? 'desc' : 'asc';
            } else {
                kindSort.key = key;
                kindSort.dir = key === 'kind' ? 'asc' : 'desc';
            }
            renderKinds();
            saveState();
            return;
        }

        const kindBtn = target.closest('[data-kind]');
        if (kindBtn && refs.kindsTable.contains(kindBtn)) {
            const kind = kindBtn.getAttribute('data-kind');
            if (!kind) return;
            setKindFocus(kind);
        }
    }

    function handleRootsClick(event) {
        const target = event.target;
        if (!(target instanceof HTMLElement)) return;

        const toggle = target.closest('[data-toggle-key]');
        if (toggle && refs.rootsTree.contains(toggle)) {
            const key = toggle.getAttribute('data-toggle-key');
            toggleRootGroup(key);
            return;
        }

        const action = target.closest('[data-action]');
        if (!action || !refs.rootsTree.contains(action)) return;
        const actionName = action.getAttribute('data-action');
        const objectId = asNumber(action.getAttribute('data-object-id'));
        const row = action.closest('[data-object-id]');
        const pathText = action.getAttribute('data-path')
            || (row ? row.getAttribute('data-path') : '');

        if (actionName === 'inspect') {
            if (!objectId) return;
            selectObject(objectId, pathText || undefined);
            return;
        }
        if (actionName === 'copy-id') {
            if (!objectId) return;
            copyText('#' + objectId);
            return;
        }
        if (actionName === 'copy-path') {
            if (!pathText) return;
            copyText(pathText);
            return;
        }
        if (actionName === 'filter-kind') {
            const kind = action.getAttribute('data-kind');
            if (!kind) return;
            setKindFocus(kind);
        }
    }

    function handleDetailClick(event) {
        const target = event.target;
        if (!(target instanceof HTMLElement)) return;

        const action = target.closest('[data-action]');
        if (!action || !refs.detailContent.contains(action)) return;
        const actionName = action.getAttribute('data-action');
        const objectId = asNumber(action.getAttribute('data-object-id'));

        if (actionName === 'inspect') {
            if (!objectId) return;
            selectObject(objectId);
            return;
        }
        if (actionName === 'copy-id') {
            if (!objectId) return;
            copyText('#' + objectId);
            return;
        }
        if (actionName === 'find-paths') {
            if (!objectId) return;
            pathsByObjectId.set(objectId, { status: 'loading' });
            renderDetail();
            vscode.postMessage({ command: 'findPaths', objectId });
        }
    }

    function resetForIdle(text) {
        snapshot = undefined;
        idleMessage = text || idleMessage;
        refs.gaugeCards.innerHTML = '';
        refs.kindsSummary.textContent = '';
        refs.kindsTable.innerHTML = '';
        refs.rootsSummary.textContent = '';
        refs.rootsTree.innerHTML = '';
        setViewState('idle', idleMessage);
        renderDetail();
    }

    function handleError(text) {
        if (snapshot) {
            refs.detailContent.innerHTML = '<div class="error">' + esc(text || 'Unknown error') + '</div>';
            return;
        }
        setViewState('error', text || 'The debugger did not return heap data.');
    }

    refs.refreshBtn.addEventListener('click', requestRefresh);
    refs.emptyRefreshBtn.addEventListener('click', requestRefresh);

    refs.baselineBtn.addEventListener('click', captureBaseline);
    refs.diffBtn.addEventListener('click', toggleDiff);
    refs.clearBaselineBtn.addEventListener('click', clearBaseline);

    refs.heapFilter.addEventListener('input', () => {
        heapFilterText = refs.heapFilter.value;
        renderKinds();
        renderRoots();
        saveState();
    });
    refs.filterMode.addEventListener('change', () => {
        const mode = refs.filterMode.value;
        filterMode = FILTER_MODES.has(mode) ? mode : 'all';
        renderKinds();
        renderRoots();
        saveState();
    });
    refs.rootsFilter.addEventListener('input', () => {
        rootsFilterText = refs.rootsFilter.value;
        renderRoots();
        saveState();
    });

    refs.expandRootsBtn.addEventListener('click', expandAllRootGroups);
    refs.collapseRootsBtn.addEventListener('click', collapseAllRootGroups);
    refs.clearKindFocusBtn.addEventListener('click', clearKindFocus);

    refs.kindsTable.addEventListener('click', handleKindsClick);
    refs.rootsTree.addEventListener('click', handleRootsClick);
    refs.detailContent.addEventListener('click', handleDetailClick);

    window.addEventListener('scroll', () => {
        lastScrollTop = getScrollTop();
        saveState();
    });

    window.addEventListener('message', (event) => {
        const msg = event.data || {};
        switch (msg.command) {
            case 'snapshot':
                snapshot = msg.data;
                renderSnapshot();
                break;
            case 'inspectResult':
                if (!msg.data || !Number.isFinite(msg.data.objectId)) break;
                inspectCache.set(msg.data.objectId, msg.data);
                if (selectedObjectId === msg.data.objectId) {
                    renderDetail();
                }
                break;
            case 'inspectMetaResult':
                handleMetaResult(msg);
                break;
            case 'pathsResult': {
                const objectId = asNumber(msg.objectId || selectedObjectId);
                if (!objectId) break;
                const payload = msg.data || {};
                if (payload.error) {
                    pathsByObjectId.set(objectId, { status: 'error', error: payload.error, payload });
                } else {
                    pathsByObjectId.set(objectId, { status: 'done', payload });
                }
                if (selectedObjectId === objectId) {
                    renderDetail();
                }
                break;
            }
            case 'idle':
                resetForIdle(msg.text || 'Pause the VM (breakpoint or step) to inspect the heap.');
                break;
            case 'error':
                handleError(msg.text || 'Unknown heap inspector error.');
                break;
        }
    });

    updateBaselineControls();
    updateKindFocusChip();
    setViewState(uiState, idleMessage);
    renderDetail();
})();
