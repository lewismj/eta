import {
  ColumnDef, SortingState, createSolidTable, flexRender, getCoreRowModel, getSortedRowModel,
} from '@tanstack/solid-table';
import { createVirtualizer } from '@tanstack/solid-virtual';
import { For, Show, createMemo, createSignal } from 'solid-js';
import type { UiNode } from './model';
import { emitHandler, isHandler, str } from './model';
import {
  ColumnSpec, TABLE_ROW_HEIGHT, TableRow, VIRTUALIZE_ABOVE, compareCells, formatCell, parseTable,
} from './tableModel';

/**
 * Sortable, resizable table styled with the theme tokens (TanStack Table
 * draws nothing itself). Click a header to sort; click a row to select it.
 * Large tables render only the visible rows.
 */
export function Table(props: { node: UiNode }) {
  let scroller!: HTMLDivElement;
  const spec = createMemo(() => parseTable(props.node.p as Record<string, unknown>));
  const [sorting, setSorting] = createSignal<SortingState>([]);

  const columns = createMemo<ColumnDef<TableRow>[]>(() =>
    spec().columns.map((c: ColumnSpec) => ({
      id: c.key,
      accessorFn: (row: TableRow) => row[c.key],
      header: c.label,
      size: c.width ?? (c.format ? 110 : 160),
      minSize: 56,
      sortingFn: (a, b, id) => compareCells(a.getValue(id), b.getValue(id)),
      sortUndefined: 'last',
      meta: c,
    })),
  );

  const table = createSolidTable({
    get data() {
      return spec().rows;
    },
    get columns() {
      return columns();
    },
    state: {
      get sorting() {
        return sorting();
      },
    },
    onSortingChange: setSorting,
    getCoreRowModel: getCoreRowModel(),
    getSortedRowModel: getSortedRowModel(),
    columnResizeMode: 'onChange',
    enableColumnResizing: true,
    getRowId: (row) => row.__key,
  });

  const rows = () => table.getRowModel().rows;
  const virtual = () => rows().length > VIRTUALIZE_ABOVE;
  const virtualizer = createVirtualizer({
    get count() {
      return rows().length;
    },
    getScrollElement: () => scroller,
    estimateSize: () => TABLE_ROW_HEIGHT,
    overscan: 12,
  });

  const select = (row: TableRow) => {
    const h = props.node.p['on-select'];
    if (isHandler(h)) emitHandler(h, { index: row.__index, key: row.__key });
  };

  const height = () => (typeof props.node.p.height === 'number' ? `${props.node.p.height}px` : undefined);
  const title = () => str(props.node.p.title);

  const renderRow = (rowIndex: number, offset?: number) => {
    const row = rows()[rowIndex];
    if (!row) return null;
    const selected = () => spec().selected === row.original.__key;
    return (
      <tr
        class={selected() ? 'selected' : undefined}
        aria-selected={selected()}
        data-key={row.original.__key}
        style={offset !== undefined ? { transform: `translateY(${offset}px)` } : undefined}
        onClick={() => select(row.original)}
      >
        <For each={row.getVisibleCells()}>
          {(cell) => {
            const col = cell.column.columnDef.meta as ColumnSpec;
            return (
              <td class={col.align === 'right' ? 'num right' : undefined} style={{ width: `${cell.column.getSize()}px` }}>
                {formatCell(cell.getValue(), col)}
              </td>
            );
          }}
        </For>
      </tr>
    );
  };

  return (
    <section class="table-card" style={{ height: height(), flex: height() ? 'none' : undefined }} data-testid="table">
      <Show when={title()}>
        <header class="plot-head"><span class="plot-title">{title()}</span>
          <span class="plot-subtitle">{spec().rows.length} rows</span></header>
      </Show>
      <div class="table-scroll" ref={scroller}>
        <table style={{ width: `${table.getTotalSize()}px` }}>
          <thead>
            <For each={table.getHeaderGroups()}>
              {(group) => (
                <tr>
                  <For each={group.headers}>
                    {(header) => {
                      const col = header.column.columnDef.meta as ColumnSpec;
                      const sorted = () => header.column.getIsSorted();
                      return (
                        <th
                          class={col.align === 'right' ? 'right' : undefined}
                          style={{ width: `${header.getSize()}px` }}
                          aria-sort={sorted() === 'asc' ? 'ascending' : sorted() === 'desc' ? 'descending' : 'none'}
                        >
                          <button type="button" class="th-sort" onClick={header.column.getToggleSortingHandler()}>
                            {flexRender(header.column.columnDef.header, header.getContext())}
                            <span class="sort-mark" aria-hidden="true">
                              {sorted() === 'asc' ? '▲' : sorted() === 'desc' ? '▼' : ''}
                            </span>
                          </button>
                          <span
                            class={header.column.getIsResizing() ? 'resizer active' : 'resizer'}
                            onMouseDown={header.getResizeHandler()}
                            onTouchStart={header.getResizeHandler()}
                          />
                        </th>
                      );
                    }}
                  </For>
                </tr>
              )}
            </For>
          </thead>
          <Show
            when={virtual()}
            fallback={<tbody><For each={rows()}>{(_, i) => renderRow(i())}</For></tbody>}
          >
            <tbody class="virtual" style={{ height: `${virtualizer.getTotalSize()}px` }}>
              <For each={virtualizer.getVirtualItems()}>{(item) => renderRow(item.index, item.start)}</For>
            </tbody>
          </Show>
        </table>
      </div>
    </section>
  );
}
