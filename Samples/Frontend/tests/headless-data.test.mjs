import test from 'node:test';
import assert from 'node:assert/strict';
import { createTable, getCoreRowModel, getFilteredRowModel, getSortedRowModel } from '@tanstack/table-core';
import { Virtualizer } from '@tanstack/virtual-core';

test('TanStack Table and Virtual cores run through a host adapter without DOM globals', () => {
  const data = Array.from({ length: 2000 }, (_, index) => ({ id: index + 1, duration: (index * 37) % 283 }));
  let sorting = [{ id: 'id', desc: true }], globalFilter = '';
  const table = createTable({
    data,
    columns: [{ accessorKey: 'id' }, { accessorKey: 'duration' }],
    state: { sorting, globalFilter },
    onStateChange: () => {},
    onSortingChange: updater => {
      sorting = typeof updater === 'function' ? updater(sorting) : updater;
      table.setOptions(previous => ({ ...previous, state: { ...previous.state, sorting, globalFilter } }));
    },
    onGlobalFilterChange: updater => {
      globalFilter = typeof updater === 'function' ? updater(globalFilter) : updater;
      table.setOptions(previous => ({ ...previous, state: { ...previous.state, sorting, globalFilter } }));
    },
    renderFallbackValue: '',
    getCoreRowModel: getCoreRowModel(),
    getFilteredRowModel: getFilteredRowModel(),
    getSortedRowModel: getSortedRowModel(),
  });
  assert.equal(table.getRowModel().rows[0].original.id, 2000);
  table.getColumn('id').toggleSorting(false);
  assert.equal(table.getRowModel().rows[0].original.id, 1);
  table.setGlobalFilter('1999');
  assert.equal(table.getRowModel().rows.length, 1);
  assert.equal(table.getRowModel().rows[0].original.id, 1999);

  let rectCallback = () => {}, offsetCallback = () => {};
  const scrollWrites = [];
  const hostElement = { handle: 41, scrollHeight: 68000, scrollWidth: 800, clientHeight: 320, clientWidth: 800 };
  const virtualizer = new Virtualizer({
    count: data.length,
    getScrollElement: () => hostElement,
    estimateSize: () => 34,
    overscan: 3,
    initialRect: { width: 800, height: 320 },
    observeElementRect: (_instance, callback) => { rectCallback = callback; return () => {}; },
    observeElementOffset: (_instance, callback) => { offsetCallback = callback; return () => {}; },
    scrollToFn: offset => scrollWrites.push(offset),
  });
  virtualizer._willUpdate();
  const dispose = virtualizer._didMount();
  rectCallback({ width: 800, height: 320 });
  offsetCallback(0, false);
  assert.ok(virtualizer.getVirtualItems().length < 40);
  assert.equal(virtualizer.getVirtualItems()[0].index, 0);
  virtualizer.scrollToIndex(999, { align: 'start' });
  assert.ok(scrollWrites.at(-1) > 30000);
  offsetCallback(scrollWrites.at(-1), false);
  assert.ok(virtualizer.getVirtualItems().some(item => item.index === 999));
  assert.equal(globalThis.document, undefined);
  dispose();
});
