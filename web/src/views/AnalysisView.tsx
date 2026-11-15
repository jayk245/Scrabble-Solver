import { useCallback, useEffect, useRef, useState, type KeyboardEvent } from 'react';

import { api } from '../api/client';
import type { BoardRows, MoveInfo, Rules } from '../api/types';
import { Board } from '../components/Board';
import { Legend } from '../components/Legend';
import { MoveChecker } from '../components/MoveChecker';
import { MoveList } from '../components/MoveList';
import { MoveLog } from '../components/MoveLog';
import { RackInput } from '../components/RackInput';
import { SequenceInput } from '../components/SequenceInput';
import { UnseenTiles } from '../components/UnseenTiles';
import { useMoves } from '../hooks/useMoves';
import {
  advance,
  emptyBoard,
  EMPTY,
  setSquare,
  squareAt,
  squareName,
  type Cursor,
} from '../lib/board';
import { addRackToCounts, drawRack } from '../lib/rack';

/** State before a played move, so Undo can restore it exactly. */
interface HistoryEntry {
  board: BoardRows;
  rack: string;
  move: MoveInfo;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function isTextField(target: EventTarget | null): boolean {
  return (
    target instanceof HTMLInputElement ||
    target instanceof HTMLTextAreaElement ||
    target instanceof HTMLSelectElement
  );
}

/**
 * The analysis board: set up a position, enter a rack, and explore every
 * legal move. All engine work happens on the server; this view owns the
 * position and its undo history.
 */
export function AnalysisView({ rules }: { rules: Rules }) {
  const [board, setBoard] = useState<BoardRows>(emptyBoard);
  const [rack, setRackState] = useState('');
  const [history, setHistory] = useState<HistoryEntry[]>([]);
  const [cursor, setCursor] = useState<Cursor | null>(null);
  const [blankMode, setBlankMode] = useState(false);
  const [hovered, setHovered] = useState<MoveInfo | null>(null);
  const [selected, setSelected] = useState<MoveInfo | null>(null);
  const [checked, setChecked] = useState<MoveInfo | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const boardRef = useRef<HTMLDivElement>(null);

  const moves = useMoves(board, rack);
  const preview = hovered ?? selected ?? checked;
  const recent = history.at(-1)?.move ?? null;

  // Any change to the position invalidates move previews and selections.
  const clearPreviews = () => {
    setHovered(null);
    setSelected(null);
    setChecked(null);
  };
  const changeBoard = (next: BoardRows) => {
    setBoard(next);
    clearPreviews();
  };
  const changeRack = (next: string) => {
    setRackState(next);
    clearPreviews();
  };

  const play = async (notation: string, withRack: boolean): Promise<string | null> => {
    try {
      const result = await api.play({
        board,
        rack: withRack && rack ? rack : undefined,
        move: notation,
      });
      setHistory((entries) => [...entries, { board, rack, move: result.move }]);
      changeBoard(result.board);
      if (result.rack !== undefined) changeRack(result.rack);
      setNotice(null);
      return null;
    } catch (error) {
      return errorMessage(error);
    }
  };

  const playFromList = async (move: MoveInfo) => {
    const error = await play(move.notation, true);
    setNotice(error);
  };

  const applySequence = async (notations: string[]) => {
    let current = board;
    const entries: HistoryEntry[] = [];
    let error: string | null = null;
    for (const notation of notations) {
      try {
        const result = await api.play({ board: current, move: notation });
        entries.push({ board: current, rack, move: result.move });
        current = result.board;
      } catch (failure) {
        error = errorMessage(failure);
        break;
      }
    }
    if (entries.length > 0) {
      setHistory((existing) => [...existing, ...entries]);
      changeBoard(current);
    }
    return { applied: entries.length, error };
  };

  const undo = useCallback(() => {
    const last = history.at(-1);
    if (!last) return;
    setHistory(history.slice(0, -1));
    setBoard(last.board);
    setRackState(last.rack);
    setHovered(null);
    setSelected(null);
    setChecked(null);
  }, [history]);

  const clearBoard = () => {
    setHistory([]);
    changeBoard(emptyBoard());
    setCursor(null);
  };

  const randomRack = () => {
    const pool = moves.data
      ? addRackToCounts(moves.data.position.unseen, rack)
      : rules.tileCounts;
    changeRack(drawRack(pool, rules.rackSize));
  };

  // Ctrl/Cmd+Z undoes a played move unless the user is editing text.
  useEffect(() => {
    const onKey = (event: globalThis.KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key === 'z' && !isTextField(event.target)) {
        event.preventDefault();
        undo();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [undo]);

  const selectSquare = useCallback((row: number, col: number) => {
    boardRef.current?.focus();
    setCursor((current) =>
      current && current.row === row && current.col === col
        ? { ...current, direction: current.direction === 'across' ? 'down' : 'across' }
        : { row, col, direction: current?.direction ?? 'across' },
    );
  }, []);

  const onBoardKey = (event: KeyboardEvent<HTMLDivElement>) => {
    if (!cursor || event.ctrlKey || event.metaKey || event.altKey) return;
    const { key } = event;
    if (/^[a-zA-Z]$/.test(key)) {
      const upper = key.toUpperCase();
      const char = event.shiftKey || blankMode ? upper.toLowerCase() : upper;
      changeBoard(setSquare(board, cursor.row, cursor.col, char));
      setCursor(advance(cursor, 1));
    } else if (key === 'Backspace') {
      // On an empty square, step back first so Backspace undoes typing.
      const target =
        squareAt(board, cursor.row, cursor.col) === EMPTY ? advance(cursor, -1) : cursor;
      changeBoard(setSquare(board, target.row, target.col, EMPTY));
      setCursor(target);
    } else if (key === 'Delete') {
      changeBoard(setSquare(board, cursor.row, cursor.col, EMPTY));
    } else if (key === ' ') {
      setCursor({ ...cursor, direction: cursor.direction === 'across' ? 'down' : 'across' });
    } else if (key.startsWith('Arrow')) {
      const across = key === 'ArrowLeft' || key === 'ArrowRight';
      const step = key === 'ArrowLeft' || key === 'ArrowUp' ? -1 : 1;
      const moved = advance({ ...cursor, direction: across ? 'across' : 'down' }, step);
      setCursor({ ...moved, direction: cursor.direction });
    } else if (key === 'Escape') {
      setCursor(null);
    } else {
      return;
    }
    event.preventDefault();
  };

  const boardIsEmpty = board.every((line) => /^\.+$/.test(line));

  return (
    <div className="analysis">
      <div className="board-column">
        <div className="board-toolbar">
          <p className="hint">
            {cursor ? (
              <>
                Typing at <strong>{squareName(cursor.row, cursor.col)}</strong> {cursor.direction}.
                Shift+letter places a blank; Space flips direction; Esc stops.
              </>
            ) : (
              'Click a square and type to place tiles.'
            )}
          </p>
          <button
            type="button"
            className={`button-ghost toggle${blankMode ? ' is-on' : ''}`}
            aria-pressed={blankMode}
            onClick={() => setBlankMode(!blankMode)}
            title="Type blanks without holding Shift"
          >
            Blank mode
          </button>
        </div>
        <Board
          board={board}
          premiums={rules.premiums}
          tileValues={rules.tileValues}
          preview={preview}
          recent={recent}
          cursor={cursor}
          onSquareSelect={selectSquare}
          onKeyDown={onBoardKey}
          boardRef={boardRef}
        />
        <Legend />
        {notice && <p className="verdict is-illegal">{notice}</p>}
      </div>

      <div className="side-column">
        <RackInput
          rack={rack}
          tileValues={rules.tileValues}
          onChange={changeRack}
          onRandom={randomRack}
        />
        <MoveList
          moves={moves.data?.moves ?? []}
          total={moves.data?.total ?? 0}
          truncated={moves.data?.truncated ?? false}
          elapsedMs={moves.data?.elapsedMs ?? null}
          loading={moves.loading}
          error={moves.error}
          selected={selected}
          onHover={setHovered}
          onSelect={setSelected}
          onPlay={playFromList}
        />
        <MoveChecker
          board={board}
          rack={rack}
          onPlay={(notation) => play(notation, true)}
          onPreview={setChecked}
        />
      </div>

      <div className="extras">
        <UnseenTiles position={moves.data?.position ?? null} />
        <MoveLog
          moves={history.map((entry) => entry.move)}
          onUndo={undo}
          onClear={clearBoard}
          canClear={!boardIsEmpty || history.length > 0}
        />
        <SequenceInput onApply={applySequence} />
      </div>
    </div>
  );
}
