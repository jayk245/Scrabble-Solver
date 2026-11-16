import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent } from 'react';

import { api } from '../api/client';
import type { Candidate, Effort, MoveInfo, Rules } from '../api/types';
import { Board } from '../components/Board';
import { Legend } from '../components/Legend';
import { MoveChecker } from '../components/MoveChecker';
import { MoveList } from '../components/MoveList';
import { PositionPanel } from '../components/PositionPanel';
import { RackInput } from '../components/RackInput';
import { SequenceInput } from '../components/SequenceInput';
import { SolverHistoryPanel } from '../components/SolverHistoryPanel';
import { SolverResults } from '../components/SolverResults';
import { UnseenTiles } from '../components/UnseenTiles';
import { useMoves } from '../hooks/useMoves';
import { useSolverHistory } from '../hooks/useSolverHistory';
import { advance, EMPTY, setSquare, squareAt, squareName, type Cursor } from '../lib/board';
import { downloadText, toGcg } from '../lib/gcg';
import { drawRack, RACK_SIZE, sortRack } from '../lib/rack';
import { boardCounts, unseenSummary } from '../lib/tiles';

/** Waits this long after the last edit before auto-solving. */
const AUTO_SOLVE_DELAY_MS = 500;
const TOTAL_TILES = 100;
const SCORELESS_TURNS_TO_END = 6;

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
 * The position solver: set up a game between two players, ask the engine
 * for the best move, play it (or any other), enter the next rack, repeat.
 *
 * The whole game lives in one reducer (useSolverHistory) as a list of
 * immutable positions; the server is stateless here. Each request carries
 * the full position, and moves go through /api/play so the engine
 * validates and scores them.
 */
export function SolverView({ rules }: { rules: Rules }) {
  const [history, dispatch] = useSolverHistory();
  const position = history.positions[history.index]!;
  const { board, names, scores, toMove, racks, opponentKnown, analysis } = position;
  const opponent: 0 | 1 = toMove === 0 ? 1 : 0;
  const rack = racks[toMove];

  const [cursor, setCursor] = useState<Cursor | null>(null);
  const [blankMode, setBlankMode] = useState(false);
  const [hovered, setHovered] = useState<MoveInfo | null>(null);
  const [hoveredCandidate, setHoveredCandidate] = useState<Candidate | null>(null);
  const [selected, setSelected] = useState<MoveInfo | null>(null);
  const [checked, setChecked] = useState<MoveInfo | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [effort, setEffort] = useState<Effort>('normal');
  const [autoSolve, setAutoSolve] = useState(false);
  const [solving, setSolving] = useState(false);
  const [solveError, setSolveError] = useState<string | null>(null);
  const [playing, setPlaying] = useState(false);
  const boardRef = useRef<HTMLDivElement>(null);
  const solveController = useRef<AbortController | null>(null);

  const moves = useMoves(board, rack);
  const unseen = useMemo(
    () =>
      unseenSummary(board, rack, rules.tileCounts, {
        opponentRack: opponentKnown ? racks[opponent] : undefined,
      }),
    [board, rack, racks, opponent, opponentKnown, rules.tileCounts],
  );

  // A previous mover who emptied their rack with at most a rack's worth of
  // tiles off the board went out (the bag must be empty).
  const tilesOffBoard =
    TOTAL_TILES - Object.values(boardCounts(board)).reduce((sum, n) => sum + n, 0);
  const origin = position.origin;
  const wentOut =
    origin !== null &&
    origin.rack !== '' &&
    origin.move.type === 'place' &&
    racks[origin.player] === '' &&
    tilesOffBoard <= RACK_SIZE;
  const gameOver = wentOut || position.scoreless >= SCORELESS_TURNS_TO_END;

  const blocked = gameOver
    ? 'The game is over.'
    : rack.length === 0
      ? `Enter ${names[toMove] || 'the mover'}'s rack to solve.`
      : unseen.overused
        ? `The position uses more ${[...unseen.overused].join(', ')} than the bag holds.`
        : null;

  // Previews and in-flight solves belong to one position.
  useEffect(() => {
    setHovered(null);
    setHoveredCandidate(null);
    setSelected(null);
    setChecked(null);
    setSolveError(null);
    return () => solveController.current?.abort();
  }, [position]);

  const solve = useCallback(() => {
    if (blocked) return;
    solveController.current?.abort();
    const controller = new AbortController();
    solveController.current = controller;
    const index = history.index;
    setSolving(true);
    setSolveError(null);
    api
      .analyze(
        {
          board,
          rack,
          opponentRack: opponentKnown ? racks[opponent] : undefined,
          score: scores[toMove],
          opponentScore: scores[opponent],
          scorelessTurns: Math.min(position.scoreless, SCORELESS_TURNS_TO_END - 1),
          effort,
        },
        controller.signal,
      )
      .then((result) => dispatch({ type: 'solved', index, position, analysis: result }))
      .catch((error: unknown) => {
        if (!controller.signal.aborted) setSolveError(errorMessage(error));
      })
      .finally(() => {
        if (solveController.current === controller) setSolving(false);
      });
  }, [blocked, board, rack, racks, opponent, opponentKnown, scores, toMove, position, effort, history.index, dispatch]);

  // Auto-solve once the mover holds a full rack (or every tile left).
  const rackComplete = rack.length >= RACK_SIZE || unseen.bagCount === 0;
  useEffect(() => {
    if (!autoSolve || blocked || analysis || !rackComplete) return;
    const timer = window.setTimeout(solve, AUTO_SOLVE_DELAY_MS);
    return () => window.clearTimeout(timer);
  }, [autoSolve, blocked, analysis, rackComplete, solve]);

  const playNotation = async (notation: string): Promise<string | null> => {
    setPlaying(true);
    try {
      const result = await api.play({ board, rack: rack || undefined, move: notation });
      dispatch({ type: 'play', move: result.move, board: result.board, leave: result.rack ?? '' });
      setNotice(null);
      setCursor(null);
      return null;
    } catch (error) {
      return errorMessage(error);
    } finally {
      setPlaying(false);
    }
  };

  const playCandidate = async (candidate: Candidate) => {
    setNotice(await playNotation(candidate.move.notation));
  };

  const applySequence = async (notations: string[]) => {
    let current = board;
    let applied = 0;
    for (const notation of notations) {
      try {
        const result = await api.play({ board: current, move: notation });
        dispatch({ type: 'play', move: result.move, board: result.board, leave: '' });
        current = result.board;
        applied++;
      } catch (failure) {
        return { applied, error: errorMessage(failure) };
      }
    }
    return { applied, error: null };
  };

  const setRack = (next: string) => {
    const updated = toMove === 0 ? ([next, racks[1]] as const) : ([racks[0], next] as const);
    dispatch({ type: 'edit', patch: { racks: [updated[0], updated[1]] } });
  };

  // Tops the rack up from the unseen pool (or deals a fresh one when full),
  // standing in for the tiles the player really drew.
  const fillRack = () => {
    const keep = rack.length >= RACK_SIZE ? '' : rack;
    // The other player's stored tiles are excluded too: they are on a rack,
    // not in the bag.
    const pool = unseenSummary(board, keep, rules.tileCounts, {
      opponentRack: racks[opponent],
    }).unseen;
    setRack(sortRack(keep + drawRack(pool, RACK_SIZE - keep.length)));
  };

  const exportGcg = () => {
    const record = history.positions.flatMap((entry) =>
      entry.origin
        ? [
            {
              player: entry.origin.player,
              rack: entry.origin.rack,
              move: entry.origin.move,
              cumulative: entry.scores[entry.origin.player],
            },
          ]
        : [],
    );
    const start = history.positions[0]!;
    const text = toGcg(names, record, {
      title: 'Solver session',
      startScores: start.scores,
      customStart: start.board.some((line) => /[^.]/.test(line)),
    });
    downloadText('solver-game.gcg', text);
  };

  // Ctrl/Cmd+Z undoes the last move; Alt+arrows walk the history.
  useEffect(() => {
    const onKey = (event: globalThis.KeyboardEvent) => {
      if (isTextField(event.target)) return;
      if ((event.ctrlKey || event.metaKey) && event.key === 'z') {
        event.preventDefault();
        dispatch({ type: 'undo' });
      } else if (event.altKey && event.key === 'ArrowLeft') {
        event.preventDefault();
        dispatch({ type: 'goto', index: history.index - 1 });
      } else if (event.altKey && event.key === 'ArrowRight') {
        event.preventDefault();
        dispatch({ type: 'goto', index: history.index + 1 });
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [dispatch, history.index]);

  const selectSquare = useCallback((row: number, col: number) => {
    boardRef.current?.focus();
    setCursor((current) =>
      current && current.row === row && current.col === col
        ? { ...current, direction: current.direction === 'across' ? 'down' : 'across' }
        : { row, col, direction: current?.direction ?? 'across' },
    );
  }, []);

  const editBoard = (row: number, col: number, char: string) =>
    dispatch({ type: 'edit', patch: { board: setSquare(board, row, col, char) } });

  const onBoardKey = (event: KeyboardEvent<HTMLDivElement>) => {
    if (!cursor || event.ctrlKey || event.metaKey || event.altKey) return;
    const { key } = event;
    if (/^[a-zA-Z]$/.test(key)) {
      const upper = key.toUpperCase();
      editBoard(cursor.row, cursor.col, event.shiftKey || blankMode ? upper.toLowerCase() : upper);
      setCursor(advance(cursor, 1));
    } else if (key === 'Backspace') {
      // On an empty square, step back first so Backspace undoes typing.
      const target =
        squareAt(board, cursor.row, cursor.col) === EMPTY ? advance(cursor, -1) : cursor;
      editBoard(target.row, target.col, EMPTY);
      setCursor(target);
    } else if (key === 'Delete') {
      editBoard(cursor.row, cursor.col, EMPTY);
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

  const preview = hoveredCandidate?.move ?? hovered ?? selected ?? checked;
  const moverName = names[toMove] || `Player ${toMove + 1}`;
  const short = rack.length < RACK_SIZE && unseen.bagCount > 0 && !gameOver;

  return (
    <div className="analysis solver">
      <div className="board-column">
        <div className="board-toolbar">
          <p className="hint">
            {cursor ? (
              <>
                Typing at <strong>{squareName(cursor.row, cursor.col)}</strong> {cursor.direction}.
                Shift+letter places a blank; Space flips direction; Esc stops.
              </>
            ) : (
              'Click a square and type to set up the board.'
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
          recent={origin?.move ?? null}
          cursor={cursor}
          onSquareSelect={selectSquare}
          onKeyDown={onBoardKey}
          boardRef={boardRef}
        />
        <Legend />
        {gameOver && (
          <p className="verdict is-legal end-banner">
            {wentOut && origin
              ? `${names[origin.player]} went out. Game over.`
              : 'Six scoreless turns in a row. Game over.'}{' '}
            Final rack adjustments are not applied here.
          </p>
        )}
        {notice && <p className="verdict is-illegal">{notice}</p>}
      </div>

      <div className="side-column">
        <PositionPanel
          position={position}
          onRename={(next) => dispatch({ type: 'rename', names: next })}
          onEdit={(patch) => dispatch({ type: 'edit', patch })}
        />
        <RackInput
          rack={rack}
          tileValues={rules.tileValues}
          onChange={setRack}
          onRandom={fillRack}
          title={`${moverName}'s rack`}
          randomLabel={rack.length > 0 && rack.length < RACK_SIZE ? 'Fill randomly' : 'Random'}
          note={
            short
              ? `${moverName} holds ${rack.length} tile${rack.length === 1 ? '' : 's'}. Type the tiles drawn, or fill randomly from the unseen pool for a quick demo.`
              : 'Letters, or ? for a blank.'
          }
        />
        <SolverResults
          moverName={moverName}
          analysis={analysis}
          loading={solving}
          error={solveError}
          effort={effort}
          onEffort={setEffort}
          blocked={blocked}
          onSolve={solve}
          autoSolve={autoSolve}
          onAutoSolve={setAutoSolve}
          hovered={hoveredCandidate}
          onHover={setHoveredCandidate}
          onPlay={playCandidate}
          playing={playing}
        />
        <MoveChecker
          board={board}
          rack={rack}
          onPlay={playNotation}
          onPreview={setChecked}
        />
      </div>

      <div className="extras">
        <SolverHistoryPanel
          positions={history.positions}
          index={history.index}
          onGoto={(index) => dispatch({ type: 'goto', index })}
          onUndo={() => dispatch({ type: 'undo' })}
          onClear={() => {
            dispatch({ type: 'reset' });
            setCursor(null);
          }}
          onExport={exportGcg}
        />
        <UnseenTiles
          position={unseen}
          note={
            opponentKnown
              ? `Excludes ${names[opponent]}'s known rack, so these are the bag.`
              : `Bag plus ${names[opponent] || 'the opponent'}'s rack, from ${moverName}'s side.`
          }
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
          onPlay={(move) => void playCandidate({ move, equity: 0, staticRank: null, chosen: false })}
        />
        <SequenceInput onApply={applySequence} />
      </div>
    </div>
  );
}
