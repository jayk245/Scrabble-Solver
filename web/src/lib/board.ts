// Pure helpers for the board-string format (see BoardRows in api/types.ts).

import type { BoardRows, Direction } from '../api/types';

export const BOARD_SIZE = 15;
export const EMPTY = '.';

export const COLUMN_LABELS = Array.from({ length: BOARD_SIZE }, (_, i) =>
  String.fromCharCode(65 + i),
);

export function emptyBoard(): BoardRows {
  return Array.from({ length: BOARD_SIZE }, () => EMPTY.repeat(BOARD_SIZE));
}

/** Returns a copy of `board` with one square replaced by `char`. */
export function setSquare(board: BoardRows, row: number, col: number, char: string): BoardRows {
  return board.map((line, r) =>
    r === row ? line.slice(0, col) + char + line.slice(col + 1) : line,
  );
}

export function squareAt(board: BoardRows, row: number, col: number): string {
  return board[row]?.[col] ?? EMPTY;
}

export function isBlankTile(char: string): boolean {
  return char >= 'a' && char <= 'z';
}

/** Human-facing coordinate, e.g. "H8". */
export function squareName(row: number, col: number): string {
  return `${COLUMN_LABELS[col]}${row + 1}`;
}

/** Key for a square in sets and maps. */
export function squareKey(row: number, col: number): string {
  return `${row},${col}`;
}

export interface Cursor {
  row: number;
  col: number;
  direction: Direction;
}

/** The cursor moved `step` squares along its direction, clamped to the board. */
export function advance(cursor: Cursor, step: number): Cursor {
  const clamp = (n: number) => Math.min(BOARD_SIZE - 1, Math.max(0, n));
  return cursor.direction === 'across'
    ? { ...cursor, col: clamp(cursor.col + step) }
    : { ...cursor, row: clamp(cursor.row + step) };
}

/**
 * Splits pasted text into move notations: one per line or separated by
 * semicolons. Blank lines and '#' comments are ignored, and a leading turn
 * number ("1." or "12)") is stripped so lists copied from annotated games
 * work as-is.
 */
export function parseMoveList(text: string): string[] {
  return text
    .split(/[\n;]/)
    .map((line) => line.replace(/#.*$/, '').trim())
    .map((line) => line.replace(/^\d+[.)]\s*/, ''))
    .filter((line) => line.length > 0);
}
