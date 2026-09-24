import { computePosition, flip, hide, offset, shift, type Placement, type Platform } from '@floating-ui/core';
import type { LayoutSnapshot, NodeMetrics } from '../../../../Frontend/src/platform';

/** Floating UI's pure core receives native geometry; no DOM package is imported. */
export function createFloatingPlatform(snapshot: LayoutSnapshot): Platform {
  const metric = (element: NodeMetrics) => snapshot.nodes.find(node => node.handle === element.handle) || element;
  return {
    getElementRects: ({ reference, floating }) => {
      const anchor = metric(reference), panel = metric(floating);
      return { reference: { x: anchor.x, y: anchor.y, width: anchor.width, height: anchor.height }, floating: { x: 0, y: 0, width: panel.width, height: panel.height } };
    },
    getDimensions: element => ({ width: metric(element).width, height: metric(element).height }),
    getClippingRect: ({ element }) => {
      const node = metric(element);
      return { x: node.clipX ?? 0, y: node.clipY ?? 0, width: node.clipWidth ?? snapshot.viewport.width, height: node.clipHeight ?? snapshot.viewport.height };
    },
    convertOffsetParentRelativeRectToViewportRelativeRect: ({ rect }) => rect,
    getScale: () => ({ x: 1, y: 1 }),
    isElement: element => typeof element?.handle === 'number',
    isRTL: () => false,
  };
}
export async function positionPopover(snapshot: LayoutSnapshot, anchor: number, panel: number, placement: Placement = 'bottom-start') {
  const reference = snapshot.nodes.find(node => node.handle === anchor);
  const floating = snapshot.nodes.find(node => node.handle === panel);
  if (!reference || !floating || !reference.width || !floating.width) return null;
  return computePosition(reference, floating, {
    strategy: 'fixed', placement, platform: createFloatingPlatform(snapshot),
    middleware: [offset(8), flip({ padding: 12 }), shift({ padding: 12 }), hide({ strategy: 'referenceHidden' })],
  });
}
