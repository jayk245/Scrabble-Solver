import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent } from 'react';

import { api, gameGcgUrl } from '../api/client';
import type {
  Candidate,
  GameEndEvent,
  MoveAnalysis,
  MoveInfo,
  ReviewEntry,
  Rules,
  Seats,
  StartGameRequest,
} from '../api/types';
import { Board } from '../components/Board';
import { CandidateTable } from '../components/CandidateTable';
import { GameLog } from '../components/GameLog';
import { Legend } from '../components/Legend';
import { PlayerRack } from '../components/PlayerRack';
import { PlaySetup } from '../components/PlaySetup';
import { ReviewPanel } from '../components/ReviewPanel';
import { Scoreboard } from '../components/Scoreboard';
import { UnseenTiles } from '../components/UnseenTiles';
import { useGameStream, type GameStream } from '../hooks/useGameStream';
import { BOARD_SIZE, EMPTY, emptyBoard, squareAt, squareName, type Cursor } from '../lib/board';
import { placementNotation, placementPreview, type PlacedTile } from '../lib/placement';
import { unseenSummary } from '../lib/tiles';

/** Delay before checking a word being built, so typing stays responsive. */
const VALIDATE_DEBOUNCE_MS = 150;
/** The bag must hold a full rack for an exchange. */
const MIN_BAG_FOR_EXCHANGE = 7;

const END_REASON: Partial<Record<GameEndEvent['reason'], string>> = {
  went_out: 'went out',
  scoreless_turns: 'six scoreless turns',
  resigned: 'resigned',
  cancelled: 'cancelled',
  abandoned: 'abandoned after inactivity',
};

type Verdict =
  | { kind: 'none' }
  | { kind: 'checking' }
  | { kind: 'legal'; notation: string; move: MoveInfo }
  | { kind: 'illegal'; message: string };

function gameIdFromHash(): string | null {
  return /^#play\/([0-9a-f]+)$/.exec(window.location.hash)?.[1] ?? null;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function identity(length: number): number[] {
  return Array.from({ length }, (_, i) => i);
}

function shuffled(values: number[]): number[] {
  const out = [...values];
  for (let i = out.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [out[i], out[j]] = [out[j]!, out[i]!];
  }
  return out;
}

/** The square `steps` along the cursor's direction, or null off the board. */
function step(cursor: Cursor, steps: number): Cursor | null {
  const row = cursor.direction === 'down' ? cursor.row + steps : cursor.row;
  const col = cursor.direction === 'across' ? cursor.col + steps : cursor.col;
  if (row < 0 || col < 0 || row >= BOARD_SIZE || col >= BOARD_SIZE) return null;
  return { ...cursor, row, col };
}

function EndSummary({
  end,
  names,
  humanSeat,
}: {
  end: GameEndEvent;
  names: Seats<string>;
  humanSeat: 0 | 1;
}) {
  if (end.reason === 'error') {
    return <p className="verdict is-illegal">The game failed: {end.message}</p>;
  }
  const scores = end.scores ?? [0, 0];
  const mine = scores[humanSeat];
  const theirs = scores[humanSeat === 0 ? 1 : 0];
  const result =
    end.winner === humanSeat
      ? `You win ${mine}–${theirs}`
      : end.winner === -1
        ? `Tie at ${mine}`
        : end.winner === null || end.winner === undefined
          ? `Game ended at ${mine}–${theirs}`
          : `The bot wins ${theirs}–${mine}`;
  const botRack = end.racks?.[humanSeat === 0 ? 1 : 0];
  const adjustments = (end.adjustments ?? [0, 0])
    .map((points, seat) => (points !== 0 ? `${names[seat]} ${points > 0 ? '+' : ''}${points}` : null))
    .filter(Boolean)
    .join(', ');
  return (
    <div className={`verdict ${end.winner === humanSeat ? 'is-legal' : 'is-neutral'} end-banner`}>
      <strong>{result}</strong> ({END_REASON[end.reason] ?? end.reason}
      {adjustments ? `; rack adjustments ${adjustments}` : ''}).
      {botRack !== undefined && botRack !== null && (
        <> The bot ended holding {botRack ? <span className="notation">{botRack}</span> : 'nothing'}.</>
      )}{' '}
      Seed {end.seed}.
    </div>
  );
}

interface PlayScreenProps {
  gameId: string;
  stream: GameStream;
  rules: Rules;
  onNewGame: () => void;
}

/** One game in progress (or finished); remounted per game. */
function PlayScreen({ gameId, stream, rules, onNewGame }: PlayScreenProps) {
  const { start, moves, end } = stream;
  const humanSeat: 0 | 1 = start?.humanSeat ?? 0;
  const botSeat: 0 | 1 = humanSeat === 0 ? 1 : 0;
  const latest = moves.at(-1) ?? null;
  const liveBoard = latest?.board ?? start?.board ?? emptyBoard();
  const rack = (latest ? latest.racks[humanSeat] : start?.racks[humanSeat]) ?? '';
  const toMove: 0 | 1 | null = end
    ? null
    : latest
      ? (latest.toMove ?? null)
      : (start?.toMove ?? null);
  const myTurn = toMove === humanSeat;
  const thinking = toMove === botSeat;
  const bag = latest?.bag ?? start?.bag ?? 0;
  const tiles = useMemo(() => [...rack], [rack]);

  const [placed, setPlaced] = useState<PlacedTile[]>([]);
  const [cursor, setCursor] = useState<Cursor | null>(null);
  const [order, setOrder] = useState<number[]>(() => identity(tiles.length));
  const [exchange, setExchange] = useState<Set<number> | null>(null);
  const [blankIndex, setBlankIndex] = useState<number | null>(null);
  const [verdict, setVerdict] = useState<Verdict>({ kind: 'none' });
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);
  const [hint, setHint] = useState<MoveAnalysis | null>(null);
  const [hoveredHint, setHoveredHint] = useState<Candidate | null>(null);
  const [hoveredReview, setHoveredReview] = useState<ReviewEntry | null>(null);
  const [viewCursor, setViewCursor] = useState<number | null>(null);
  const boardRef = useRef<HTMLDivElement>(null);

  // Every new move (ours, confirmed by the server, or the bot's) starts a
  // fresh turn: tentative tiles were either accepted or are now stale.
  useEffect(() => {
    setPlaced([]);
    setOrder(identity(tiles.length));
    setExchange(null);
    setBlankIndex(null);
    setHint(null);
    setHoveredHint(null);
    setNotice(null);
  }, [moves.length, tiles.length]);

  const used = useMemo(() => new Set(placed.map((tile) => tile.rackIndex)), [placed]);
  const placement = useMemo(() => placementNotation(liveBoard, placed), [liveBoard, placed]);

  // Live legality and score: the engine is the only judge of words, so
  // each new arrangement is sent to /api/validate (debounced, and aborted
  // when superseded so a stale answer never wins).
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
        .validate({ board: liveBoard, rack, move: placement.notation }, controller.signal)
        .then((response) =>
          setVerdict(
            response.valid
              ? { kind: 'legal', notation: placement.notation, move: response.move }
              : { kind: 'illegal', message: response.message },
          ),
        )
        .catch((error: unknown) => {
          if (!controller.signal.aborted) setVerdict({ kind: 'illegal', message: errorMessage(error) });
        });
    }, VALIDATE_DEBOUNCE_MS);
    return () => {
      window.clearTimeout(timer);
      controller.abort();
    };
  }, [placement, liveBoard, rack]);

  const occupiedAt = useCallback(
    (row: number, col: number) =>
      squareAt(liveBoard, row, col) !== EMPTY ||
      placed.some((tile) => tile.row === row && tile.col === col),
    [liveBoard, placed],
  );

  const canEdit = myTurn && !busy && viewCursor === null && exchange === null;

  /** Places rack tile `index` (as board character `letter`) at the cursor. */
  const placeAt = (index: number, letter: string) => {
    if (!cursor) return;
    let target: Cursor | null = cursor;
    while (target && occupiedAt(target.row, target.col)) target = step(target, 1);
    if (!target) return;
    const square = target;
    setPlaced((current) => [...current, { row: square.row, col: square.col, letter, rackIndex: index }]);
    setCursor(step(square, 1) ?? square);
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
    if (blank >= 0) {
      placeAt(blank, letter.toLowerCase());
      return;
    }
    setNotice(asBlank ? 'You have no blank left.' : `There is no ${letter} on your rack.`);
  };

  const recallTile = (index: number) => {
    const tile = placed.find((entry) => entry.rackIndex === index);
    setPlaced((current) => current.filter((entry) => entry.rackIndex !== index));
    if (tile) setCursor((current) => ({ row: tile.row, col: tile.col, direction: current?.direction ?? 'across' }));
  };

  const recallAll = () => {
    setPlaced([]);
    setBlankIndex(null);
  };

  const selectSquare = useCallback(
    (row: number, col: number) => {
      boardRef.current?.focus();
      const tile = placed.find((entry) => entry.row === row && entry.col === col);
      if (tile && canEdit) {
        setPlaced((current) => current.filter((entry) => entry !== tile));
        setCursor((current) => ({ row, col, direction: current?.direction ?? 'across' }));
        return;
      }
      setCursor((current) =>
        current && current.row === row && current.col === col
          ? { ...current, direction: current.direction === 'across' ? 'down' : 'across' }
          : { row, col, direction: current?.direction ?? 'across' },
      );
    },
    [placed, canEdit],
  );

  const onRackClick = (index: number) => {
    boardRef.current?.focus();
    if (exchange) {
      const next = new Set(exchange);
      if (next.has(index)) next.delete(index);
      else next.add(index);
      setExchange(next);
      return;
    }
    if (!canEdit) return;
    if (used.has(index)) {
      recallTile(index);
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

  const send = async (move: string): Promise<boolean> => {
    setBusy(true);
    setNotice(null);
    try {
      await api.humanMove(gameId, { move });
      // The board updates when the move arrives over the event stream.
      return true;
    } catch (error) {
      setNotice(errorMessage(error));
      return false;
    } finally {
      setBusy(false);
    }
  };

  const submit = () => {
    if (verdict.kind === 'legal' && canEdit) void send(verdict.notation);
  };

  const pass = () => {
    if (canEdit && window.confirm('Pass this turn?')) void send('-');
  };

  const confirmExchange = async () => {
    if (!exchange || exchange.size === 0) return;
    const letters = [...exchange].map((i) => tiles[i]).join('');
    if (await send(`-${letters}`)) setExchange(null);
  };

  const resign = async () => {
    if (!window.confirm('Resign this game?')) return;
    try {
      await api.humanMove(gameId, { resign: true });
    } catch (error) {
      setNotice(errorMessage(error));
    }
  };

  const askHint = async () => {
    try {
      setHint(await api.hint(gameId));
    } catch (error) {
      setNotice(errorMessage(error));
    }
  };

  /** Lays a suggested move's tiles on the board for the player to submit. */
  const applyCandidate = (candidate: Candidate) => {
    const taken = new Set<number>();
    const next: PlacedTile[] = [];
    for (const square of candidate.move.tiles) {
      if (square.playThrough) continue;
      const wanted = square.blank ? '?' : square.letter;
      const index = tiles.findIndex((tile, i) => tile === wanted && !taken.has(i));
      if (index < 0) return;
      taken.add(index);
      next.push({ row: square.row, col: square.col, letter: square.letter, rackIndex: index });
    }
    setPlaced(next);
    setCursor(null);
    setHoveredHint(null);
    boardRef.current?.focus();
  };

  const onBoardKey = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.ctrlKey || event.metaKey || event.altKey) return;
    const { key } = event;
    if (key === 'Enter') {
      submit();
    } else if (key === 'Escape') {
      recallAll();
      setCursor(null);
    } else if (!canEdit) {
      return;
    } else if (/^[a-zA-Z]$/.test(key) && cursor) {
      placeLetter(key.toUpperCase(), event.shiftKey);
    } else if (key === 'Backspace') {
      const last = placed.at(-1);
      if (!last) return;
      setPlaced(placed.slice(0, -1));
      setCursor({ row: last.row, col: last.col, direction: cursor?.direction ?? 'across' });
    } else if (key === ' ' && cursor) {
      setCursor({ ...cursor, direction: cursor.direction === 'across' ? 'down' : 'across' });
    } else if (key.startsWith('Arrow') && cursor) {
      const across = key === 'ArrowLeft' || key === 'ArrowRight';
      const delta = key === 'ArrowLeft' || key === 'ArrowUp' ? -1 : 1;
      const moved = step({ ...cursor, direction: across ? 'across' : 'down' }, delta);
      if (moved) setCursor({ ...moved, direction: cursor.direction });
    } else {
      return;
    }
    event.preventDefault();
  };

  if (!start) {
    return (
      <p className="splash">{stream.status === 'error' ? stream.error : 'Dealing the tiles…'}</p>
    );
  }

  const names: Seats<string> = [start.players[0].name, start.players[1].name];
  const scores = end?.scores ?? latest?.scores ?? start.scores;
  const shownRacks: Seats<string | null> = end?.racks ?? [
    humanSeat === 0 ? rack : null,
    humanSeat === 1 ? rack : null,
  ];

  // What the board shows: a review row's position, an earlier position
  // picked in the log, or the live game with the tentative tiles on top.
  const boardBefore = (turn: number) => {
    const index = moves.findIndex((move) => move.turn === turn);
    return index > 0 ? (moves[index - 1]?.board ?? start.board) : start.board;
  };
  let shownBoard = liveBoard;
  let preview: MoveInfo | null = hoveredHint?.move ?? placementPreview(placed);
  let recent: MoveInfo | null = latest?.move ?? null;
  if (hoveredReview) {
    shownBoard = boardBefore(hoveredReview.turn);
    preview = hoveredReview.best?.move ?? null;
    recent = null;
  } else if (viewCursor !== null) {
    shownBoard = viewCursor === 0 ? start.board : (moves[viewCursor - 1]?.board ?? liveBoard);
    preview = null;
    recent = viewCursor === 0 ? null : (moves[viewCursor - 1]?.move ?? null);
  }

  const unseen = unseenSummary(liveBoard, rack, rules.tileCounts, { bag });
  const exchangeAllowed = bag >= MIN_BAG_FOR_EXCHANGE;

  const status = end
    ? 'Game over'
    : myTurn
      ? 'Your turn'
      : thinking
        ? 'The bot is thinking…'
        : 'Waiting…';

  return (
    <div className="analysis viewer play">
      <div className="board-column">
        <Scoreboard
          players={start.players}
          scores={scores}
          racks={shownRacks}
          toMove={toMove}
          lastMover={latest?.player ?? null}
          lastScore={latest?.score ?? null}
          bag={bag}
          winner={end && typeof end.winner === 'number' ? end.winner : null}
          tileValues={rules.tileValues}
        />
        <Board
          board={shownBoard}
          premiums={rules.premiums}
          tileValues={rules.tileValues}
          preview={preview}
          recent={recent}
          cursor={canEdit ? cursor : null}
          onSquareSelect={selectSquare}
          onKeyDown={onBoardKey}
          boardRef={boardRef}
        />

        {viewCursor !== null && (
          <p className="verdict is-neutral">
            Showing the board after move {viewCursor}.{' '}
            <button type="button" className="link-button" onClick={() => setViewCursor(null)}>
              Back to the game
            </button>
          </p>
        )}

        {!end && (
          <div className="play-controls">
            <p className={`play-status${thinking ? ' is-loading' : ''}`} role="status">
              <strong>{status}</strong>
              {myTurn && !exchange && (
                <span className="hint">
                  {' '}
                  Click a square and type, or click a square then a tile. Shift+letter plays a
                  blank; Enter submits; Esc recalls.
                </span>
              )}
            </p>
            <PlayerRack
              tiles={tiles}
              order={order}
              used={used}
              selected={exchange}
              tileValues={rules.tileValues}
              disabled={!myTurn || busy}
              onTileClick={onRackClick}
              blankPending={blankIndex !== null}
              onBlankLetter={(letter) => {
                if (blankIndex !== null) placeAt(blankIndex, letter.toLowerCase());
                setBlankIndex(null);
                boardRef.current?.focus();
              }}
              onBlankCancel={() => setBlankIndex(null)}
            />

            {exchange ? (
              <div className="button-row">
                <button
                  type="button"
                  className="button-primary"
                  disabled={exchange.size === 0 || busy}
                  onClick={confirmExchange}
                >
                  Exchange {exchange.size || ''} {exchange.size === 1 ? 'tile' : 'tiles'}
                </button>
                <button type="button" className="button-ghost" onClick={() => setExchange(null)}>
                  Cancel
                </button>
                <span className="hint">Pick the tiles to put back.</span>
              </div>
            ) : (
              <div className="button-row play-buttons">
                <button
                  type="button"
                  className="button-primary"
                  disabled={!canEdit || verdict.kind !== 'legal'}
                  onClick={submit}
                >
                  Submit
                </button>
                <button type="button" className="button-secondary" disabled={!canEdit} onClick={pass}>
                  Pass
                </button>
                <button
                  type="button"
                  className="button-secondary"
                  disabled={!canEdit || !exchangeAllowed}
                  title={exchangeAllowed ? undefined : 'The bag needs at least 7 tiles'}
                  onClick={() => {
                    recallAll();
                    setExchange(new Set());
                  }}
                >
                  Exchange
                </button>
                <button
                  type="button"
                  className="button-ghost"
                  disabled={!myTurn}
                  onClick={() => setOrder(shuffled(order))}
                >
                  Shuffle
                </button>
                <button
                  type="button"
                  className="button-ghost"
                  disabled={placed.length === 0}
                  onClick={recallAll}
                >
                  Recall
                </button>
                <button type="button" className="button-ghost" disabled={!canEdit} onClick={askHint}>
                  Hint
                </button>
              </div>
            )}

            {verdict.kind === 'legal' && (
              <p className="verdict is-legal">
                <strong className="notation">{verdict.move.notation}</strong> scores{' '}
                <strong>{verdict.move.score}</strong>
                {verdict.move.bingo && ' (bingo!)'}
              </p>
            )}
            {verdict.kind === 'illegal' && <p className="verdict is-illegal">{verdict.message}</p>}
            {verdict.kind === 'checking' && <p className="hint">Checking…</p>}
            {notice && <p className="verdict is-illegal">{notice}</p>}
          </div>
        )}
        {end && <EndSummary end={end} names={names} humanSeat={humanSeat} />}
        {stream.status === 'error' && <p className="verdict is-illegal">{stream.error}</p>}
        <Legend />
      </div>

      <div className="side-column">
        <div className="panel game-toolbar">
          <div className="panel-header">
            <h2>
              You vs {start.strength ?? 'normal'} bot · {start.rules?.toUpperCase()}
            </h2>
          </div>
          <div className="button-row">
            <button
              type="button"
              className="button-secondary"
              onClick={() => {
                if (!end && !window.confirm('Leave this game? It will be resigned.')) return;
                if (!end) api.humanMove(gameId, { resign: true }).catch(() => {});
                onNewGame();
              }}
            >
              New game
            </button>
            {!end && (
              <button type="button" className="button-ghost" onClick={resign}>
                Resign
              </button>
            )}
            {end?.gcg ? (
              <a className="button-ghost" href={gameGcgUrl(gameId)} download={`game-${gameId}.gcg`}>
                Download GCG
              </a>
            ) : (
              <button type="button" className="button-ghost" disabled title="Available when the game ends">
                Download GCG
              </button>
            )}
          </div>
        </div>

        {hint && !end && (
          <section className="panel" aria-labelledby="hint-heading">
            <div className="panel-header">
              <h2 id="hint-heading">Hint</h2>
              <button type="button" className="button-ghost" onClick={() => setHint(null)}>
                Hide
              </button>
            </div>
            <CandidateTable
              candidates={hint.candidates}
              hovered={hoveredHint}
              onHover={setHoveredHint}
              showSim={false}
              chosenLabel="best"
              action={{ label: 'Use', onClick: applyCandidate, disabled: !canEdit }}
            />
            <p className="hint">
              The engine&apos;s top static moves for your rack ({hint.totalMoves.toLocaleString()}{' '}
              legal). Hover to preview, Use to lay the tiles down.
            </p>
          </section>
        )}

        {end?.review && (
          <ReviewPanel
            review={end.review}
            hintsUsed={end.hintsUsed ?? 0}
            hovered={hoveredReview}
            onHover={setHoveredReview}
          />
        )}

        <GameLog
          moves={moves}
          names={names}
          cursor={viewCursor ?? moves.length}
          onSeek={(next) => setViewCursor(next >= moves.length ? null : next)}
          live={thinking}
          thinkingLabel="The bot is thinking…"
        />
        <UnseenTiles
          position={unseen}
          note="Unseen tiles are the bag plus the bot's rack, as you would track them."
        />
        {cursor && canEdit && (
          <p className="hint">
            Cursor at {squareName(cursor.row, cursor.col)} {cursor.direction}.
          </p>
        )}
      </div>
    </div>
  );
}

/**
 * Play against the bot. The server deals, referees and runs the bot; this
 * view only arranges tiles, asks the engine whether the arrangement is a
 * legal word (/api/validate), and submits it. Everything shown comes from
 * the game's event stream, so a reload (or a #play/<id> link) restores the
 * game exactly.
 */
export function PlayView({ rules, active }: { rules: Rules; active: boolean }) {
  const [gameId, setGameId] = useState<string | null>(gameIdFromHash);
  const [starting, setStarting] = useState(false);
  const [startError, setStartError] = useState<string | null>(null);
  const stream = useGameStream(gameId);

  useEffect(() => {
    if (!active) return;
    window.history.replaceState(null, '', gameId ? `#play/${gameId}` : '#play');
  }, [active, gameId]);

  const startGame = async (request: StartGameRequest) => {
    setStarting(true);
    setStartError(null);
    try {
      const response = await api.startGame(request);
      setGameId(response.id);
    } catch (error) {
      setStartError(errorMessage(error));
    } finally {
      setStarting(false);
    }
  };

  if (gameId) {
    return (
      <PlayScreen
        key={gameId}
        gameId={gameId}
        stream={stream}
        rules={rules}
        onNewGame={() => setGameId(null)}
      />
    );
  }

  return (
    <div className="analysis viewer play">
      <div className="board-column">
        <Board board={emptyBoard()} premiums={rules.premiums} tileValues={rules.tileValues} />
      </div>
      <div className="side-column">
        <PlaySetup busy={starting} error={startError} onStart={startGame} />
      </div>
    </div>
  );
}
