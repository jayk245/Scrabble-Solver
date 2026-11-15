import { useMemo, type KeyboardEvent, type Ref } from 'react';

import type { BoardRows, MoveInfo } from '../api/types';
import { BOARD_SIZE, COLUMN_LABELS, squareKey, type Cursor } from '../lib/board';
import { Square, type PreviewRole } from './Square';

export interface BoardProps {
  board: BoardRows;
  premiums: string[];
  tileValues: Record<string, number>;
  /** Move to overlay on the board, e.g. the one hovered in the move list. */
  preview?: MoveInfo | null;
  /** Move to outline as the last one played. */
  recent?: MoveInfo | null;
  cursor?: Cursor | null;
  onSquareSelect?: (row: number, col: number) => void;
  onKeyDown?: (event: KeyboardEvent<HTMLDivElement>) => void;
  boardRef?: Ref<HTMLDivElement>;
}

const NO_OP = () => {};

/**
 * The 15x15 board with coordinate labels. Purely presentational: editing
 * and previews are driven by props, so the same component can render a
 * replayed or streamed game.
 */
export function Board({
  board,
  premiums,
  tileValues,
  preview = null,
  recent = null,
  cursor = null,
  onSquareSelect = NO_OP,
  onKeyDown,
  boardRef,
}: BoardProps) {
  const previewSquares = useMemo(() => {
    const map = new Map<string, { role: PreviewRole; letter: string }>();
    for (const tile of preview?.tiles ?? []) {
      map.set(squareKey(tile.row, tile.col), {
        role: tile.playThrough ? 'through' : 'placed',
        letter: tile.letter,
      });
    }
    return map;
  }, [preview]);

  const recentSquares = useMemo(
    () =>
      new Set(
        (recent?.tiles ?? [])
          .filter((tile) => !tile.playThrough)
          .map((tile) => squareKey(tile.row, tile.col)),
      ),
    [recent],
  );

  const cells = [];
  cells.push(<div key="corner" className="board-label" />);
  for (const label of COLUMN_LABELS) {
    cells.push(
      <div key={`c${label}`} className="board-label">
        {label}
      </div>,
    );
  }
  for (let row = 0; row < BOARD_SIZE; row++) {
    cells.push(
      <div key={`r${row}`} className="board-label">
        {row + 1}
      </div>,
    );
    for (let col = 0; col < BOARD_SIZE; col++) {
      const key = squareKey(row, col);
      const previewed = previewSquares.get(key);
      cells.push(
        <Square
          key={key}
          row={row}
          col={col}
          premium={premiums[row]?.[col] ?? '.'}
          tile={board[row]?.[col] ?? '.'}
          previewTile={previewed?.role === 'placed' ? previewed.letter : null}
          previewRole={previewed?.role ?? null}
          recent={recentSquares.has(key)}
          cursor={cursor && cursor.row === row && cursor.col === col ? cursor.direction : null}
          tileValues={tileValues}
          onSelect={onSquareSelect}
        />,
      );
    }
  }

  return (
    <div
      className="board"
      role="grid"
      aria-label="Scrabble board"
      tabIndex={0}
      onKeyDown={onKeyDown}
      ref={boardRef}
    >
      {cells}
    </div>
  );
}
