import { native } from '../../src/bridge';
import {
  getAnimationCompletionDebugState,
  getAnimationStartDebugState,
  startAnimations,
  type RmlAnimation,
  type RmlAnimationRequest,
} from '../../src/animation';

interface BurstConfig {
  count?: number;
  batchCount?: number;
  continuationOperations?: number;
}

let config: BurstConfig = {};
try { config = JSON.parse(native.StateJson) as BurstConfig; } catch { /* Use defaults. */ }
const count = Number.isInteger(config.count) && config.count! > 0 ? config.count! : 1000;
const batchCount = Number.isInteger(config.batchCount) && config.batchCount! > 0
  ? config.batchCount! : 1;
const continuationOperations = Number.isInteger(config.continuationOperations) &&
  config.continuationOperations! >= 0 ? config.continuationOperations! : 0;
if (batchCount > 4 || batchCount > count) throw new Error('Invalid completion burst batch count');

const animations: RmlAnimation[] = [];
let requestOffset = 0;
for (let batchIndex = 0; batchIndex < batchCount; ++batchIndex) {
  const batchSize = Math.floor(count / batchCount) + (batchIndex < count % batchCount ? 1 : 0);
  const node = native.FindNode(batchCount === 1 ? 'completion-target' : `completion-target-${batchIndex}`);
  const requests: RmlAnimationRequest[] = Array.from({ length: batchSize }, (_, index) => ({
    node,
    property: 'opacity',
    keyframes: [
      { offset: 0, value: 0.25, easing: 'linear' },
      { offset: 1, value: 1 },
    ],
    options: {
      duration: 0.001,
      fill: 'both',
      composite: 'layered-replace',
      compositionOrder: requestOffset + index,
      fallback: 'reject',
    },
  }));
  animations.push(...startAnimations(requests));
  requestOffset += batchSize;
}
let settled = 0;
let continuationExecutions = 0;
let continuationChecksum = 0;
for (const animation of animations) {
  void animation.finished.then(result => {
    const continuationSeed = Number(result.handle.slice(-6));
    for (let operation = 0; operation < continuationOperations; ++operation) {
      continuationChecksum = (continuationChecksum +
        ((operation + continuationSeed) & 0xff)) >>> 0;
    }
    ++continuationExecutions;
    if (result.reason === 'completed') ++settled;
    if (settled === count) {
      const completion = getAnimationCompletionDebugState();
      const start = getAnimationStartDebugState();
      native.ReportDebugState(JSON.stringify({
        count, batchCount, settled,
        batchDispatches: completion.batchDispatches,
        batchEvents: completion.events,
        continuationOperations, continuationExecutions, continuationChecksum,
        batchTransport: completion.transport,
        startTransport: start.transport,
        planRegistrations: start.registrations,
        planCacheHits: start.cacheHits,
        cachedPlans: start.cachedPlans,
        state: 'finished',
      }));
    }
  });
}

const initialCompletion = getAnimationCompletionDebugState();
const initialStart = getAnimationStartDebugState();
native.ReportDebugState(JSON.stringify({
  count, batchCount, settled,
  batchDispatches: initialCompletion.batchDispatches,
  batchEvents: initialCompletion.events,
  continuationOperations, continuationExecutions, continuationChecksum,
  batchTransport: initialCompletion.transport,
  startTransport: initialStart.transport,
  planRegistrations: initialStart.registrations,
  planCacheHits: initialStart.cacheHits,
  cachedPlans: initialStart.cachedPlans,
  state: 'running',
}));
native.ReportReady();
