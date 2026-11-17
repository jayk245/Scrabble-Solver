import { useReducer } from 'react';

import type { Seats } from '../api/types';
import {
  editRacks,
  editSetup,
  emptyTree,
  goTo,
  promote,
  recordMove,
  removeNode,
  step,
  type GameTree,
  type RackPatch,
  type RecordedMove,
  type SetupPatch,
  type Step,
} from '../lib/gameTree';

export type GameTreeAction =
  /** Edit board mode: changes the position and drops the moves after it. */
  | { type: 'setup'; patch: SetupPatch }
  /** Rack knowledge for the current position; later moves are kept. */
  | { type: 'racks'; patch: RackPatch }
  | { type: 'rename'; names: Seats<string> }
  /** A move the server has validated, from the current position. */
  | { type: 'record'; recorded: RecordedMove }
  | { type: 'goto'; id: string }
  | { type: 'step'; where: Step }
  | { type: 'promote'; id: string }
  /** Removes a node and its subtree (a variation, or the moves from here on). */
  | { type: 'remove'; id: string }
  | { type: 'load'; tree: GameTree }
  | { type: 'reset' };

/** Pure transition function: each action is one function of lib/gameTree. */
export function gameTreeReducer(tree: GameTree, action: GameTreeAction): GameTree {
  switch (action.type) {
    case 'setup':
      return editSetup(tree, action.patch);
    case 'racks':
      return editRacks(tree, action.patch);
    case 'rename':
      return { ...tree, names: action.names };
    case 'record':
      return recordMove(tree, action.recorded);
    case 'goto':
      return goTo(tree, action.id);
    case 'step':
      return step(tree, action.where);
    case 'promote':
      return promote(tree, action.id);
    case 'remove':
      return removeNode(tree, action.id);
    case 'load':
      return action.tree;
    case 'reset':
      return emptyTree(tree.names);
  }
}

/**
 * The solver's move tree as a reducer. Every change is a named action
 * applied by one pure function over immutable nodes, so navigation,
 * branching and promotion never lose information and are easy to test.
 */
export function useGameTree() {
  return useReducer(gameTreeReducer, undefined, () => emptyTree());
}
