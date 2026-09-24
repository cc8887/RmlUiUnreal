<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch, type RmlNode } from '@rmlui/vue';
import { createTable, functionalUpdate, getCoreRowModel, getFilteredRowModel, getSortedRowModel, type ColumnDef, type SortingState } from '@tanstack/table-core';
import { Virtualizer, type VirtualItem } from '@tanstack/virtual-core';
import { native } from '../../../../Frontend/src/bridge';
import { observeLayout } from '../../../../Frontend/src/platform';

interface BuildJob {
  id: number;
  target: string;
  owner: string;
  duration: number;
  status: 'Ready' | 'Running' | 'Queued' | 'Failed';
}

const owners = ['Rendering', 'Gameplay', 'Tools', 'Platform'];
const statuses: BuildJob['status'][] = ['Ready', 'Running', 'Queued', 'Ready', 'Failed'];
const jobs: BuildJob[] = Array.from({ length: 2000 }, (_, index) => ({
  id: index + 1,
  target: `Package-${String(index + 1).padStart(4, '0')}`,
  owner: owners[index % owners.length],
  duration: 18 + ((index * 37) % 283),
  status: statuses[index % statuses.length],
}));
const columns: ColumnDef<BuildJob>[] = [
  { accessorKey: 'id', header: 'ID' },
  { accessorKey: 'target', header: 'Target' },
  { accessorKey: 'owner', header: 'Owner' },
  { accessorKey: 'duration', header: 'Time' },
  { accessorKey: 'status', header: 'Status' },
];

const query = ref('');
const sorting = ref<SortingState>([{ id: 'duration', desc: true }]);
const tableRevision = ref(0);
const scrollNode = ref<RmlNode | null>(null);
const virtualRows = ref<VirtualItem[]>([]);
const totalSize = ref(0);
const table = createTable<BuildJob>({
  data: jobs,
  columns,
  state: { sorting: sorting.value, globalFilter: query.value },
  onStateChange: () => {},
  onSortingChange: updater => {
    sorting.value = functionalUpdate(updater, sorting.value);
    table.setOptions(previous => ({ ...previous, state: { ...previous.state, sorting: sorting.value, globalFilter: query.value } }));
    tableRevision.value++;
  },
  onGlobalFilterChange: updater => {
    query.value = functionalUpdate(updater, query.value);
    table.setOptions(previous => ({ ...previous, state: { ...previous.state, sorting: sorting.value, globalFilter: query.value } }));
    tableRevision.value++;
  },
  renderFallbackValue: '',
  getCoreRowModel: getCoreRowModel(),
  getFilteredRowModel: getFilteredRowModel(),
  getSortedRowModel: getSortedRowModel(),
});
const rows = computed(() => {
  void tableRevision.value;
  return table.getRowModel().rows;
});

const scrollElement = { handle: 0, scrollHeight: jobs.length * 34, scrollWidth: 800, clientHeight: 320, clientWidth: 800 };
let stopVirtualizer = () => {};
const virtualizer = new Virtualizer<any, any>({
  count: jobs.length,
  getScrollElement: () => scrollElement.handle ? scrollElement : null,
  estimateSize: () => 34,
  overscan: 3,
  initialRect: { width: 800, height: 320 },
  scrollToFn: offset => { if (scrollElement.handle) native.ScrollNode(scrollElement.handle, offset); },
  observeElementRect: (_instance, callback) => observeLayout(
    () => scrollElement.handle ? [scrollElement.handle] : [],
    snapshot => {
      const metric = snapshot.nodes[0];
      if (metric) {
        scrollElement.clientWidth = metric.clientWidth; scrollElement.clientHeight = metric.clientHeight;
        scrollElement.scrollWidth = metric.scrollWidth; scrollElement.scrollHeight = metric.scrollHeight;
        callback({ width: metric.clientWidth, height: metric.clientHeight });
      }
    }),
  observeElementOffset: (_instance, callback) => observeLayout(
    () => scrollElement.handle ? [scrollElement.handle] : [],
    snapshot => {
      const metric = snapshot.nodes[0];
      if (metric) callback(metric.scrollTop, false);
    }),
  onChange: instance => {
    virtualRows.value = instance.getVirtualItems();
    totalSize.value = instance.getTotalSize();
  },
});

function refreshVirtualizer(reset = false): void {
  virtualizer.setOptions({ ...virtualizer.options, count: rows.value.length });
  scrollElement.scrollHeight = rows.value.length * 34;
  if (reset && scrollElement.handle) native.ScrollNode(scrollElement.handle, 0);
  virtualizer._willUpdate();
  virtualRows.value = virtualizer.getVirtualItems();
  totalSize.value = virtualizer.getTotalSize();
}
function sortBy(id: string): void {
  const current = sorting.value.find(item => item.id === id);
  sorting.value = [{ id, desc: current?.id === id ? !current.desc : false }];
  table.setOptions(previous => ({ ...previous, state: { ...previous.state, sorting: sorting.value } }));
  tableRevision.value++;
  refreshVirtualizer(true);
}
function sortMark(id: string): string {
  const state = sorting.value.find(item => item.id === id);
  return state ? (state.desc ? 'down' : 'up') : 'none';
}
function jumpTo(index: number): void {
  virtualizer.scrollToIndex(Math.max(0, Math.min(index, rows.value.length - 1)), { align: 'start' });
}

watch(query, value => {
  table.setGlobalFilter(value);
  refreshVirtualizer(true);
});
onMounted(() => {
  scrollElement.handle = scrollNode.value?.handle || 0;
  virtualizer._willUpdate();
  stopVirtualizer = virtualizer._didMount();
  refreshVirtualizer();
});
onBeforeUnmount(() => {
  stopVirtualizer();
  scrollElement.handle = 0;
});
</script>

<template>
  <div id="headless-data-showcase" class="data-page">
      <div class="data-sidebar">
        <div class="data-sidebar-block">
          <strong>SIDEBAR</strong>
          <span>164 x 164</span>
        </div>
      </div>
    <div class="data-heading">
      <div><h2>Headless data lab</h2><p>TanStack algorithms, native RmlUi nodes and host layout metrics.</p></div>
      <div class="package-badges"><span>Table Core 8.21.3</span><span>Virtual Core 3.17.10</span></div>
    </div>
    <div class="data-toolbar">
      <input id="data-filter" v-model.trim="query" class="data-filter" type="text" placeholder="Filter target, owner or status" />
      <button id="data-jump-middle" class="data-command" @click="jumpTo(999)"><span>Jump to 1,000</span></button>
      <button id="data-jump-end" class="data-command" @click="jumpTo(rows.length - 1)"><span>Jump to end</span></button>
    </div>
    <div class="data-summary">
      <span id="data-total">{{ rows.length }} sorted records</span>
      <span id="data-mounted">{{ virtualRows.length }} native rows mounted</span>
      <span>Overscan 3</span>
    </div>
    <div class="data-table">
      <div class="data-header">
        <button id="data-sort-id" @click="sortBy('id')"><span>ID</span><span>{{ sortMark('id') }}</span></button>
        <button id="data-sort-target" @click="sortBy('target')"><span>Target</span><span>{{ sortMark('target') }}</span></button>
        <button id="data-sort-owner" @click="sortBy('owner')"><span>Owner</span><span>{{ sortMark('owner') }}</span></button>
        <button id="data-sort-duration" @click="sortBy('duration')"><span>Time</span><span>{{ sortMark('duration') }}</span></button>
        <button id="data-sort-status" @click="sortBy('status')"><span>Status</span><span>{{ sortMark('status') }}</span></button>
      </div>
      <div id="data-virtual-scroll" ref="scrollNode" class="data-scroll">
        <div id="data-virtual-spacer" class="data-spacer" :style="{ height: `${totalSize}px` }">
          <div v-for="virtualRow in virtualRows" :id="`data-row-${virtualRow.index}`" :key="virtualRow.key" class="data-row" :style="{ top: `${virtualRow.start}px` }">
            <span>{{ rows[virtualRow.index]?.original.id }}</span>
            <strong :id="virtualRow.index === 0 ? 'data-first-target' : undefined">{{ rows[virtualRow.index]?.original.target }}</strong>
            <span>{{ rows[virtualRow.index]?.original.owner }}</span>
            <span>{{ rows[virtualRow.index]?.original.duration }} ms</span>
            <span class="status" :class="`status-${rows[virtualRow.index]?.original.status.toLowerCase()}`">{{ rows[virtualRow.index]?.original.status }}</span>
          </div>
        </div>
      </div>
    </div>
    <div class="data-footnote"><strong>Native platform adapter</strong><span>observeLayout -> rect / scroll offset</span><span>ScrollNode -> scrollToIndex</span><span>Vue renderer -> visible rows only</span></div>
  </div>
</template>

<style>
.data-page { display:flex; flex-direction:column; flex:1; min-height:0; padding:20px; background-color:#eef3f5; color:#24373f; }
.data-heading { display:flex; justify-content:space-between; align-items:center; gap:16px; margin-bottom:14px; }
.data-heading h2 { margin:0; font-size:21px; } .data-heading p { margin:5px 0 0; font-size:11px; color:#5b6c74; }
.package-badges { display:flex; gap:8px; } .package-badges span { padding:6px 9px; border:1px #7ba69d; border-radius:4px; background-color:#e5f3ee; color:#176c5c; font-size:10px; }
.data-toolbar { display:grid; grid-template-columns:minmax(260px,1fr) 150px 130px; gap:8px; margin-bottom:10px; }
.data-filter { height:34px; box-sizing:border-box; padding:6px 10px; border:1px #8ea5af; border-radius:3px; background-color:#e6f4ef; color:#24373f; }
.data-command { display:flex; justify-content:center; align-items:center; border:1px #168c78; border-radius:3px; background-color:#e6f4ef; color:#145b50; }
.data-command:focus,.data-filter:focus { border:2px #d89324; }
.data-summary { display:flex; gap:18px; padding:8px 12px; border-left:4px #158d79; background-color:#ffffff; color:#52646c; font-size:10px; }
.data-summary span:first-child { color:#174f43; font-weight:bold; }
.data-table { display:flex; flex-direction:column; min-height:0; flex:1; margin-top:10px; border:1px #130e0eff; background-color:#e6f4ef; }
.data-header,.data-row { display:grid; grid-template-columns:80px minmax(190px,2fr) minmax(120px,1fr) 100px 110px; align-items:center; }
.data-header { height:38px; border-bottom:1px #829aa5; background-color:#dfe9ec; }
.data-header button { display:flex; height:38px; align-items:center; justify-content:space-between; padding:0 12px; border-width:0 1px 0 0; border-color:#b4c3c9; background-color:transparent; color:#314851; font-size:10px; font-weight:bold; }
.data-header button:focus { background-color:#fff1cf; }
.data-scroll { position:relative; min-height:0; flex:1; overflow:auto; }
.data-spacer { position:relative; width:100%; min-height:1px; }
.data-row { position:absolute; left:0; right:0; height:34px; box-sizing:border-box; border-bottom:1px #e67c12ff; background-color:#ffffff; color:#52636b; font-size:10px; }
.data-row:nth-child(even) { background-color:#f7fafb; }
.data-row span,.data-row strong { padding:0 12px; overflow:hidden; }
.data-row strong { color:#263f49; }
.data-row .status { width:72px; box-sizing:border-box; padding:4px 8px; border-radius:3px; text-align:center; }
.status-ready { background-color:#ddf2e9; color:#126249; } .status-running { background-color:#dcecf8; color:#285f85; }
.status-queued { background-color:#eef0f2; color:#5b6670; } .status-failed { background-color:#f7dfdd; color:#95392e; }
.data-footnote { display:flex; align-items:center; gap:18px; margin-top:10px; padding:8px 12px; background-color:#263d46; color:#dbe8ec; font-size:10px; }
.data-footnote strong { color:#65d0b7; }
</style>
