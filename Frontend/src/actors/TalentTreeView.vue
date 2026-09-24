<script setup lang="ts">
import { computed, nextTick, onUnmounted, ref } from '@rmlui/vue';
import { adaptAnimationJs, type RmlAnimationGroup } from '../animation-adapters';

type BranchId = 'consciousness' | 'weapon' | 'body' | 'harmony' | 'infinity';
interface TalentEffect { label: string; value: string }
interface TalentSpec { id: string; branch: BranchId; tier: number; lane: number; name: string; maxRank: number; rank: number; cost: number; summary: string; detail: string; effects: TalentEffect[] }

const STAGE_W = 980;
const STAGE_H = 620;
const CX = 490;
const CY = 306;
const RADIUS_X = [140, 235, 320];
const RADIUS_Y = [118, 196, 262];
const SPREAD = 20;
const TOTAL_POINTS = 13;

const BRANCHES: { id: BranchId; label: string; short: string; angle: number; color: string }[] = [
  { id: 'consciousness', label: 'CONSCIOUSNESS', short: 'con', angle: -90, color: '#4aa3dd' },
  { id: 'weapon', label: 'WEAPON', short: 'wep', angle: -18, color: '#e0a63a' },
  { id: 'body', label: 'BODY', short: 'bod', angle: 54, color: '#1fa088' },
  { id: 'harmony', label: 'HARMONY', short: 'har', angle: 126, color: '#8f6bb8' },
  { id: 'infinity', label: 'INFINITY', short: 'inf', angle: 198, color: '#d4695a' },
];
const PHASES = [
  { id: 'evolve', label: 'EVOLVE', from: 0 },
  { id: 'adapt', label: 'ADAPT', from: 5 },
  { id: 'transcend', label: 'TRANSCEND', from: 10 },
];

function requires(branch: BranchId, tier: number): string[] {
  const short = BRANCHES.find(item => item.id === branch)!.short;
  if (tier === 0) return [];
  if (tier === 3) return [`${short}-1`, `${short}-2`];
  return [`${short}-0`];
}

const talents = ref<TalentSpec[]>([
  { id: 'con-0', branch: 'consciousness', tier: 0, lane: 0, name: 'Insight', maxRank: 3, rank: 2, cost: 1, summary: 'Neural link exposes structural weak points.', detail: 'PLACEHOLDER — 神经链路解析目标结构，每层提升弱点判定窗口。', effects: [{ label: 'Crit window', value: '+4% / rank' }, { label: 'Link drain', value: '-2% / rank' }] },
  { id: 'con-1', branch: 'consciousness', tier: 1, lane: -1, name: 'Clarity', maxRank: 3, rank: 1, cost: 1, summary: 'Thought latency collapses under pressure.', detail: 'PLACEHOLDER — 净化意识噪声，缩短技能冷却。', effects: [{ label: 'Cooldown', value: '-3% / rank' }, { label: 'Focus regen', value: '+1.5 / s' }] },
  { id: 'con-2', branch: 'consciousness', tier: 2, lane: 1, name: 'Focus', maxRank: 3, rank: 0, cost: 1, summary: 'Sustained aim despite recoil and chaos.', detail: 'PLACEHOLDER — 稳定瞄准，降低后坐力影响。', effects: [{ label: 'Spread', value: '-6% / rank' }, { label: 'Stability', value: '+8' }] },
  { id: 'con-3', branch: 'consciousness', tier: 3, lane: 0, name: 'Synthesis', maxRank: 1, rank: 0, cost: 2, summary: 'Mind and machine resolve into one intent.', detail: 'PLACEHOLDER — 意识合流，解锁终极形态。', effects: [{ label: 'All link gains', value: '+25%' }, { label: 'Overload', value: 'Enabled' }] },
  { id: 'wep-0', branch: 'weapon', tier: 0, lane: 0, name: 'Draw Speed', maxRank: 3, rank: 1, cost: 1, summary: 'Weapons come online faster.', detail: 'PLACEHOLDER — 加快武器切换与首发速度。', effects: [{ label: 'Swap time', value: '-8% / rank' }, { label: 'First shot', value: '+5%' }] },
  { id: 'wep-1', branch: 'weapon', tier: 1, lane: -1, name: 'Overcharge', maxRank: 3, rank: 2, cost: 1, summary: 'Dump the cell for a heavier hit.', detail: 'PLACEHOLDER — 过载射击，提升单发伤害与热量。', effects: [{ label: 'Damage', value: '+7% / rank' }, { label: 'Heat', value: '+4% / rank' }] },
  { id: 'wep-2', branch: 'weapon', tier: 2, lane: 1, name: 'Ricochet', maxRank: 2, rank: 0, cost: 1, summary: 'Shots seek a second target.', detail: 'PLACEHOLDER — 弹射，命中后寻找次要目标。', effects: [{ label: 'Bounces', value: '1' }, { label: 'Falloff', value: '-15%' }] },
  { id: 'wep-3', branch: 'weapon', tier: 3, lane: 0, name: 'Annihilation', maxRank: 1, rank: 0, cost: 2, summary: 'Final form of the weapon branch.', detail: 'PLACEHOLDER — 湮灭协议，终极武器节点。', effects: [{ label: 'Burst', value: '+40%' }, { label: 'Cost', value: '2 points' }] },
  { id: 'bod-0', branch: 'body', tier: 0, lane: 0, name: 'Vitality', maxRank: 5, rank: 2, cost: 1, summary: 'Denser tissue, longer fights.', detail: 'PLACEHOLDER — 强化体质，提升生命上限。', effects: [{ label: 'Max health', value: '+30 / rank' }, { label: 'Regen delay', value: '-0.2s' }] },
  { id: 'bod-1', branch: 'body', tier: 1, lane: -1, name: 'Ironhide', maxRank: 3, rank: 1, cost: 1, summary: 'Armor plating grown, not worn.', detail: 'PLACEHOLDER — 生物甲壳，降低受创。', effects: [{ label: 'Damage taken', value: '-4% / rank' }, { label: 'Poise', value: '+10' }] },
  { id: 'bod-2', branch: 'body', tier: 2, lane: 1, name: 'Adrenaline', maxRank: 3, rank: 0, cost: 1, summary: 'Pain becomes throughput.', detail: 'PLACEHOLDER — 低血量时提升移动与攻速。', effects: [{ label: 'Speed', value: '+6% / rank' }, { label: 'Threshold', value: '35%' }] },
  { id: 'bod-3', branch: 'body', tier: 3, lane: 0, name: 'Titan', maxRank: 1, rank: 0, cost: 2, summary: 'The body refuses to yield.', detail: 'PLACEHOLDER — 泰坦形态，终极体质节点。', effects: [{ label: 'Stagger', value: 'Immune' }, { label: 'Health', value: '+15%' }] },
  { id: 'har-0', branch: 'harmony', tier: 0, lane: 0, name: 'Resonance', maxRank: 3, rank: 1, cost: 1, summary: 'Squad abilities echo outward.', detail: 'PLACEHOLDER — 共鸣，扩大增益范围。', effects: [{ label: 'Aura radius', value: '+1m / rank' }, { label: 'Ally bonus', value: '+3%' }] },
  { id: 'har-1', branch: 'harmony', tier: 1, lane: -1, name: 'Aegis', maxRank: 3, rank: 0, cost: 1, summary: 'A shared shield, held together by will.', detail: 'PLACEHOLDER — 群体护盾。', effects: [{ label: 'Shield', value: '+80 / rank' }, { label: 'Uptime', value: '6s' }] },
  { id: 'har-2', branch: 'harmony', tier: 2, lane: 1, name: 'Cadence', maxRank: 3, rank: 1, cost: 1, summary: 'Timing turns a team into a rhythm.', detail: 'PLACEHOLDER — 节奏同步，连锁触发。', effects: [{ label: 'Chain', value: '+1 target' }, { label: 'Window', value: '+0.4s' }] },
  { id: 'har-3', branch: 'harmony', tier: 3, lane: 0, name: 'Equilibrium', maxRank: 1, rank: 0, cost: 2, summary: 'Perfect balance of give and take.', detail: 'PLACEHOLDER — 均衡，终极和谐节点。', effects: [{ label: 'Share damage', value: '35%' }, { label: 'Heal', value: '+20%' }] },
  { id: 'inf-0', branch: 'infinity', tier: 0, lane: 0, name: 'Aperture', maxRank: 3, rank: 1, cost: 1, summary: 'Open a thin seam in local space.', detail: 'PLACEHOLDER — 开启空间裂隙。', effects: [{ label: 'Range', value: '+5% / rank' }, { label: 'Pierce', value: '+1' }] },
  { id: 'inf-1', branch: 'infinity', tier: 1, lane: -1, name: 'Recursion', maxRank: 3, rank: 0, cost: 1, summary: 'Effects remember themselves.', detail: 'PLACEHOLDER — 递归，效果二次触发。', effects: [{ label: 'Repeat', value: '35%' }, { label: 'Decay', value: '-10%' }] },
  { id: 'inf-2', branch: 'infinity', tier: 2, lane: 1, name: 'Singularity', maxRank: 2, rank: 0, cost: 1, summary: 'A small gravity, well behaved.', detail: 'PLACEHOLDER — 奇点，牵引周围目标。', effects: [{ label: 'Pull', value: '3m' }, { label: 'Duration', value: '2s' }] },
  { id: 'inf-3', branch: 'infinity', tier: 3, lane: 0, name: 'Transcendence', maxRank: 1, rank: 0, cost: 2, summary: 'Step outside the measured system.', detail: 'PLACEHOLDER — 超脱，终极无限节点。', effects: [{ label: 'All branches', value: '+10%' }, { label: 'Cooldown', value: '-20%' }] },
]);

const points = ref(TOTAL_POINTS - 12);
const hoveredId = ref('');
const selectedId = ref('con-0');
const focusBranch = ref<BranchId | ''>('');
const zoom = ref(1);
const xp = ref(700);
const xpMax = 2000;
const notice = ref('');
let detailAnimation: RmlAnimationGroup | undefined;
let ringAnimation: RmlAnimationGroup | undefined;
let selectionAnimationRevision = 0;

const nodes = computed(() => talents.value.map(spec => {
  const branch = BRANCHES.find(item => item.id === spec.branch)!;
  const angle = spec.tier === 1 || spec.tier === 2 ? branch.angle + spec.lane * SPREAD : branch.angle;
  const index = spec.tier === 0 ? 0 : spec.tier === 3 ? 2 : 1;
  const radians = angle * Math.PI / 180;
  return { ...spec, x: CX + RADIUS_X[index] * Math.cos(radians), y: CY + RADIUS_Y[index] * Math.sin(radians), color: branch.color, branchLabel: branch.label, requires: requires(spec.branch, spec.tier) };
}));
const byId = computed(() => new Map(nodes.value.map(node => [node.id, node])));
const edges = computed(() => nodes.value.flatMap(node => node.requires.map(parentId => {
  const parent = byId.value.get(parentId)!;
  const dx = node.x - parent.x;
  const dy = node.y - parent.y;
  return { id: `${parentId}--${node.id}`, from: parentId, to: node.id, branch: node.branch, color: node.color,
    x: parent.x, y: parent.y, length: Math.sqrt(dx * dx + dy * dy), angle: Math.atan2(dy, dx) * 180 / Math.PI,
    linked: parent.rank > 0 && node.rank > 0 };
})));
const spent = computed(() => talents.value.reduce((total, spec) => total + spec.rank, 0));
const spentByBranch = computed(() => BRANCHES.map(branch => ({ ...branch, spent: talents.value.filter(spec => spec.branch === branch.id).reduce((total, spec) => total + spec.rank, 0) })));
const currentPhase = computed(() => [...PHASES].reverse().find(phase => spent.value >= phase.from) ?? PHASES[0]);
const hovered = computed(() => byId.value.get(hoveredId.value));
const selected = computed(() => byId.value.get(selectedId.value));
const popupNode = computed(() => hovered.value ?? selected.value);
const popupStyle = computed(() => {
  const node = popupNode.value;
  if (!node) return {};
  const width = 272;
  const height = 226;
  let left = node.x + 34;
  if (left + width > STAGE_W - 6) left = node.x - 34 - width;
  let top = node.y - 30;
  if (top + height > STAGE_H - 6) top = STAGE_H - height - 6;
  if (top < 6) top = 6;
  return { left: `${left}px`, top: `${top}px`, width: `${width}px` };
});
const stageStyle = computed(() => ({ width: `${STAGE_W}px`, height: `${STAGE_H}px`, transform: `scale(${zoom.value})`, 'transform-origin': '50% 50%' }));
const xpWidth = computed(() => `${Math.round(xp.value / xpMax * 100)}%`);
const stars = Array.from({ length: 72 }, (_, index) => {
  const a = Math.sin(index * 12.9898) * 43758.5453;
  const b = Math.sin(index * 78.233) * 12345.6789;
  const c = Math.sin(index * 4.1414) * 9871.233;
  return { id: `talent-star-${index}`, left: `${((a - Math.floor(a)) * 100).toFixed(2)}%`, top: `${((b - Math.floor(b)) * 100).toFixed(2)}%`,
    size: index % 7 === 0 ? 3 : 2, delayClass: `talent-star-d${Math.floor((c - Math.floor(c)) * 6)}` };
});

function status(node: { id: string; rank: number; requires: string[] }): string {
  if (node.rank > 0) return 'unlocked';
  return node.requires.every(id => (byId.value.get(id)?.rank ?? 0) > 0) ? 'available' : 'locked';
}
function canAllocate(node: { rank: number; maxRank: number; requires: string[] }): boolean {
  if (node.rank >= node.maxRank) return false;
  if (points.value < 1) return false;
  return node.requires.every(id => (byId.value.get(id)?.rank ?? 0) > 0);
}
async function animateSelection(nodeId: string): Promise<void> {
  const revision = ++selectionAnimationRevision;
  detailAnimation?.cancel();
  ringAnimation?.cancel();
  detailAnimation = undefined;
  ringAnimation = undefined;
  await nextTick();
  if (revision !== selectionAnimationRevision || selectedId.value !== nodeId) return;

  detailAnimation = adaptAnimationJs({
    el: '#talent-popup',
    draw: { opacity: [0, 1], translateY: [18, 0] },
    dur: 320,
    ease: 'easeInOutQuad',
  });
  ringAnimation = adaptAnimationJs({
    el: `#talent-ring-${nodeId}`,
    draw: { scale: [1, 1.28] },
    dur: 180,
    ease: 'easeInOutQuad',
    dir: 'alternate',
    loop: 1,
  });
}
function allocate(node: TalentSpec): void {
  selectedId.value = node.id;
  void animateSelection(node.id);
  const spec = talents.value.find(item => item.id === node.id)!;
  if (!canAllocate(node)) { notice.value = spec.rank >= spec.maxRank ? `${spec.name} is already at maximum rank.` : points.value < 1 ? 'No talent point available. Respec to recover points.' : 'Prerequisite node is not unlocked yet.'; return; }
  spec.rank += 1;
  points.value -= 1;
  notice.value = `${spec.name} raised to rank ${spec.rank} / ${spec.maxRank}.`;
}
function respec(): void {
  for (const spec of talents.value) spec.rank = 0;
  points.value = TOTAL_POINTS;
  notice.value = 'All points refunded.';
}
function setZoom(next: number): void { zoom.value = Math.max(0.65, Math.min(1.15, Math.round(next * 100) / 100)); }
function toggleBranch(branch: BranchId): void { focusBranch.value = focusBranch.value === branch ? '' : branch; }
function isDimmed(branch: BranchId): boolean { return focusBranch.value !== '' && focusBranch.value !== branch; }
function isEdgeActive(edge: { from: string; to: string }): boolean {
  const target = hoveredId.value || selectedId.value;
  return target !== '' && (edge.from === target || edge.to === target);
}
onUnmounted(() => {
  ++selectionAnimationRevision;
  detailAnimation?.cancel();
  ringAnimation?.cancel();
  detailAnimation = undefined;
  ringAnimation = undefined;
});
</script>

<template>
  <div id="talent-tree" class="talent-page">
    <div class="talent-toolbar">
      <div>
        <span class="talent-eyebrow">NEXUS SYSTEM / PLACEHOLDER DATA</span>
        <strong>Consciousness talent lattice</strong>
      </div>
      <div class="talent-actions">
        <button id="talent-zoom-out" class="talent-icon-button" title="Zoom out" @click="setZoom(zoom - 0.1)"><img src="icons/zoom-out.png" /></button>
        <span id="talent-zoom-value" class="talent-zoom-value">{{ Math.round(zoom * 100) }}%</span>
        <button id="talent-zoom-in" class="talent-icon-button" title="Zoom in" @click="setZoom(zoom + 0.1)"><img src="icons/zoom-in.png" /></button>
        <button id="talent-respec" class="talent-reset-button" title="Refund every point" @click="respec"><img src="icons/rotate-ccw.png" /><span>Respec</span></button>
      </div>
    </div>

    <div class="talent-body">
      <div class="talent-side">
        <div class="talent-points">
          <span class="talent-side-label">TALENT POINTS</span>
          <strong id="talent-points">{{ points }}</strong>
          <span class="talent-side-note">{{ spent }} points invested</span>
        </div>
        <div class="talent-phases">
          <div v-for="phase in PHASES" :key="phase.id" class="talent-phase" :class="currentPhase.id === phase.id ? 'current' : ''">
            <span class="talent-phase-dot"></span><span>{{ phase.label }}</span>
          </div>
        </div>
        <div class="talent-hints">
          <p class="talent-hint"><strong>Hover</strong> a node to highlight its path and open the detail card.</p>
          <p class="talent-hint"><strong>Click</strong> a node to pin it and spend one talent point.</p>
          <p class="talent-hint"><strong>Branch column</strong> on the right isolates a single line.</p>
        </div>
        <p id="talent-notice" class="talent-notice">{{ notice || 'Select a node to begin.' }}</p>
      </div>

      <div id="talent-viewport" class="talent-viewport">
        <div id="talent-stage" class="talent-stage" :style="stageStyle">
          <div v-for="star in stars" :key="star.id" class="talent-star" :class="star.delayClass" :style="{ left: star.left, top: star.top, width: `${star.size}px`, height: `${star.size}px` }"></div>

          <div class="talent-core">
            <span class="talent-orbit orbit-a"></span><span class="talent-orbit orbit-b"></span><span class="talent-orbit orbit-c"></span>
            <div class="talent-core-glyph"><strong>NEXUS</strong><span>CONSCIOUSNESS</span></div>
          </div>

          <div v-for="edge in edges" :id="'talent-edge-' + edge.id" :key="edge.id" class="talent-edge" :class="[edge.branch, { linked: edge.linked, active: isEdgeActive(edge), dimmed: isDimmed(edge.branch) }]" :style="{ left: `${edge.x}px`, top: `${edge.y}px`, width: `${edge.length}px`, transform: `rotate(${edge.angle}deg)`, 'border-top-color': edge.color }"></div>

          <div v-for="node in nodes" :key="node.id" class="talent-shell" :class="[{ hovered: hoveredId === node.id, selected: selectedId === node.id, dimmed: isDimmed(node.branch) }]" :style="{ left: `${node.x - 23}px`, top: `${node.y - 23}px` }">
            <span :id="'talent-ring-' + node.id" class="talent-shell-ring" :class="[{ visible: hoveredId === node.id, selected: selectedId === node.id }]"></span>
            <button :id="'talent-node-' + node.id" class="talent-node" :class="[node.branch, status(node), { dimmed: isDimmed(node.branch) }]" :title="node.name" @mouseenter="hoveredId = node.id" @mouseleave="hoveredId = ''" @click="allocate(node)">
              <span class="talent-node-rank">{{ node.rank }}<span class="talent-node-max">/{{ node.maxRank }}</span></span>
            </button>
          </div>

          <div v-if="popupNode" id="talent-popup" class="talent-popup" :class="popupNode.branch" :style="popupStyle">
            <div class="talent-popup-head">
              <div><span class="talent-popup-branch">{{ popupNode.branchLabel }} / TIER {{ popupNode.tier + 1 }}</span><strong id="talent-popup-name">{{ popupNode.name }}</strong></div>
              <span id="talent-popup-rank" class="talent-popup-rank">{{ popupNode.rank }}/{{ popupNode.maxRank }}</span>
            </div>
            <p class="talent-popup-summary">{{ popupNode.summary }}</p>
            <p class="talent-popup-detail">{{ popupNode.detail }}</p>
            <div class="talent-popup-effects">
              <div v-for="effect in popupNode.effects" :key="effect.label" class="talent-popup-effect"><span>{{ effect.label }}</span><strong>{{ effect.value }}</strong></div>
            </div>
            <div class="talent-popup-foot">
              <span id="talent-popup-state" class="talent-popup-state" :class="status(popupNode)">{{ status(popupNode) }}</span>
              <span class="talent-popup-cost">COST {{ popupNode.cost }} PT</span>
              <span class="talent-popup-req">{{ popupNode.requires.length ? 'REQ ' + popupNode.requires.join(' + ') : 'ROOT NODE' }}</span>
            </div>
          </div>
        </div>
      </div>

      <div class="talent-branches">
        <span class="talent-side-label">BRANCHES</span>
        <button v-for="branch in spentByBranch" :id="'talent-branch-' + branch.id" :key="branch.id" class="talent-branch" :class="focusBranch === branch.id ? 'active' : ''" @click="toggleBranch(branch.id)">
          <span class="talent-branch-dot" :style="{ 'background-color': branch.color }"></span>
          <span class="talent-branch-label">{{ branch.label }}</span>
          <strong>{{ branch.spent }}</strong>
        </button>
      </div>
    </div>

    <div class="talent-footer">
      <div class="talent-xp">
        <div class="talent-xp-head"><span>EXPERIENCE</span><strong id="talent-xp">{{ xp }} / {{ xpMax }}</strong></div>
        <div class="talent-xp-track"><span :style="{ width: xpWidth }"></span></div>
      </div>
      <div class="talent-stats">
        <div><span>SELECTED</span><strong id="talent-selected">{{ selected ? selected.name : 'None' }}</strong></div>
        <div><span>INVESTED</span><strong id="talent-spent">{{ spent }}</strong></div>
        <div><span>AVAILABLE</span><strong id="talent-available">{{ points }}</strong></div>
      </div>
    </div>
  </div>
</template>

<style>
.talent-page { display:flex; flex-direction:column; min-height:0; flex:1; background-color:#0a141d; color:#dbe7ee; }
.talent-toolbar { display:flex; align-items:center; justify-content:space-between; flex-shrink:0; min-height:58px; padding:10px 18px; border-bottom:1px #1e3442; background-color:#0e1c26; }
.talent-toolbar strong { margin-top:3px; color:#e8f3f8; font-size:14px; }
.talent-eyebrow { color:#5f8496; font-family:"JetBrains Mono"; font-size:9px; }
.talent-actions { display:flex; align-items:center; gap:6px; }
.talent-icon-button { display:flex; align-items:center; justify-content:center; width:32px; height:32px; padding:0; border:1px #2c4a5b; border-radius:4px; background-color:#132735; }
.talent-icon-button:hover,.talent-reset-button:hover { border-color:#3ad0ae; background-color:#173b3a; }
.talent-icon-button img,.talent-reset-button img { width:16px; height:16px; }
.talent-zoom-value { width:46px; color:#9fb8c4; font-family:"JetBrains Mono"; font-size:10px; text-align:center; }
.talent-reset-button { display:flex; align-items:center; gap:7px; height:32px; padding:0 10px; border:1px #2c4a5b; border-radius:4px; background-color:#132735; color:#b9cfda; font-size:10px; }
.talent-body { display:grid; grid-template-columns:190px minmax(0px,1fr) 150px; min-height:0; flex:1; }
.talent-side { display:flex; flex-direction:column; gap:14px; min-width:0; padding:16px 14px; border-right:1px #1b3140; background-color:#0c1922; }
.talent-side-label { color:#5f8496; font-family:"JetBrains Mono"; font-size:9px; }
.talent-points { padding:12px 14px; border:1px #234454; border-radius:5px; background-color:#10283a; }
.talent-points strong { margin-top:6px; color:#4fe0bd; font-family:"JetBrains Mono"; font-size:34px; }
.talent-side-note { margin-top:4px; color:#87a6b5; font-size:10px; }
.talent-phases { display:flex; flex-direction:column; gap:6px; }
.talent-phase { display:flex; align-items:center; gap:9px; padding:7px 10px; border-left:3px #274a5b; background-color:#0f212c; color:#5d7c8b; font-family:"JetBrains Mono"; font-size:10px; }
.talent-phase-dot { width:7px; height:7px; border-radius:7px; background-color:#33566a; }
.talent-phase.current { border-left-color:#3ad0ae; background-color:#12303a; color:#d6ecef; }
.talent-phase.current .talent-phase-dot { background-color:#3ad0ae; }
.talent-hints { display:flex; flex-direction:column; gap:8px; }
.talent-hint { margin:0; color:#7c9aa9; font-size:10px; line-height:1.55; }
.talent-hint strong { display:inline; color:#b6d3de; }
.talent-notice { margin:0; min-height:34px; padding:8px 10px; border-left:3px #d69a2f; background-color:#1a1b12; color:#d8c58f; font-size:10px; line-height:1.45; }
.talent-viewport { position:relative; min-width:0; min-height:0; overflow:hidden; background-color:#08111a; }
.talent-stage { position:relative; margin:0 auto; overflow:hidden; background-color:#08111a; }
.talent-star { position:absolute; border-radius:4px; background-color:#cfe6f2; }
.talent-star-d0 { animation:2.2s cubic-in-out talent-twinkle infinite alternate; }
.talent-star-d1 { animation:2.6s cubic-in-out talent-twinkle infinite alternate; }
.talent-star-d2 { animation:3s cubic-in-out talent-twinkle infinite alternate; }
.talent-star-d3 { animation:3.4s cubic-in-out talent-twinkle infinite alternate; }
.talent-star-d4 { animation:3.8s cubic-in-out talent-twinkle infinite alternate; }
.talent-star-d5 { animation:4.2s cubic-in-out talent-twinkle infinite alternate; }
@keyframes talent-twinkle { from { opacity:0.12; } to { opacity:0.72; } }
.talent-core { position:absolute; left:402px; top:218px; display:flex; align-items:center; justify-content:center; width:176px; height:176px; border:3px #3ad0ae; border-radius:176px; background-color:#0d2b33; }
.talent-orbit { position:absolute; width:150px; height:150px; border:1px #57d8bb66; border-radius:150px; }
.orbit-a { transform:rotate(0deg); } .orbit-b { transform:rotate(60deg); } .orbit-c { transform:rotate(120deg); }
.talent-core-glyph { display:flex; flex-direction:column; align-items:center; justify-content:center; text-align:center; }
.talent-core-glyph strong { color:#e7fbff; font-family:"JetBrains Mono"; font-size:15px; }
.talent-core-glyph span { margin-top:4px; color:#79c6c0; font-family:"JetBrains Mono"; font-size:7px; }
.talent-edge { position:absolute; height:0; border-top:2px #2d4b5a; transform-origin:0px 50%; opacity:0.55; }
.talent-edge.linked { opacity:1; border-top-width:3px; }
.talent-edge.active { opacity:1; border-top-width:4px; }
.talent-edge.dimmed { opacity:0.12; }
.talent-shell { position:absolute; width:46px; height:46px; }
.talent-shell-ring { position:absolute; top:-7px; left:-7px; box-sizing:border-box; width:60px; height:60px; border:2px transparent; border-radius:60px; }
.talent-shell-ring.visible { border-color:#ffffff; }
.talent-shell-ring.selected { border-color:#3ad0ae; }
.talent-node { position:relative; display:flex; align-items:center; justify-content:center; box-sizing:border-box; width:46px; height:46px; padding:0; border:2px #33566a; border-radius:46px; background-color:#10222d; color:#7f9fae; font-family:"JetBrains Mono"; font-size:13px; }
.talent-shell.hovered .talent-node { border-width:3px; }
.talent-shell.selected .talent-node { border-width:4px; }
.talent-node-rank { font-size:13px; }
.talent-node-max { display:inline; color:#5c7d8d; font-size:9px; }
.talent-node.available { border-color:#d8c477; color:#2b2408; background-color:#d8c477; }
.talent-node.unlocked { color:#ffffff; }
.talent-node.consciousness.unlocked { background-color:#4aa3dd; }
.talent-node.weapon.unlocked { background-color:#e0a63a; }
.talent-node.body.unlocked { background-color:#1fa088; }
.talent-node.harmony.unlocked { background-color:#8f6bb8; }
.talent-node.infinity.unlocked { background-color:#d4695a; }
.talent-node.hovered { border-color:#ffffff; color:#06202b; background-color:#ffffff; }
.talent-node.selected { border-width:4px; border-color:#3ad0ae; }
.talent-node.dimmed { opacity:0.22; }
.talent-popup { position:absolute; box-sizing:border-box; padding:12px 14px; border:2px #4d90a8; border-left:5px #3ad0ae; border-radius:5px; background-color:#0d1f2af2; pointer-events:none; }
.talent-popup.consciousness { border-left-color:#4aa3dd; } .talent-popup.weapon { border-left-color:#e0a63a; } .talent-popup.body { border-left-color:#1fa088; }
.talent-popup.harmony { border-left-color:#8f6bb8; } .talent-popup.infinity { border-left-color:#d4695a; }
.talent-popup-head { display:flex; align-items:flex-start; justify-content:space-between; gap:10px; }
.talent-popup-branch { color:#5f8496; font-family:"JetBrains Mono"; font-size:8px; }
.talent-popup-head strong { margin-top:3px; color:#eaf6fb; font-size:14px; }
.talent-popup-rank { padding:3px 7px; border:1px #2f5871; border-radius:3px; background-color:#12303e; color:#7fd9c4; font-family:"JetBrains Mono"; font-size:11px; }
.talent-popup-summary { margin:8px 0 0; color:#b8d3de; font-size:10px; line-height:1.5; }
.talent-popup-detail { margin:6px 0 0; color:#8fb0bf; font-size:9px; line-height:1.55; }
.talent-popup-effects { margin-top:9px; border-top:1px #1f3d4d; }
.talent-popup-effect { display:flex; align-items:center; justify-content:space-between; min-height:22px; border-bottom:1px #162e3a; color:#89a9b8; font-size:9px; }
.talent-popup-effect strong { color:#d6ecef; font-family:"JetBrains Mono"; font-size:10px; }
.talent-popup-foot { display:flex; align-items:center; gap:7px; margin-top:9px; color:#648a9c; font-family:"JetBrains Mono"; font-size:8px; }
.talent-popup-state { padding:2px 6px; border-radius:3px; background-color:#1a3340; color:#8fb0bf; }
.talent-popup-state.unlocked { background-color:#123f33; color:#63dcbb; } .talent-popup-state.available { background-color:#332c12; color:#e0c877; }
.talent-branches { display:flex; flex-direction:column; gap:6px; min-width:0; padding:16px 12px; border-left:1px #1b3140; background-color:#0c1922; }
.talent-branch { display:flex; align-items:center; gap:8px; min-height:40px; padding:7px 9px; border:1px #1e3b4a; border-radius:4px; background-color:#0f212c; color:#7f9fae; }
.talent-branch:hover { border-color:#3ad0ae; background-color:#132e33; }
.talent-branch.active { border-color:#3ad0ae; background-color:#16323a; color:#e2f4f8; }
.talent-branch-dot { width:9px; height:9px; border-radius:9px; flex-shrink:0; }
.talent-branch-label { flex:1; min-width:0; overflow:hidden; font-family:"JetBrains Mono"; font-size:9px; white-space:nowrap; }
.talent-branch strong { color:#d9eef4; font-family:"JetBrains Mono"; font-size:12px; }
.talent-footer { display:grid; grid-template-columns:minmax(0px,1fr) 320px; flex-shrink:0; gap:18px; align-items:center; min-height:72px; padding:12px 18px; border-top:1px #1e3442; background-color:#0e1c26; }
.talent-xp-head { display:flex; align-items:center; justify-content:space-between; margin-bottom:7px; }
.talent-xp-head span { color:#5f8496; font-family:"JetBrains Mono"; font-size:9px; }
.talent-xp-head strong { color:#cfe6f2; font-family:"JetBrains Mono"; font-size:12px; }
.talent-xp-track { height:8px; overflow:hidden; border-radius:4px; background-color:#16303d; }
.talent-xp-track span { display:block; height:8px; border-radius:4px; background-color:#3ad0ae; }
.talent-stats { display:grid; grid-template-columns:repeat(3,minmax(0px,1fr)); gap:8px; }
.talent-stats div { min-width:0; padding:7px 10px; border-left:3px #2d5d6b; background-color:#102530; }
.talent-stats span { color:#5f8496; font-family:"JetBrains Mono"; font-size:8px; }
.talent-stats strong { margin-top:3px; overflow:hidden; color:#d9eef4; font-size:12px; white-space:nowrap; }
@media (max-width:900px) { .talent-body { grid-template-columns:150px minmax(0px,1fr) 118px; } .talent-footer { grid-template-columns:minmax(0px,1fr); } }
</style>
