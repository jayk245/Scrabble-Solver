import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent } from 'react';

import { api } from '../api/client';
import type {
  AnalyzeRequest,
  AnalyzeResponse,
  Candidate,
  Effort,
  GameMoveEvent,
  GameStartEvent,
  MoveInfo,
  Rules,
} from '../api/types';
import { Board } from '../components/Board';
import { GameReviewPanel } from '../components/GameReviewPanel';
import { ImportPanel } from '../components/ImportPanel';
import { Legend } from '../components/Legend';
import { MoveChecker } from '../components/MoveChecker';
import { MoveList } from '../components/MoveList';
import { MoveTree } from '../components/MoveTree';
import { PlacementVerdict } from '../components/PlacementVerdict';
import { PlayerRack } from '../components/PlayerRack';
import { PositionPanel } from '../components/PositionPanel';
import { Scoreboard } from '../components/Scoreboard';
import { SolverResults } from '../components/SolverResults';
import { UnseenTiles } from '../components/UnseenTiles';
import { useGameReview } from '../hooks/useGameReview';
import { useGameTree } from '../hooks/useGameTree';
import { useMoves } from '../hooks/useMoves';
import { usePlacement } from '../hooks/usePlacement';
import { advance, EMPTY, setSquare, squareAt, squareName } from '../lib/board';
import {
  analyzeRequest,
  lookupAnalysis,
  rackComplete,
  storeAnalysis,
  type AnalysisCache,
} from '../lib/engine';
import {
  currentNode,
  descendantCount,
  isOnMainLine,
  mainLine,
  nodeAt,
  other,
  plyOf,
  rackBefore,
  SCORELESS_TURNS_TO_END,
  setSeat,
  type GameTree,
} from '../lib/gameTree';
import { downloadText, tilesPlayed, toGcg } from '../lib/gcg';
import { treeFromEvents } from '../lib/importGame';
import { accountTiles, drawRack, normaliseRack, RACK_SIZE, sortRack } from '../lib/rack';
import { GRADE_LABEL, reviewMainLine, wentOut, type MoveGrade } from '../lib/review';
import { unseenSummary } from '../lib/tiles';

/** Waits this long on a position before the engine starts on it. */
const ENGINE_DELAY_MS = 300;
/** The bag must hold a full rack for an exchange. */
const MIN_BAG_FOR_EXCHANGE = 7;

/** A game handed over by another tab ("Open in Solver"). */
export type SolverImport =
  { id: number; gcg: string } | { id: number; start: GameStartEvent; moves: GameMoveEvent[] };

type Mode = 'moves' | 'edit';

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

function hasMoves(tree: GameTree): boolean {
  return Object.keys(tree.nodes).length > 1;
}

/**
 * The analysis board: enter or import a game, step through it, branch
 * into variations, and ask the engine for the best move anywhere, the way
 * a chess analysis board works.
 *
 * State is split by lifetime. The move tree (useGameTree) is the game
 * itself; the analysis cache (lib/engine.ts) maps positions to the
 * engine's answers and outlives edits; everything else is transient UI.
 * The server stays stateless: each request carries the full position,
 * and moves go through /api/play so the engine validates and scores them.
 */
export function SolverView({
  rules,
  incoming,
}: {
  rules: Rules;
  /** A game to load, from the Play tab or the Game viewer. */
  incoming: SolverImport | null;
}) {
  const [tree, dispatch] = useGameTree();
  const node = currentNode(tree);
  const { board, scores, toMove, racks, opponentKnown } = node;
  const names = tree.names;
  const opponent = other(toMove);
  const rack = racks[toMove];
  const moverName = names[toMove] || `Player ${toMove + 1}`;

  const [mode, setMode] = useState<Mode>('moves');
  const [showRacks, setShowRacks] = useState(true);
  const [blankMode, setBlankMode] = useState(false);
  const [hovered, setHovered] = useState<MoveInfo | null>(null);
  const [hoveredCandidate, setHoveredCandidate] = useState<Candidate | null>(null);
  const [selected, setSelected] = useState<MoveInfo | null>(null);
  const [checked, setChecked] = useState<MoveInfo | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [effort, setEffort] = useState<Effort>('normal');
  const [engineOn, setEngineOn] = useState(false);
  const [playing, setPlaying] = useState(false);
  const [exchange, setExchange] = useState<Set<number> | null>(null);
  const [exchangeText, setExchangeText] = useState('');
  const boardRef = useRef<HTMLDivElement>(null);

  // ---- Position facts ---------------------------------------------------

  const unseen = useMemo(
    () =>
      unseenSummary(board, rack, rules.tileCounts, {
        opponentRack: opponentKnown ? racks[opponent] : undefined,
      }),
    [board, rack, racks, opponent, opponentKnown, rules.tileCounts],
  );
  const complete = rackComplete(node, rules.tileCounts);
  const ended = wentOut(tree, node) || node.scoreless >= SCORELESS_TURNS_TO_END;
  const mainChild = node.children[0] ? nodeAt(tree, node.children[0]) : null;
  const playedHere =
    mainChild?.origin?.kind === 'play' ? mainChild.origin.move.notation : undefined;

  // ---- Engine: cached answers per position ------------------------------

  const [cache, setCache] = useState<AnalysisCache>(() => new Map());
  const store = useCallback(
    (request: AnalyzeRequest, result: AnalyzeResponse) =>
      setCache((current) => storeAnalysis(current, request, result)),
    [],
  );
  const request = useMemo(
    () => ({ ...analyzeRequest(node, effort), played: playedHere }),
    [node, effort, playedHere],
  );
  const requestKey = JSON.stringify(request);
  const cached = lookupAnalysis(cache, request);
  const analysis = cached?.result ?? null;

  const [solvingKey, setSolvingKey] = useState<string | null>(null);
  const [solveError, setSolveError] = useState<{ key: string; message: string } | null>(null);
  const solveController = useRef<AbortController | null>(null);
  const review = useGameReview(tree, cache, store, rules.tileCounts);

  const blocked = ended
    ? 'The game is over.'
    : rack.length === 0
      ? `Enter ${moverName}'s rack to solve.`
      : unseen.overused
        ? `The position uses more ${[...unseen.overused].join(', ')} than the bag holds.`
        : review.running
          ? 'The game review is using the engine.'
          : null;

  const solve = useCallback(() => {
    if (blocked) return;
    solveController.current?.abort();
    const controller = new AbortController();
    solveController.current = controller;
    setSolvingKey(requestKey);
    setSolveError(null);
    api
      .analyze(request, controller.signal)
      .then((result) => store(request, result))
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setSolveError({ key: requestKey, message: errorMessage(error) });
        }
      })
      .finally(() => {
        if (solveController.current === controller) {
          solveController.current = null;
          setSolvingKey(null);
        }
      });
  }, [blocked, request, requestKey, store]);

  // Leaving a position cancels its search: the fetch is aborted, which
  // closes the connection, and the server drops the search if it is still
  // queued, so the next position is not kept waiting.
  useEffect(
    () => () => {
      solveController.current?.abort();
      solveController.current = null;
      setSolvingKey(null);
    },
    [requestKey],
  );

  // Engine on: solve each position with a complete rack once the user
  // settles on it, unless it is cached or just failed.
  const failedHere = solveError?.key === requestKey;
  useEffect(() => {
    if (!engineOn || blocked || !complete || cached || failedHere) return;
    const timer = window.setTimeout(solve, ENGINE_DELAY_MS);
    return () => window.clearTimeout(timer);
  }, [engineOn, blocked, complete, cached, failedHere, solve]);

  // ---- Review ----------------------------------------------------------

  const reviewed = useMemo(
    () => reviewMainLine(tree, cache, rules.tileCounts, rules.tileValues),
    [tree, cache, rules.tileCounts, rules.tileValues],
  );
  const grades = useMemo(() => {
    const map = new Map<string, MoveGrade>();
    for (const move of reviewed) if (move.grade) map.set(move.nodeId, move.grade);
    return map;
  }, [reviewed]);
  const currentGrade = grades.get(node.id);

  // ---- Recording moves -------------------------------------------------

  /**
   * Plays `notation` from the current position through /api/play and
   * records it. With the rack known in full the server checks the tiles
   * come from it; otherwise only the board and the words are checked, and
   * the tiles the move used outside the known part of the rack are added
   * to it, since the player evidently held them.
   */
  const playNotation = async (notation: string): Promise<string | null> => {
    if (ended) return 'The game is over.';
    setPlaying(true);
    try {
      const strict = complete;
      const result = await api.play({ board, rack: strict ? rack : undefined, move: notation });
      const known = strict
        ? { rackBefore: rack, leave: result.rack ?? '' }
        : accountTiles(rack, tilesPlayed(result.move));
      dispatch({
        type: 'record',
        recorded: { move: result.move, board: result.board, ...known },
      });
      setNotice(null);
      setExchange(null);
      setExchangeText('');
      return null;
    } catch (error) {
      return errorMessage(error);
    } finally {
      setPlaying(false);
    }
  };

  const placement = usePlacement({
    board,
    rack,
    strict: complete,
    enabled: mode === 'moves' && !ended && !playing && exchange === null,
    blankMode,
    onSubmit: (notation) => {
      void playNotation(notation).then(setNotice);
    },
  });
  const { cursor, setCursor } = placement;

  const playCandidate = async (candidate: Candidate) => {
    setNotice(await playNotation(candidate.move.notation));
  };

  // Pasted moves are played one after another from the current position.
  // This render's `node` does not change during the loop, so the loop
  // tracks the position itself; the reducer applies each recorded move to
  // the position the previous one produced.
  const applySequence = async (notations: string[]) => {
    let applied = 0;
    let at = { board, racks, toMove };
    for (const notation of notations) {
      try {
        const result = await api.play({ board: at.board, move: notation });
        const known = accountTiles(at.racks[at.toMove], tilesPlayed(result.move));
        dispatch({
          type: 'record',
          recorded: { move: result.move, board: result.board, ...known },
        });
        at = {
          board: result.board,
          racks: setSeat(at.racks, at.toMove, known.leave),
          toMove: other(at.toMove),
        };
        applied++;
      } catch (error) {
        return { applied, error: errorMessage(error) };
      }
    }
    return { applied, error: null };
  };

  const pass = () => {
    if (window.confirm(`Record a pass for ${moverName}?`)) void playNotation('-').then(setNotice);
  };

  const confirmExchange = () => {
    const letters = complete
      ? [...(exchange ?? [])].map((i) => rack[i]).join('')
      : normaliseRack(exchangeText);
    if (!letters) {
      setNotice('Choose the tiles exchanged.');
      return;
    }
    void playNotation(`-${letters}`).then(setNotice);
  };

  const onRackTile = (index: number) => {
    boardRef.current?.focus();
    if (exchange) {
      const next = new Set(exchange);
      if (next.has(index)) next.delete(index);
      else next.add(index);
      setExchange(next);
      return;
    }
    placement.clickRackTile(index);
  };

  const setRack = (next: string) =>
    dispatch({ type: 'racks', patch: { racks: setSeat(racks, toMove, next) } });

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

  // ---- Edit board mode ---------------------------------------------------

  const enterEditMode = () => {
    const later = descendantCount(tree, node.id);
    if (
      later > 0 &&
      !window.confirm(
        `Editing this position discards the ${later} move${later === 1 ? '' : 's'} after it. Continue?`,
      )
    ) {
      return;
    }
    if (later > 0) dispatch({ type: 'setup', patch: {} });
    placement.recallAll();
    setExchange(null);
    setMode('edit');
  };

  const editBoard = (row: number, col: number, char: string) =>
    dispatch({ type: 'setup', patch: { board: setSquare(board, row, col, char) } });

  const onEditKey = (event: KeyboardEvent<HTMLDivElement>) => {
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
      const delta = key === 'ArrowLeft' || key === 'ArrowUp' ? -1 : 1;
      const moved = advance({ ...cursor, direction: across ? 'across' : 'down' }, delta);
      setCursor({ ...moved, direction: cursor.direction });
    } else if (key === 'Escape') {
      setCursor(null);
    } else {
      return;
    }
    event.preventDefault();
  };

  // ---- Tree actions ------------------------------------------------------

  const removeCurrent = useCallback(() => {
    if (node.parent === null) return;
    const later = descendantCount(tree, node.id);
    const what = later > 0 ? `this move and the ${later} after it` : 'this move';
    if (later > 0 && !window.confirm(`Delete ${what}?`)) return;
    dispatch({ type: 'remove', id: node.id });
  }, [dispatch, node, tree]);

  const loadTree = (next: GameTree) => {
    if (hasMoves(tree) && !window.confirm('Replace the current game and its variations?')) {
      return false;
    }
    dispatch({ type: 'load', tree: next });
    setMode('moves');
    return true;
  };

  const importGcg = async (gcg: string): Promise<string | null> => {
    try {
      const parsed = await api.parseGcg({ gcg });
      return loadTree(treeFromEvents(parsed.start, parsed.moves)) ? null : 'Import cancelled.';
    } catch (error) {
      return errorMessage(error);
    }
  };

  // "Open in Solver" from another tab; each hand-over is applied once.
  const handled = useRef<number | null>(null);
  useEffect(() => {
    if (!incoming || handled.current === incoming.id) return;
    handled.current = incoming.id;
    if ('gcg' in incoming) {
      void importGcg(incoming.gcg).then((error) => error && setNotice(error));
    } else {
      loadTree(treeFromEvents(incoming.start, incoming.moves));
    }
    // Runs only for a new hand-over; the loaders read the latest tree.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [incoming]);

  const exportGcg = () => {
    const record = mainLine(tree)
      .slice(1)
      .map((id) => nodeAt(tree, id))
      .map((entry) => ({
        player: entry.origin!.player,
        rack: rackBefore(tree, entry),
        move: entry.origin!.move,
        cumulative: entry.scores[entry.origin!.player],
        kind: entry.origin!.kind,
        label: entry.origin!.label,
      }));
    const start = nodeAt(tree, tree.rootId);
    const text = toGcg(names, record, {
      title: 'Solver session',
      startScores: start.scores,
      customStart: start.board.some((line) => /[^.]/.test(line)),
    });
    downloadText('solver-game.gcg', text);
  };

  // Previews and transient input belong to one position.
  useEffect(() => {
    setHovered(null);
    setHoveredCandidate(null);
    setSelected(null);
    setChecked(null);
    setExchange(null);
    setNotice(null);
  }, [node.id]);

  // Alt+←/→ step through the line, Home/End jump, Ctrl/Cmd+Z takes the
  // current move back.
  useEffect(() => {
    const onKey = (event: globalThis.KeyboardEvent) => {
      if (isTextField(event.target)) return;
      if ((event.ctrlKey || event.metaKey) && event.key === 'z') {
        removeCurrent();
      } else if (event.altKey && event.key === 'ArrowLeft') {
        dispatch({ type: 'step', where: 'back' });
      } else if (event.altKey && event.key === 'ArrowRight') {
        dispatch({ type: 'step', where: 'forward' });
      } else if (event.key === 'Home' && !event.altKey) {
        dispatch({ type: 'step', where: 'first' });
      } else if (event.key === 'End' && !event.altKey) {
        dispatch({ type: 'step', where: 'last' });
      } else {
        return;
      }
      event.preventDefault();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [dispatch, removeCurrent]);

  const moves = useMoves(board, rack);

  // ---- Render ------------------------------------------------------------

  const preview =
    hoveredCandidate?.move ??
    hovered ??
    selected ??
    checked ??
    (mode === 'moves' ? placement.preview : null);
  const origin = node.origin;
  const onMainLine = isOnMainLine(tree, node.id);
  const ply = plyOf(tree, node.id);
  const exchangeAllowed = unseen.bagCount >= MIN_BAG_FOR_EXCHANGE;
  const editingMoves = mode === 'moves' && !ended;
  const short = rack.length < RACK_SIZE && unseen.bagCount > 0 && !ended;
  // A rack entered (or changed) after the move from here was recorded must
  // still be able to make that move.
  const rackConflict =
    complete &&
    mainChild?.origin?.kind === 'play' &&
    rack.length - accountTiles(rack, tilesPlayed(mainChild.origin.move)).leave.length <
      tilesPlayed(mainChild.origin.move).length;

  return (
    <div className="analysis solver">
      <div className="board-column">
        <div className="board-toolbar solver-toolbar">
          <div className="segmented" role="radiogroup" aria-label="Board mode">
            <button
              type="button"
              role="radio"
              aria-checked={mode === 'moves'}
              className={mode === 'moves' ? 'is-active' : undefined}
              onClick={() => setMode('moves')}
              title="Tiles placed on the board form a move for the player to move"
            >
              Enter moves
            </button>
            <button
              type="button"
              role="radio"
              aria-checked={mode === 'edit'}
              className={mode === 'edit' ? 'is-active' : undefined}
              onClick={() => mode !== 'edit' && enterEditMode()}
              title="Set up any position: tiles typed here are not a move"
            >
              Edit board
            </button>
          </div>
          <div className="button-row">
            <button
              type="button"
              className={`button-ghost toggle${showRacks ? ' is-on' : ''}`}
              aria-pressed={showRacks}
              onClick={() => setShowRacks(!showRacks)}
              title="Hide racks everywhere, e.g. when presenting a game"
            >
              Show racks
            </button>
            <button
              type="button"
              className={`button-ghost toggle${blankMode ? ' is-on' : ''}`}
              aria-pressed={blankMode}
              onClick={() => setBlankMode(!blankMode)}
              title="Type blanks without holding Shift"
            >
              Blank
            </button>
          </div>
        </div>

        <Scoreboard
          players={[{ name: names[0] || 'Player 1' }, { name: names[1] || 'Player 2' }]}
          scores={scores}
          racks={[
            racks[0] || (opponentKnown || toMove === 0 ? '' : null),
            racks[1] || (opponentKnown || toMove === 1 ? '' : null),
          ]}
          toMove={ended ? null : toMove}
          lastMover={origin?.player ?? null}
          lastScore={origin?.move.score ?? null}
          bag={unseen.bagCount}
          winner={null}
          tileValues={rules.tileValues}
          hideRacks={!showRacks}
        />

        <Board
          board={board}
          premiums={rules.premiums}
          tileValues={rules.tileValues}
          preview={preview}
          recent={origin?.kind === 'play' ? origin.move : null}
          cursor={mode === 'edit' || placement.cursor ? cursor : null}
          onSquareSelect={(row, col) => {
            boardRef.current?.focus();
            placement.selectSquare(row, col);
          }}
          onKeyDown={mode === 'edit' ? onEditKey : placement.onKeyDown}
          boardRef={boardRef}
        />

        <div className="nav-bar" role="group" aria-label="Move navigation">
          <button
            type="button"
            className="button-ghost"
            onClick={() => dispatch({ type: 'step', where: 'first' })}
            disabled={node.parent === null}
            title="Start (Home)"
          >
            ⏮
          </button>
          <button
            type="button"
            className="button-ghost"
            onClick={() => dispatch({ type: 'step', where: 'back' })}
            disabled={node.parent === null}
            title="Back (Alt+←)"
          >
            ◀
          </button>
          <span className="status-text nav-position">
            {ply === 0 ? 'Start' : `Move ${ply}`}
            {!onMainLine && ' · variation'}
          </span>
          <button
            type="button"
            className="button-ghost"
            onClick={() => dispatch({ type: 'step', where: 'forward' })}
            disabled={node.children.length === 0}
            title="Forward (Alt+→)"
          >
            ▶
          </button>
          <button
            type="button"
            className="button-ghost"
            onClick={() => dispatch({ type: 'step', where: 'last' })}
            disabled={node.children.length === 0}
            title="End of the line (End)"
          >
            ⏭
          </button>
        </div>

        {mode === 'edit' ? (
          <p className="hint mode-hint">
            {cursor ? (
              <>
                Editing at <strong>{squareName(cursor.row, cursor.col)}</strong> {cursor.direction}.
                Type letters (Shift for a blank), Backspace to clear, Space to turn, Esc to stop.
              </>
            ) : (
              'Edit board: click a square and type to set up any position. Tiles placed here are not a move. Switch back to Enter moves to record moves.'
            )}
          </p>
        ) : (
          <div className="play-controls move-entry">
            <p className="play-status">
              <strong>{ended ? 'Game over' : `${moverName} to move`}</strong>
              {editingMoves && !exchange && (
                <span className="hint">
                  {' '}
                  Click a square and type, or click a square then a tile. Shift+letter plays a
                  blank; Space turns; Enter records; Esc recalls.
                </span>
              )}
            </p>
            {showRacks ? (
              <div className="rack-entry">
                <input
                  className="text-input rack-text"
                  value={rack}
                  onChange={(event) => setRack(normaliseRack(event.target.value))}
                  placeholder="Rack (optional)"
                  aria-label={`${moverName}'s rack`}
                  spellCheck={false}
                  autoComplete="off"
                />
                <button type="button" className="button-ghost" onClick={fillRack}>
                  {rack.length > 0 && rack.length < RACK_SIZE ? 'Fill randomly' : 'Random'}
                </button>
                <button
                  type="button"
                  className="button-ghost"
                  onClick={() => setRack('')}
                  disabled={rack.length === 0}
                >
                  Clear
                </button>
              </div>
            ) : null}
            {showRacks && rack.length > 0 && (
              <PlayerRack
                tiles={placement.tiles}
                order={placement.tiles.map((_, i) => i)}
                used={placement.used}
                selected={exchange}
                tileValues={rules.tileValues}
                disabled={!editingMoves || playing}
                onTileClick={onRackTile}
                blankPending={placement.blankPending}
                onBlankLetter={(letter) => {
                  placement.chooseBlank(letter);
                  boardRef.current?.focus();
                }}
                onBlankCancel={placement.cancelBlank}
              />
            )}
            <p className="hint">
              {!showRacks
                ? 'Racks are hidden.'
                : complete
                  ? 'Moves must come from this rack.'
                  : rack.length > 0
                    ? `Partly known (${rack.length} tile${rack.length === 1 ? '' : 's'}): tiles not on it are taken as drawn. ${short ? 'Type the tiles drawn, or fill randomly, to solve.' : ''}`
                    : 'Rack unknown: any tiles may be placed. Enter it to solve here and to review this move.'}
            </p>
            {showRacks && rackConflict && (
              <p className="verdict is-illegal">
                This rack does not hold the tiles of {mainChild?.origin?.move.notation}, the move
                played from here.
              </p>
            )}

            {exchange !== null ? (
              <div className="button-row">
                {!complete && (
                  <input
                    className="text-input rack-text exchange-text"
                    value={exchangeText}
                    onChange={(event) => setExchangeText(normaliseRack(event.target.value))}
                    placeholder="Tiles exchanged"
                    aria-label="Tiles exchanged"
                    autoFocus
                  />
                )}
                <button
                  type="button"
                  className="button-primary"
                  disabled={playing}
                  onClick={confirmExchange}
                >
                  Exchange
                  {complete && exchange.size > 0
                    ? ` ${exchange.size} tile${exchange.size === 1 ? '' : 's'}`
                    : ''}
                </button>
                <button type="button" className="button-ghost" onClick={() => setExchange(null)}>
                  Cancel
                </button>
                {complete && <span className="hint">Pick the tiles put back.</span>}
              </div>
            ) : (
              <div className="button-row play-buttons">
                <button
                  type="button"
                  className="button-primary"
                  disabled={!editingMoves || playing || placement.verdict.kind !== 'legal'}
                  onClick={placement.submit}
                >
                  Record move
                </button>
                <button
                  type="button"
                  className="button-secondary"
                  disabled={!editingMoves || playing}
                  onClick={pass}
                >
                  Pass
                </button>
                <button
                  type="button"
                  className="button-secondary"
                  disabled={!editingMoves || playing || !exchangeAllowed}
                  title={exchangeAllowed ? undefined : 'The bag needs at least 7 tiles'}
                  onClick={() => {
                    placement.recallAll();
                    setExchange(new Set());
                  }}
                >
                  Exchange
                </button>
                <button
                  type="button"
                  className="button-ghost"
                  disabled={placement.placed.length === 0}
                  onClick={placement.recallAll}
                >
                  Recall
                </button>
              </div>
            )}
            <PlacementVerdict verdict={placement.verdict} notice={placement.notice} />
          </div>
        )}

        {ended && (
          <p className="verdict is-legal end-banner">
            {wentOut(tree, node) && origin
              ? `${names[origin.player]} went out. Game over.`
              : 'Six scoreless turns in a row. Game over.'}{' '}
            Final rack adjustments are not applied here.
          </p>
        )}
        {notice && <p className="verdict is-illegal">{notice}</p>}
        <Legend />
      </div>

      <div className="side-column">
        <SolverResults
          moverName={moverName}
          analysis={analysis}
          loading={solvingKey === requestKey}
          error={failedHere ? solveError.message : null}
          effort={effort}
          onEffort={setEffort}
          blocked={blocked}
          onSolve={solve}
          engineOn={engineOn}
          onEngineOn={setEngineOn}
          playedNotation={playedHere}
          hovered={hoveredCandidate}
          onHover={setHoveredCandidate}
          onPlay={playCandidate}
          playing={playing || ended}
        />

        <section className="panel" aria-labelledby="moves-heading">
          <div className="panel-header">
            <h2 id="moves-heading">Moves</h2>
            <span className="status-text">{mainLine(tree).length - 1} in the main line</span>
          </div>
          {currentGrade && origin && (
            <p className={`verdict grade-note grade-${currentGrade.grade}`}>
              <strong>{GRADE_LABEL[currentGrade.grade]}</strong>: {origin.move.notation}{' '}
              {currentGrade.lost > 0
                ? `gave up ${currentGrade.lost.toFixed(1)} points to ${currentGrade.best.move.notation}`
                : 'matches the engine'}{' '}
              ({currentGrade.basis}).{' '}
              {node.parent && currentGrade.lost > 0 && (
                <button
                  type="button"
                  className="link-button"
                  onClick={() => dispatch({ type: 'goto', id: node.parent! })}
                >
                  See the position before
                </button>
              )}
            </p>
          )}
          {origin?.note && <p className="hint">Note: {origin.note}</p>}
          <MoveTree
            tree={tree}
            grades={grades}
            showRacks={showRacks}
            onGoto={(id) => dispatch({ type: 'goto', id })}
          />
          <div className="button-row">
            <button
              type="button"
              className="button-ghost"
              disabled={onMainLine}
              onClick={() => dispatch({ type: 'promote', id: node.id })}
              title="Make the line through this move the main line"
            >
              Promote to main line
            </button>
            <button
              type="button"
              className="button-ghost"
              disabled={node.parent === null}
              onClick={removeCurrent}
              title="Delete this move and everything after it (Ctrl+Z)"
            >
              {onMainLine ? 'Delete from here' : 'Delete variation'}
            </button>
            <button
              type="button"
              className="button-ghost"
              onClick={exportGcg}
              disabled={!hasMoves(tree)}
              title="Download the main line as GCG"
            >
              Export GCG
            </button>
            <button
              type="button"
              className="button-ghost"
              onClick={() => {
                if (
                  !hasMoves(tree) ||
                  window.confirm('Start a new game? The moves are discarded.')
                ) {
                  dispatch({ type: 'reset' });
                  setCursor(null);
                }
              }}
            >
              New game
            </button>
          </div>
          <p className="hint">
            A move made from an earlier position starts a variation; the main line is kept.
          </p>
        </section>

        <PositionPanel
          names={names}
          node={node}
          editable={mode === 'edit'}
          showRacks={showRacks}
          onRename={(next) => dispatch({ type: 'rename', names: next })}
          onSetup={(patch) => dispatch({ type: 'setup', patch })}
          onRacks={(patch) => dispatch({ type: 'racks', patch })}
        />
        <MoveChecker
          board={board}
          rack={complete ? rack : ''}
          onPlay={playNotation}
          onPreview={setChecked}
        />
      </div>

      <div className="extras">
        <GameReviewPanel
          names={names}
          moves={reviewed}
          progress={review.progress}
          currentId={node.id}
          onRun={(level) => {
            solveController.current?.abort();
            void review.run(level);
          }}
          onCancel={review.cancel}
          onGoto={(id) => dispatch({ type: 'goto', id })}
        />
        <ImportPanel onImportGcg={importGcg} onApplyMoves={applySequence} />
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
          onPlay={(move) =>
            void playCandidate({ move, equity: 0, staticRank: null, chosen: false })
          }
        />
      </div>
    </div>
  );
}
