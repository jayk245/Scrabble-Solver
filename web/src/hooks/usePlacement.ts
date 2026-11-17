import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent } from 'react';

import { api } from '../api/client';
import type { BoardRows, MoveInfo, MoveSquare } from '../api/types';
import { BOARD_SIZE, EMPTY, squareAt, type Cursor } from '../lib/board';
import {
  placementNotation,
  placementPreview,
  type PlacedTile,
  type PlacementResult,
} from '../lib/placement';

/** Delay before checking a word being built, so typing stays responsive. */
const VALIDATE_DEBOUNCE_MS = 150;

export type Verdict =
  | { kind: 'none' }
  | { kind: 'checking' }
  | { kind: 'legal'; notation: string; move: MoveInfo }
  | { kind: 'illegal'; message: string };

export interface PlacementOptions {
  board: BoardRows;
  /** The tiles the mover is known to hold, '?' for a blank ('' if unknown). */
  rack: string;
  /**
   * Only tiles from `rack` may be placed (the rack is known in full).
   * Otherwise a letter the rack lacks is placed as a tile from outside it,
   * which is how a move is recorded when the rack is partly or wholly
   * unknown; tile ownership is then not checked.
   */
  strict: boolean;
  /** Whether tiles may be placed at all (e.g. it is the player's turn). */
  enabled: boolean;
  /** Typed letters become blanks without holding Shift. */
  blankMode?: boolean;
  /** Called with the notation when Enter is pressed on a legal move. */
  onSubmit: (notation: string) => void;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/** The square `steps` along the cursor's direction, or null off the board. */
export function stepCursor(cursor: Cursor, steps: number): Cursor | null {
  const row = cursor.direction === 'down' ? cursor.row + steps : cursor.row;
  const col = cursor.direction === 'across' ? cursor.col + steps : cursor.col;
  if (row < 0 || col < 0 || row >= BOARD_SIZE || col >= BOARD_SIZE) return null;
  return { ...cursor, row, col };
}

function flip(cursor: Cursor): Cursor {
  return { ...cursor, direction: cursor.direction === 'across' ? 'down' : 'across' };
}

/**
 * Building a move by placing tiles on the board, shared by the Play tab
 * and the Solver's move entry: a cursor with a direction, typing or
 * clicking rack tiles to lay them down, a picker for a clicked blank, and
 * a live verdict from the engine (/api/validate) with the move's score.
 *
 * The client only works out which move is meant (lib/placement.ts); the
 * engine decides whether it is legal. Each new arrangement is checked
 * after a short debounce, and a superseded check is aborted so a stale
 * answer never wins.
 */
export function usePlacement({
  board,
  rack,
  strict,
  enabled,
  blankMode = false,
  onSubmit,
}: PlacementOptions) {
  const tiles = useMemo(() => [...rack], [rack]);
  const [placed, setPlaced] = useState<PlacedTile[]>([]);
  const [cursor, setCursor] = useState<Cursor | null>(null);
  const [blankIndex, setBlankIndex] = useState<number | null>(null);
  const [verdict, setVerdict] = useState<Verdict>({ kind: 'none' });
  const [notice, setNotice] = useState<string | null>(null);
  // Tiles from outside the rack get negative indices, unique per move.
  const nextFree = useRef(-1);

  // A new position (or a rack edit) starts a fresh move.
  useEffect(() => {
    setPlaced([]);
    setBlankIndex(null);
    setNotice(null);
  }, [board, rack]);

  const used = useMemo(
    () => new Set(placed.filter((tile) => tile.rackIndex >= 0).map((tile) => tile.rackIndex)),
    [placed],
  );
  const placement: PlacementResult = useMemo(
    () => placementNotation(board, placed),
    [board, placed],
  );

  useEffect(() => {
    if (placement.kind === 'none') {
      setVerdict({ kind: 'none' });
      return;
    }
    if (placement.kind === 'error') {
      setVerdict({ kind: 'illegal', message: placement.message });
      return;
    }
    setVerdict({ kind: 'checking' });
    const controller = new AbortController();
    const timer = window.setTimeout(() => {
      api
        .validate(
          { board, rack: strict && rack ? rack : undefined, move: placement.notation },
          controller.signal,
        )
        .then((response) =>
          setVerdict(
            response.valid
              ? { kind: 'legal', notation: placement.notation, move: response.move }
              : { kind: 'illegal', message: response.message },
          ),
        )
        .catch((error: unknown) => {
          if (!controller.signal.aborted)
            setVerdict({ kind: 'illegal', message: errorMessage(error) });
        });
    }, VALIDATE_DEBOUNCE_MS);
    return () => {
      window.clearTimeout(timer);
      controller.abort();
    };
  }, [placement, board, rack, strict]);

  const occupiedAt = useCallback(
    (row: number, col: number) =>
      squareAt(board, row, col) !== EMPTY ||
      placed.some((tile) => tile.row === row && tile.col === col),
    [board, placed],
  );

  /** Places rack tile `index` (as board character `letter`) at the cursor. */
  const placeAt = (index: number, letter: string) => {
    if (!cursor) return;
    let target: Cursor | null = cursor;
    while (target && occupiedAt(target.row, target.col)) target = stepCursor(target, 1);
    if (!target) return;
    const square = target;
    setPlaced((current) => [
      ...current,
      { row: square.row, col: square.col, letter, rackIndex: index },
    ]);
    setCursor(stepCursor(square, 1) ?? square);
    setNotice(null);
  };

  const placeLetter = (letter: string, asBlank: boolean) => {
    const free = (wanted: string) => tiles.findIndex((tile, i) => tile === wanted && !used.has(i));
    const exact = asBlank ? -1 : free(letter);
    if (exact >= 0) {
      placeAt(exact, letter);
      return;
    }
    const blank = free('?');
    if (blank >= 0 && (asBlank || strict)) {
      placeAt(blank, letter.toLowerCase());
      return;
    }
    if (!strict) {
      placeAt(nextFree.current--, asBlank ? letter.toLowerCase() : letter);
      return;
    }
    setNotice(
      asBlank ? 'There is no blank left on the rack.' : `There is no ${letter} on the rack.`,
    );
  };

  const recallAll = useCallback(() => {
    setPlaced([]);
    setBlankIndex(null);
  }, []);

  const selectSquare = useCallback(
    (row: number, col: number) => {
      const tile = placed.find((entry) => entry.row === row && entry.col === col);
      if (tile && enabled) {
        setPlaced((current) => current.filter((entry) => entry !== tile));
        setCursor((current) => ({ row, col, direction: current?.direction ?? 'across' }));
        return;
      }
      setCursor((current) =>
        current && current.row === row && current.col === col
          ? flip(current)
          : { row, col, direction: current?.direction ?? 'across' },
      );
    },
    [placed, enabled],
  );

  /** A click on rack tile `index`: place it at the cursor, or take it back. */
  const clickRackTile = (index: number) => {
    if (!enabled) return;
    if (used.has(index)) {
      const tile = placed.find((entry) => entry.rackIndex === index);
      setPlaced((current) => current.filter((entry) => entry.rackIndex !== index));
      if (tile) {
        setCursor((current) => ({
          row: tile.row,
          col: tile.col,
          direction: current?.direction ?? 'across',
        }));
      }
      return;
    }
    if (!cursor) {
      setNotice('Click a square on the board first, then a tile (or just type).');
      return;
    }
    if (tiles[index] === '?') {
      setBlankIndex(index);
      return;
    }
    placeAt(index, tiles[index] ?? '');
  };

  /** The letter chosen for a clicked blank. */
  const chooseBlank = (letter: string) => {
    if (blankIndex !== null) placeAt(blankIndex, letter.toLowerCase());
    setBlankIndex(null);
  };

  /**
   * Lays a suggested move's tiles on the board for the player to submit.
   * False if the rack cannot supply them.
   */
  const applyMove = (move: MoveInfo): boolean => {
    const taken = new Set<number>();
    const next: PlacedTile[] = [];
    const squares: MoveSquare[] = move.tiles.filter((square) => !square.playThrough);
    for (const square of squares) {
      const wanted = square.blank ? '?' : square.letter;
      let index = tiles.findIndex((tile, i) => tile === wanted && !taken.has(i));
      if (index < 0) {
        if (strict) return false;
        index = nextFree.current--;
      }
      taken.add(index);
      next.push({ row: square.row, col: square.col, letter: square.letter, rackIndex: index });
    }
    setPlaced(next);
    setCursor(null);
    return true;
  };

  const submit = () => {
    if (enabled && verdict.kind === 'legal') onSubmit(verdict.notation);
  };

  /** Keyboard handling for the board while building a move. */
  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.ctrlKey || event.metaKey || event.altKey) return;
    const { key } = event;
    if (key === 'Enter') {
      submit();
    } else if (key === 'Escape') {
      recallAll();
      setCursor(null);
    } else if (!enabled) {
      return;
    } else if (/^[a-zA-Z]$/.test(key) && cursor) {
      placeLetter(key.toUpperCase(), event.shiftKey || blankMode);
    } else if (key === 'Backspace') {
      const last = placed.at(-1);
      if (!last) return;
      setPlaced(placed.slice(0, -1));
      setCursor({ row: last.row, col: last.col, direction: cursor?.direction ?? 'across' });
    } else if (key === ' ' && cursor) {
      setCursor(flip(cursor));
    } else if (key.startsWith('Arrow') && cursor) {
      const across = key === 'ArrowLeft' || key === 'ArrowRight';
      const delta = key === 'ArrowLeft' || key === 'ArrowUp' ? -1 : 1;
      const moved = stepCursor({ ...cursor, direction: across ? 'across' : 'down' }, delta);
      if (moved) setCursor({ ...moved, direction: cursor.direction });
    } else {
      return;
    }
    event.preventDefault();
  };

  /** Tiles placed from outside the rack, as rack letters ('?' for a blank). */
  const outsideTiles = placed
    .filter((tile) => tile.rackIndex < 0)
    .map((tile) => (tile.letter >= 'a' && tile.letter <= 'z' ? '?' : tile.letter))
    .join('');

  return {
    tiles,
    placed,
    used,
    cursor,
    setCursor,
    verdict,
    notice,
    setNotice,
    blankPending: blankIndex !== null,
    cancelBlank: () => setBlankIndex(null),
    chooseBlank,
    clickRackTile,
    selectSquare,
    onKeyDown,
    recallAll,
    applyMove,
    submit,
    outsideTiles,
    preview: placementPreview(placed),
  };
}

export type Placement = ReturnType<typeof usePlacement>;
