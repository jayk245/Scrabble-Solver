// Client-side tile accounting. The server does the same in
// ComputeUnseen() (apps/server/codec.cc); doing it here keeps the unseen
// panel instant while the user types, with no request per keystroke.

import type { BoardRows, PerLetter, PositionSummary } from '../api/types';
import { isBlankTile } from './board';
import { RACK_SIZE } from './rack';

export const TILE_LETTERS = [...'ABCDEFGHIJKLMNOPQRSTUVWXYZ?'];

/** Tiles on the board; a blank counts as '?', not as its letter. */
export function boardCounts(board: BoardRows): PerLetter {
  const counts: PerLetter = {};
  for (const line of board) {
    for (const char of line) {
      if (char === '.') continue;
      const key = isBlankTile(char) ? '?' : char;
      counts[key] = (counts[key] ?? 0) + 1;
    }
  }
  return counts;
}

export interface UnseenOptions {
  /** The bag size when known exactly (a live game); otherwise inferred. */
  bag?: number;
  /** Tiles of the hidden rack, if it is known (the solver's opponent rack). */
  opponentRack?: string;
}

/**
 * Tiles not visible to a player holding `rack`: the distribution minus the
 * board, the rack and (if known) the opponent's rack. Negative counts are
 * clamped and the letter reported in `overused`, as the server does.
 */
export function unseenSummary(
  board: BoardRows,
  rack: string,
  tileCounts: PerLetter,
  options: UnseenOptions = {},
): PositionSummary {
  const used = boardCounts(board);
  const tilesOnBoard = Object.values(used).reduce((sum, n) => sum + n, 0);
  for (const tile of rack + (options.opponentRack ?? '')) used[tile] = (used[tile] ?? 0) + 1;

  const unseen: PerLetter = {};
  let unseenCount = 0;
  let overused = '';
  for (const letter of TILE_LETTERS) {
    const remaining = (tileCounts[letter] ?? 0) - (used[letter] ?? 0);
    if (remaining < 0) overused += letter;
    unseen[letter] = Math.max(0, remaining);
    unseenCount += unseen[letter];
  }
  const hiddenRack =
    options.opponentRack !== undefined ? 0 : Math.min(RACK_SIZE, unseenCount);
  const bagCount = options.bag ?? Math.max(0, unseenCount - hiddenRack);
  return { unseen, unseenCount, bagCount, tilesOnBoard, overused };
}
