import { hierarchy, tree, type HierarchyPointNode } from 'd3-hierarchy';

export interface TreeCanvasNode {
  id: string;
  label: string;
  detail: string;
  kind: 'runtime' | 'render' | 'bridge' | 'tooling';
  children?: TreeCanvasNode[];
}

export interface PositionedTreeNode extends TreeCanvasNode {
  left: number;
  top: number;
  hasChildren: boolean;
  collapsed: boolean;
}

export interface TreeEdgeSegment {
  id: string;
  left: number;
  top: number;
  width: number;
  height: number;
}

export interface TreeLayoutResult {
  width: number;
  height: number;
  nodes: PositionedTreeNode[];
  edges: TreeEdgeSegment[];
}

export const TREE_NODE_WIDTH = 176;
export const TREE_NODE_HEIGHT = 44;
const HORIZONTAL_STEP = 220;
const VERTICAL_STEP = 46;
const PADDING = 24;

function segment(id: string, left: number, top: number, width: number, height: number): TreeEdgeSegment {
  return { id, left: Math.round(left), top: Math.round(top), width: Math.max(2, Math.round(width)), height: Math.max(2, Math.round(height)) };
}

export function layoutTree(rootData: TreeCanvasNode, collapsed: ReadonlySet<string>): TreeLayoutResult {
  const seen = new Set<string>();
  const root = hierarchy(rootData, node => collapsed.has(node.id) ? undefined : node.children);
  for (const node of root.descendants()) {
    if (seen.has(node.data.id)) throw new Error(`Duplicate tree node id: ${node.data.id}`);
    seen.add(node.data.id);
  }

  const positioned = tree<TreeCanvasNode>().nodeSize([VERTICAL_STEP, HORIZONTAL_STEP])(root);
  const descendants = positioned.descendants();
  const minTreeX = Math.min(...descendants.map(node => node.x));
  const positions = new Map<string, { left: number; top: number }>();
  const nodes = descendants.map(node => {
    const left = node.y + PADDING;
    const top = node.x - minTreeX + PADDING;
    positions.set(node.data.id, { left, top });
    return {
      ...node.data,
      left,
      top,
      hasChildren: Boolean(node.data.children?.length),
      collapsed: collapsed.has(node.data.id),
    };
  });

  const edges: TreeEdgeSegment[] = [];
  for (const link of positioned.links()) {
    appendOrthogonalEdge(edges, link.source, link.target, positions);
  }
  return {
    width: Math.max(...nodes.map(node => node.left)) + TREE_NODE_WIDTH + PADDING,
    height: Math.max(...nodes.map(node => node.top)) + TREE_NODE_HEIGHT + PADDING,
    nodes,
    edges,
  };
}

function appendOrthogonalEdge(
  edges: TreeEdgeSegment[],
  source: HierarchyPointNode<TreeCanvasNode>,
  target: HierarchyPointNode<TreeCanvasNode>,
  positions: ReadonlyMap<string, { left: number; top: number }>,
): void {
  const from = positions.get(source.data.id)!;
  const to = positions.get(target.data.id)!;
  const startX = from.left + TREE_NODE_WIDTH;
  const startY = from.top + TREE_NODE_HEIGHT / 2;
  const endX = to.left;
  const endY = to.top + TREE_NODE_HEIGHT / 2;
  const middleX = (startX + endX) / 2;
  const prefix = `tree-edge-${source.data.id}-${target.data.id}`;
  edges.push(segment(`${prefix}-start`, startX, startY - 1, middleX - startX, 2));
  edges.push(segment(`${prefix}-branch`, middleX - 1, Math.min(startY, endY), 2, Math.abs(endY - startY) + 2));
  edges.push(segment(`${prefix}-end`, middleX, endY - 1, endX - middleX, 2));
}
