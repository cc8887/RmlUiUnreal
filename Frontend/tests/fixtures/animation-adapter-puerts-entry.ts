import '../../src/runtime';
import {
  adaptAnimationJs,
  adaptAnimeJs,
  adaptAnimeTimeline,
  adaptGsapTimeline,
  adaptGsapTo,
  animeTimelinePositionStagger,
  captureAnimationHostSnapshot,
  RmlAnimationAdapterError,
  RML_ANIMATION_ADAPTER_VERSIONS,
} from '../../src/animation-adapters';
import { native } from '../../src/bridge';

const keyframeGroup = adaptAnimeJs('#keyframe-target', {
  ease: 'linear',
  keyframes: [
    { opacity: 0.7, duration: 75, ease: 'linear' },
    { duration: 25 },
    { opacity: 0.4, duration: 100, ease: 'ease-in' },
  ],
});
const staggerGroup = adaptGsapTo('.stagger-target', {
  duration: 0.15,
  ease: 'none',
  stagger: 0.05,
  keyframes: [
    { opacity: '-=0.1', duration: 0.05, ease: 'none' },
    { opacity: '-=0.1', delay: 0.025, duration: 0.05, ease: 'none' },
  ],
});
const timelineGroup = adaptGsapTimeline([
  { label: 'intro', position: 0 },
  { targets: '#keyframe-target', position: 'intro', vars: { scale: 1.1, duration: 0.05, ease: 'none' } },
  { targets: '#keyframe-target', position: '>', set: { scale: 1.05 } },
  { targets: '#keyframe-target', position: '>', vars: { scale: 1, duration: 0.05, ease: 'none' } },
]);
const animeTimelineGroup = adaptAnimeTimeline([
  { label: 'intro', position: 0 },
  {
    targets: '.stagger-target',
    position: animeTimelinePositionStagger(25, { start: 'intro' }),
    params: { scale: 1.05, duration: 50, ease: 'in(2)' },
  },
  { targets: '.stagger-target', position: '<<+=12.5', params: { scale: 1, duration: 50, ease: 'linear' } },
]);
const officialSnapshot = captureAnimationHostSnapshot(
  ['#official-field', '#official-ball'], [], true,
);
const officialField = officialSnapshot.nodes.find(
  node => node.node === officialSnapshot.targetGroups[0][0],
)?.metrics;
const officialBall = officialSnapshot.nodes.find(
  node => node.node === officialSnapshot.targetGroups[1][0],
)?.metrics;
if (!officialField || !officialBall) throw new Error('Animation.js official fixture metrics are unavailable');
const officialTravelX = (officialField.clientWidth || officialField.width) -
  (officialBall.clientWidth || officialBall.width);
const officialTravelY = (officialField.clientHeight || officialField.height) -
  (officialBall.clientHeight || officialBall.height);

const officialGaps: Array<{ case: string; code: string }> = [];
function expectOfficialGap(name: string, callback: () => void): void {
  try {
    callback();
    officialGaps.push({ case: name, code: 'unexpected_support' });
  } catch (error) {
    officialGaps.push({
      case: name,
      code: error instanceof RmlAnimationAdapterError ? error.code : 'unexpected_error',
    });
  }
}
expectOfficialGap('readme-infinite-loop', () => {
  adaptAnimationJs({
    el: '#official-ball', draw: { left: [0, officialTravelX] },
    dur: 2000, ease: 'easeOutQuad', loop: true,
  });
});
expectOfficialGap('readme-bounce', () => {
  adaptAnimationJs({
    el: '#official-ball', draw: { top: [0, officialTravelY] },
    dur: 2000, ease: 'easeOutBounce', loop: 1,
  });
});
// The upstream README uses loop:true. This runtime probe is intentionally bounded.
const officialGroups = [
  adaptAnimationJs({
    el: '#official-ball', draw: { left: [0, officialTravelX] },
    dur: 2000, ease: 'easeOutQuad', loop: 2, dir: 'alternate',
  }),
  adaptAnimationJs({
    el: '#official-ball', draw: { rotate: [0, 360] }, dur: 1200, loop: 2,
  }),
  adaptAnimationJs({
    el: '#official-fade', draw: { opacity: [0, 1] }, dur: 300, ease: 'linear',
  }),
  adaptAnimationJs({
    el: '#official-slide', draw: { left: [-100, 0], opacity: [0, 1] }, dur: 300, ease: 'linear',
  }),
  adaptAnimationJs({
    el: '#official-zoom', draw: { scale: [3, 1], opacity: [0, 1] }, dur: 300, ease: 'linear',
  }),
  adaptAnimationJs({
    el: '#official-effect', draw: { scale: [3, 1], rotate: [180, 0], opacity: [0, 1] },
    dur: 300, ease: 'linear',
  }),
];
const officialAnimations = officialGroups.flatMap(group => group.animations);
const animations = [
  ...keyframeGroup.animations,
  ...staggerGroup.animations,
  ...timelineGroup.animations,
  ...animeTimelineGroup.animations,
];

native.ReportDebugState(JSON.stringify({
  versions: RML_ANIMATION_ADAPTER_VERSIONS,
  animationCount: animations.length,
  handles: animations.map(animation => animation.handle),
  routes: animations.map(animation => animation.route),
  states: animations.map(animation => animation.state),
  official: {
    source: 'https://github.com/olton/animation/tree/d1506c290b488aa42d91fa2e26bea5ab3d5f3805',
    travel: { x: officialTravelX, y: officialTravelY },
    animationCount: officialAnimations.length,
    handles: officialAnimations.map(animation => animation.handle),
    nativeHandles: officialAnimations.filter(animation => animation.route === 'native').map(animation => animation.handle),
    routes: officialAnimations.map(animation => animation.route),
    gaps: officialGaps,
  },
}));
native.ReportReady();
