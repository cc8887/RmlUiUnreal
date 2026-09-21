import {
  adaptAnimationJs,
  adaptAnimeJs,
  adaptGsapFromTo,
  RML_ANIMATION_ADAPTER_VERSIONS,
  type RmlAnimationGroup,
} from '../../src/animation-adapters';
import { getAnimationStartDebugState } from '../../src/animation';
import { native } from '../../src/bridge';

const phases = [
  { name: 'cold', plans: [0, 1, 2, 3, 4, 5, 6, 7] },
  { name: 'churn', plans: [4, 5, 6, 7, 8, 9, 10, 11] },
  { name: 'hot', plans: [4, 5, 6, 7, 8, 9, 10, 11] },
] as const;

const phaseStats: Array<Record<string, unknown>> = [];

function resetTarget(index: number): string {
  const id = `plan-cache-target-${index}`;
  const node = native.FindNode(id);
  if (!node || !native.SetProperty(node, 'opacity', '1', false))
    throw new Error(`Unable to reset ${id}`);
  return `#${id}`;
}

function startPlan(index: number): RmlAnimationGroup {
  const selector = resetTarget(index);
  const opacity = 0.2 + index * 0.025;
  const durationMilliseconds = 20 + index;
  switch (index % 3) {
    case 0:
      return adaptAnimationJs({
        el: selector,
        draw: { opacity },
        dur: durationMilliseconds,
        ease: 'linear',
      });
    case 1:
      return adaptAnimeJs(selector, {
        opacity,
        duration: durationMilliseconds,
        ease: 'linear',
      });
    default:
      return adaptGsapFromTo(selector, { opacity: 1 }, {
        opacity,
        duration: durationMilliseconds / 1000,
        ease: 'none',
      });
  }
}

function runPhase(index: number): void {
  const phase = phases[index];
  const animations = phase.plans.flatMap(plan => startPlan(plan).animations);
  void Promise.all(animations.map(animation => animation.finished)).then(results => {
    if (results.some(result => result.reason !== 'completed'))
      throw new Error(`${phase.name} phase did not complete cleanly`);
    phaseStats.push({ name: phase.name, ...getAnimationStartDebugState() });
    if (index + 1 < phases.length) {
      runPhase(index + 1);
      return;
    }
    native.ReportDebugState(JSON.stringify({
      state: 'finished',
      versions: RML_ANIMATION_ADAPTER_VERSIONS,
      phaseStats,
      ...getAnimationStartDebugState(),
    }));
  });
}

runPhase(0);
native.ReportDebugState(JSON.stringify({
  state: 'running',
  phaseStats,
  ...getAnimationStartDebugState(),
}));
native.ReportReady();
