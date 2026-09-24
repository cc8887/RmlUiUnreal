import { native, check } from '../../../../Frontend/src/bridge';
import { afterLayout, containsNode } from '../../../../Frontend/src/platform';

interface ModalEntry { root: number; initial: number; restore: number; cancelReady: () => void }
const stack: ModalEntry[] = [];

/** Native modal scope owns input isolation and Tab order; this stack owns nesting. */
export function pushModal(root: number, initial: number, restore: number): () => void {
  const entry: ModalEntry = { root, initial, restore: restore || native.ActiveNode(), cancelReady: () => {} };
  stack.push(entry);
  check(native.SetModalRoot(root, 0));
  entry.cancelReady = afterLayout(() => {
    if (stack[stack.length - 1] === entry && native.IsNodeValid(root)) check(native.SetModalRoot(root, initial));
  });
  return () => {
    entry.cancelReady();
    const index = stack.indexOf(entry);
    if (index < 0) return;
    const wasTop = index === stack.length - 1;
    stack.splice(index, 1);
    if (!wasTop) return;
    while (stack.length && !native.IsNodeValid(stack[stack.length - 1].root)) stack.pop();
    const parent = stack[stack.length - 1];
    const restoreValid = entry.restore && native.IsNodeValid(entry.restore);
    const restore = restoreValid && (!parent || containsNode(parent.root, entry.restore)) ? entry.restore : parent?.initial || 0;
    check(native.SetModalRoot(parent?.root || 0, parent ? restore : 0));
    if (!parent && restore) native.FocusNode(restore);
  };
}
export function isTopModal(root: number): boolean { return stack[stack.length - 1]?.root === root; }
export function modalDepth(): number { return stack.length; }
