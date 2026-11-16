// Turning tiles dragged onto the board into move notation.
//
// The client only works out *which* move the player means; whether it is
// legal (words, connection, centre square) and what it scores is always
// decided by the engine via /api/validate and, on submit, by the game
// server itself.

import type { BoardRows, Direction, MoveInfo } from '../api/types';
import { BOARD_SIZE, COLUMN_LABELS, EMPTY, squareAt } from './board';

/** A tile placed on the board but not yet submitted. */
export interface PlacedTile {
  row: number;
  col: number;
  /** Board character: 'A'-'Z', or 'a'-'z' for a blank. */
  letter: string;
  /** Index of the tile in the rack it came from. */
  rackIndex: number;
}

export type PlacementResult =
  | { kind: 'none' }
  | { kind: 'error'; message: string }
  | { kind: 'move'; notation: string; direction: Direction };

function occupied(board: BoardRows, row: number, col: number): boolean {
  return row >= 0 && col >= 0 && row < BOARD_SIZE && col < BOARD_SIZE &&
    squareAt(board, row, col) !== EMPTY;
}

/** Coordinate in notation: "8D" across (row first), "D8" down. */
export function coordinate(row: number, col: number, direction: Direction): string {
  const column = COLUMN_LABELS[col] ?? '?';
  return direction === 'across' ? `${row + 1}${column}` : `${column}${row + 1}`;
}

/**
 * The notation for the tiles in `placed`, e.g. "8D QU(I)T", where letters
 * in parentheses are already on the board. A single tile is read in the
 * direction in which it touches a board tile.
 */
export function placementNotation(board: BoardRows, placed: PlacedTile[]): PlacementResult {
  if (placed.length === 0) return { kind: 'none' };
  const first = placed[0];
  const sameRow = placed.every((tile) => tile.row === first.row);
  const sameCol = placed.every((tile) => tile.col === first.col);
  if (!sameRow && !sameCol) {
    return { kind: 'error', message: 'Place all tiles in one row or one column.' };
  }

  let direction: Direction;
  if (placed.length > 1) {
    direction = sameRow ? 'across' : 'down';
  } else {
    const horizontal =
      occupied(board, first.row, first.col - 1) || occupied(board, first.row, first.col + 1);
    const vertical =
      occupied(board, first.row - 1, first.col) || occupied(board, first.row + 1, first.col);
    direction = vertical && !horizontal ? 'down' : 'across';
  }

  const across = direction === 'across';
  const at = (i: number) => (across ? { row: first.row, col: i } : { row: i, col: first.col });
  const positions = placed.map((tile) => (across ? tile.col : tile.row));
  let start = Math.min(...positions);
  let end = Math.max(...positions);
  // The word runs on through any tiles already on the board at either end.
  while (start > 0 && occupied(board, at(start - 1).row, at(start - 1).col)) start--;
  while (end < BOARD_SIZE - 1 && occupied(board, at(end + 1).row, at(end + 1).col)) end++;

  const byPosition = new Map(placed.map((tile) => [across ? tile.col : tile.row, tile]));
  let word = '';
  let inBoardRun = false;
  for (let i = start; i <= end; i++) {
    const { row, col } = at(i);
    const tile = byPosition.get(i);
    if (tile) {
      if (inBoardRun) word += ')';
      inBoardRun = false;
      word += tile.letter;
    } else if (occupied(board, row, col)) {
      if (!inBoardRun) word += '(';
      inBoardRun = true;
      word += squareAt(board, row, col).toUpperCase();
    } else {
      return { kind: 'error', message: 'Leave no gaps between your tiles.' };
    }
  }
  if (inBoardRun) word += ')';
  const origin = at(start);
  return {
    kind: 'move',
    notation: `${coordinate(origin.row, origin.col, direction)} ${word}`,
    direction,
  };
}

/** Draws placed tiles on the board as a move preview. */
export function placementPreview(placed: PlacedTile[]): MoveInfo | null {
  if (placed.length === 0) return null;
  return {
    notation: '',
    type: 'place',
    score: 0,
    tilesUsed: placed.length,
    bingo: false,
    word: '',
    tiles: placed.map((tile) => ({
      row: tile.row,
      col: tile.col,
      letter: tile.letter,
      blank: tile.letter >= 'a' && tile.letter <= 'z',
      playThrough: false,
    })),
  };
}
