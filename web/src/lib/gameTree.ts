// The solver's game record as a move tree, the way a chess analysis board
// keeps one: a main line plus any number of variations branching from any
// position.
//
// The tree is a flat map of nodes that refer to each other by id, rather
// than nested objects. Updating one node then copies only that node and
// the map, every other node keeps its identity, and React (which compares
// by reference) re-renders only what changed. All functions here are pure;
// the reducer in hooks/useGameTree.ts applies them.

import type { BoardRows, MoveInfo, Seats } from '../api/types';
import { emptyBoard } from './board';

/** Six consecutive scoreless turns end the game. */
export const SCORELESS_TURNS_TO_END = 6;

/** How a node was reached from its parent. */
export interface MoveOrigin {
  player: 0 | 1;
  /**
   * 'play' is a placement, exchange or pass. Imported records add
   * 'withdrawn' (a phony taken back; the board reverts) and 'adjustment'
   * (a challenge bonus or time penalty), which are not moves to analyse.
   */
  kind: 'play' | 'withdrawn' | 'adjustment';
  /** For 'withdrawn' and 'adjustment', a stand-in carrying the points. */
  move: MoveInfo;
  /** Display text when it differs from the notation (imported records). */
  label?: string;
  /** Imported records: a word missing from the lexicon or a score mismatch. */
  warning?: string;
  /** Imported records: the GCG #note for the move. */
  note?: string;
}

/** One position of the game and the move that led to it. */
export interface GameNode {
  id: string;
  parent: string | null;
  /** Continuations; the first is the main one, the rest are variations. */
  children: string[];
  board: BoardRows;
  scores: Seats<number>;
  toMove: 0 | 1;
  /**
   * What is known of each rack here (may be partial or empty). The
   * mover's rack is what gets solved and is the rack "before" the next
   * move; the other is used only when `opponentKnown` is set, and is
   * otherwise kept as that player's leave for their next turn.
   */
  racks: Seats<string>;
  opponentKnown: boolean;
  /** Consecutive scoreless turns leading here (0-6). */
  scoreless: number;
  origin: MoveOrigin | null;
}

export interface GameTree {
  nodes: Readonly<Record<string, GameNode>>;
  rootId: string;
  currentId: string;
  /** Player names apply to the whole game. */
  names: Seats<string>;
  /** Source of fresh node ids, so the reducer stays a pure function. */
  nextId: number;
}

/** The fields of a position that "Edit board" may change. */
export type SetupPatch = Partial<Pick<GameNode, 'board' | 'scores' | 'toMove'>>;
/** Knowledge about the racks, which never invalidates the moves after it. */
export type RackPatch = Partial<Pick<GameNode, 'racks' | 'opponentKnown'>>;

export function other(seat: 0 | 1): 0 | 1 {
  return seat === 0 ? 1 : 0;
}

export function setSeat<T>(pair: Seats<T>, seat: 0 | 1, value: T): Seats<T> {
  return seat === 0 ? [value, pair[1]] : [pair[0], value];
}

export function emptyTree(names: Seats<string> = ['player1', 'player2']): GameTree {
  const root: GameNode = {
    id: 'n0',
    parent: null,
    children: [],
    board: emptyBoard(),
    scores: [0, 0],
    toMove: 0,
    racks: ['', ''],
    opponentKnown: false,
    scoreless: 0,
    origin: null,
  };
  return { nodes: { n0: root }, rootId: 'n0', currentId: 'n0', names, nextId: 1 };
}

export function nodeAt(tree: GameTree, id: string): GameNode {
  const node = tree.nodes[id];
  if (!node) throw new Error(`unknown node ${id}`);
  return node;
}

export function currentNode(tree: GameTree): GameNode {
  return nodeAt(tree, tree.currentId);
}

/** Ids from the root to `id`, inclusive. */
export function pathTo(tree: GameTree, id: string): string[] {
  const path: string[] = [];
  for (let at: string | null = id; at !== null; at = nodeAt(tree, at).parent) path.push(at);
  return path.reverse();
}

/** The main line: from the root, always the first continuation. */
export function mainLine(tree: GameTree): string[] {
  const line = [tree.rootId];
  for (let node = nodeAt(tree, tree.rootId); node.children.length > 0;) {
    node = nodeAt(tree, node.children[0]!);
    line.push(node.id);
  }
  return line;
}

export function isOnMainLine(tree: GameTree, id: string): boolean {
  return pathTo(tree, id).every((at) => {
    const parent = nodeAt(tree, at).parent;
    return parent === null || nodeAt(tree, parent).children[0] === at;
  });
}

/** Moves from the root to `id` (the root is ply 0). */
export function plyOf(tree: GameTree, id: string): number {
  return pathTo(tree, id).length - 1;
}

/** The mover's rack before the move that reached `node` ('' if unknown). */
export function rackBefore(tree: GameTree, node: GameNode): string {
  if (!node.origin || node.parent === null) return '';
  return nodeAt(tree, node.parent).racks[node.origin.player];
}

function descendants(tree: GameTree, id: string): string[] {
  const out: string[] = [];
  const stack = [...nodeAt(tree, id).children];
  while (stack.length > 0) {
    const at = stack.pop()!;
    out.push(at);
    stack.push(...nodeAt(tree, at).children);
  }
  return out;
}

export function descendantCount(tree: GameTree, id: string): number {
  return descendants(tree, id).length;
}

function withNodes(tree: GameTree, nodes: Record<string, GameNode>): GameTree {
  return { ...tree, nodes };
}

/**
 * Changes the set-up of the current position. The moves after it no
 * longer follow from it, so they are removed.
 */
export function editSetup(tree: GameTree, patch: SetupPatch): GameTree {
  const node = currentNode(tree);
  const nodes = { ...tree.nodes };
  for (const id of descendants(tree, node.id)) delete nodes[id];
  nodes[node.id] = { ...node, ...patch, children: [] };
  return withNodes(tree, nodes);
}

export function editRacks(tree: GameTree, patch: RackPatch): GameTree {
  const node = currentNode(tree);
  return withNodes(tree, { ...tree.nodes, [node.id]: { ...node, ...patch } });
}

export interface RecordedMove {
  move: MoveInfo;
  /** Board after the move. */
  board: BoardRows;
  /** What is now known of the mover's rack before the move. */
  rackBefore: string;
  /** The mover's leave, as far as it is known. */
  leave: string;
}

/**
 * Records a move from the current position. Playing a move that already
 * continues this position just follows it; anything else adds a new
 * continuation, which is the main line if there was none and otherwise a
 * variation, so nothing already recorded is lost.
 *
 * What the move reveals about the mover's rack (tiles from outside the
 * known part) is kept on the position only for its first continuation, the
 * game as played; a variation is hypothetical and reveals nothing.
 */
export function recordMove(tree: GameTree, recorded: RecordedMove): GameTree {
  const parent = currentNode(tree);
  const mover = parent.toMove;
  const existing = parent.children.find((id) => {
    const origin = nodeAt(tree, id).origin;
    return origin?.kind === 'play' && origin.move.notation === recorded.move.notation;
  });
  if (existing) return { ...tree, currentId: existing };
  const revealing = parent.children.length === 0;
  const parentRacks = revealing ? setSeat(parent.racks, mover, recorded.rackBefore) : parent.racks;
  const nodes = { ...tree.nodes, [parent.id]: { ...parent, racks: parentRacks } };

  const id = `n${tree.nextId}`;
  const scores = setSeat(parent.scores, mover, parent.scores[mover] + recorded.move.score);
  nodes[id] = {
    id,
    parent: parent.id,
    children: [],
    board: recorded.board,
    scores,
    toMove: other(mover),
    racks: setSeat(parent.racks, mover, recorded.leave),
    // The previous mover's rack is now their leave plus unknown draws.
    opponentKnown: false,
    scoreless:
      recorded.move.score === 0 ? Math.min(parent.scoreless + 1, SCORELESS_TURNS_TO_END) : 0,
    origin: { player: mover, kind: 'play', move: recorded.move },
  };
  nodes[parent.id] = { ...nodes[parent.id]!, children: [...parent.children, id] };
  return { ...tree, nodes, currentId: id, nextId: tree.nextId + 1 };
}

/**
 * Makes the line through `id` the main line: at every branch point on the
 * way up, the branch taken moves to the front.
 */
export function promote(tree: GameTree, id: string): GameTree {
  const nodes = { ...tree.nodes };
  for (let at = nodeAt(tree, id); at.parent !== null; at = nodeAt(tree, at.parent)) {
    const parent = nodes[at.parent]!;
    if (parent.children[0] === at.id) continue;
    nodes[parent.id] = {
      ...parent,
      children: [at.id, ...parent.children.filter((child) => child !== at.id)],
    };
  }
  return withNodes(tree, nodes);
}

/** Removes `id` and everything after it; the root cannot be removed. */
export function removeNode(tree: GameTree, id: string): GameTree {
  const node = nodeAt(tree, id);
  if (node.parent === null) return tree;
  const doomed = new Set([id, ...descendants(tree, id)]);
  const nodes = { ...tree.nodes };
  for (const gone of doomed) delete nodes[gone];
  const parent = nodes[node.parent]!;
  nodes[parent.id] = { ...parent, children: parent.children.filter((child) => child !== id) };
  const currentId = doomed.has(tree.currentId) ? parent.id : tree.currentId;
  return { ...tree, nodes, currentId };
}

export function goTo(tree: GameTree, id: string): GameTree {
  return tree.nodes[id] ? { ...tree, currentId: id } : tree;
}

export type Step = 'back' | 'forward' | 'first' | 'last';

/** Back and Forward follow the current line; Last runs down its main continuation. */
export function step(tree: GameTree, where: Step): GameTree {
  const node = currentNode(tree);
  switch (where) {
    case 'back':
      return node.parent === null ? tree : goTo(tree, node.parent);
    case 'forward':
      return node.children.length === 0 ? tree : goTo(tree, node.children[0]!);
    case 'first':
      return goTo(tree, tree.rootId);
    case 'last': {
      let at = node;
      while (at.children.length > 0) at = nodeAt(tree, at.children[0]!);
      return goTo(tree, at.id);
    }
  }
}

/** A node to append when building a tree from a record (see lib/importGame.ts). */
export type ImportedNode = Omit<GameNode, 'id' | 'parent' | 'children'>;

/** A tree whose main line is `line`, starting from `root`. */
export function treeFromLine(
  names: Seats<string>,
  root: ImportedNode,
  line: ImportedNode[],
): GameTree {
  const nodes: Record<string, GameNode> = {};
  let previous: GameNode = { ...root, id: 'n0', parent: null, children: [] };
  nodes.n0 = previous;
  line.forEach((entry, i) => {
    const id = `n${i + 1}`;
    nodes[previous.id] = { ...previous, children: [id] };
    previous = { ...entry, id, parent: previous.id, children: [] };
    nodes[id] = previous;
  });
  return { nodes, rootId: 'n0', currentId: 'n0', names, nextId: line.length + 1 };
}
