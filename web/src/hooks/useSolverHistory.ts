import { useReducer } from 'react';

import type { AnalyzeResponse, BoardRows, MoveInfo, Seats } from '../api/types';
import { emptyBoard } from '../lib/board';

/** How a position was reached: the previous mover and their move. */
export interface PositionOrigin {
  player: 0 | 1;
  /** The mover's rack before the move ('' if it was not entered). */
  rack: string;
  move: MoveInfo;
}

/** One position of a game being solved move by move. */
export interface SolverPosition {
  board: BoardRows;
  names: Seats<string>;
  scores: Seats<number>;
  toMove: 0 | 1;
  /**
   * What is known of each rack. The mover's rack is what gets solved; the
   * other is used only when `opponentKnown` is set, and otherwise kept as a
   * starting point for that player's next turn (their leave).
   */
  racks: Seats<string>;
  opponentKnown: boolean;
  /** Consecutive scoreless turns leading here (0-5). */
  scoreless: number;
  origin: PositionOrigin | null;
  /** The engine's answer for this exact position, once solved. */
  analysis: AnalyzeResponse | null;
}

export interface SolverHistory {
  positions: SolverPosition[];
  /** The position shown; earlier ones are reachable with Back. */
  index: number;
}

export type SolverAction =
  /** Changes the current position. Later positions no longer follow from it, so they are dropped. */
  | { type: 'edit'; patch: Partial<Omit<SolverPosition, 'origin' | 'analysis'>> }
  /** Player names apply to the whole game, so they change every position. */
  | { type: 'rename'; names: Seats<string> }
  /**
   * Records the engine's answer for `position`, if it is still the
   * position at `index` (an edit while solving makes the answer stale).
   */
  | { type: 'solved'; index: number; position: SolverPosition; analysis: AnalyzeResponse }
  /** Applies a move the server has validated: `board` is the board after it. */
  | { type: 'play'; move: MoveInfo; board: BoardRows; leave: string }
  | { type: 'goto'; index: number }
  /** Steps back and forgets the move undone. */
  | { type: 'undo' }
  | { type: 'reset' };

export function initialPosition(): SolverPosition {
  return {
    board: emptyBoard(),
    names: ['player1', 'player2'],
    scores: [0, 0],
    toMove: 0,
    racks: ['', ''],
    opponentKnown: false,
    scoreless: 0,
    origin: null,
    analysis: null,
  };
}

function other(seat: 0 | 1): 0 | 1 {
  return seat === 0 ? 1 : 0;
}

/** Pure transition function; exported for clarity and testing. */
export function solverReducer(state: SolverHistory, action: SolverAction): SolverHistory {
  const current = state.positions[state.index] ?? initialPosition();
  switch (action.type) {
    case 'edit': {
      const next: SolverPosition = { ...current, ...action.patch, analysis: null };
      return {
        positions: [...state.positions.slice(0, state.index), next],
        index: state.index,
      };
    }
    case 'rename':
      return {
        ...state,
        positions: state.positions.map((position) => ({ ...position, names: action.names })),
      };
    case 'solved': {
      if (state.positions[action.index] !== action.position) return state;
      const positions = state.positions.map((position, i) =>
        i === action.index ? { ...position, analysis: action.analysis } : position,
      );
      return { ...state, positions };
    }
    case 'play': {
      const mover = current.toMove;
      const scores = [...current.scores] as Seats<number>;
      scores[mover] += action.move.score;
      const racks = [...current.racks] as Seats<string>;
      racks[mover] = action.leave;
      const next: SolverPosition = {
        board: action.board,
        names: current.names,
        scores,
        toMove: other(mover),
        racks,
        // The previous mover's rack is now their leave plus unknown draws.
        opponentKnown: false,
        scoreless: action.move.score === 0 ? Math.min(current.scoreless + 1, 6) : 0,
        origin: { player: mover, rack: current.racks[mover], move: action.move },
        analysis: null,
      };
      return {
        positions: [...state.positions.slice(0, state.index + 1), next],
        index: state.index + 1,
      };
    }
    case 'goto':
      return {
        ...state,
        index: Math.max(0, Math.min(state.positions.length - 1, action.index)),
      };
    case 'undo':
      if (state.index === 0) return state;
      return { positions: state.positions.slice(0, state.index), index: state.index - 1 };
    case 'reset':
      return { positions: [initialPosition()], index: 0 };
  }
}

/**
 * The solver's position history as a reducer: every change is a named
 * action applied by one pure function, so Back/Forward/Undo are just index
 * moves over immutable snapshots.
 */
export function useSolverHistory() {
  return useReducer(solverReducer, undefined, () => ({
    positions: [initialPosition()],
    index: 0,
  }));
}
