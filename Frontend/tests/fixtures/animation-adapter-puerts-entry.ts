import {
  adaptAnimeJs,
  adaptAnimeTimeline,
  adaptGsapTimeline,
  adaptGsapTo,
  animeTimelinePositionStagger,
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
}));
native.ReportReady();
