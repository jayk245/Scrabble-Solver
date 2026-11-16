import { useEffect, useState } from 'react';

import { api, gameGcgUrl } from '../api/client';
import type { Candidate, GameEndEvent, Rules, Seats, StartGameRequest } from '../api/types';
import { AnalysisPanel } from '../components/AnalysisPanel';
import { Board } from '../components/Board';
import { GameLog } from '../components/GameLog';
import { GameSetup } from '../components/GameSetup';
import { Legend } from '../components/Legend';
import { PlaybackControls } from '../components/PlaybackControls';
import { Scoreboard } from '../components/Scoreboard';
import { useGameStream, type GameStream } from '../hooks/useGameStream';
import { usePlayback } from '../hooks/usePlayback';
import { emptyBoard } from '../lib/board';

const END_REASON: Record<GameEndEvent['reason'], string> = {
  went_out: 'went out',
  scoreless_turns: 'six scoreless turns',
  record_end: 'end of record',
  cancelled: 'cancelled',
  error: 'error',
};

function isTextField(target: EventTarget | null): boolean {
  return (
    target instanceof HTMLInputElement ||
    target instanceof HTMLTextAreaElement ||
    target instanceof HTMLSelectElement
  );
}

function gameIdFromHash(): string | null {
  return /^#viewer\/([0-9a-f]+)$/.exec(window.location.hash)?.[1] ?? null;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function EndBanner({ end, names }: { end: GameEndEvent; names: Seats<string> }) {
  if (end.reason === 'error') {
    return <p className="verdict is-illegal">The game failed: {end.message}</p>;
  }
  const scores = end.scores ?? [0, 0];
  const adjustments = end.adjustments ?? [0, 0];
  const result =
    end.winner === 0 || end.winner === 1
      ? `${names[end.winner]} wins ${Math.max(...scores)}–${Math.min(...scores)}`
      : end.winner === -1
        ? `Tie at ${scores[0]}`
        : `Stopped at ${scores[0]}–${scores[1]}`;
  const adjusted = adjustments
    .map((points, seat) =>
      points !== 0 ? `${names[seat]} ${points > 0 ? '+' : ''}${points}` : null,
    )
    .filter(Boolean)
    .join(', ');
  return (
    <p className="verdict is-legal end-banner">
      {result} ({END_REASON[end.reason]}
      {adjusted ? `; rack adjustments ${adjusted}` : ''}).
    </p>
  );
}

interface GameScreenProps {
  gameId: string;
  stream: GameStream;
  rules: Rules;
  active: boolean;
  onNewGame: () => void;
}

/** Playback of one game; remounted per game so playback state resets. */
function GameScreen({ gameId, stream, rules, active, onNewGame }: GameScreenProps) {
  const { start, moves, end } = stream;
  const live = end === null && stream.status !== 'error';
  const playback = usePlayback(moves.length, live);
  const [hovered, setHovered] = useState<Candidate | null>(null);
  const [cancelling, setCancelling] = useState(false);
  const { cursor, step, seek, toggle } = playback;
  const total = moves.length;

  // Arrow keys step, Space plays/pauses, Home/End jump, while this view is
  // visible and the user is not typing.
  useEffect(() => {
    if (!active) return;
    const onKey = (event: KeyboardEvent) => {
      if (isTextField(event.target) || event.ctrlKey || event.metaKey || event.altKey) return;
      if (event.key === 'ArrowLeft') step(-1);
      else if (event.key === 'ArrowRight') step(1);
      else if (event.key === 'Home') seek(0);
      else if (event.key === 'End') seek(total);
      else if (event.key === ' ') toggle();
      else return;
      event.preventDefault();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [active, step, seek, toggle, total]);

  // The candidate preview belongs to one position.
  useEffect(() => setHovered(null), [cursor]);

  if (!start) {
    return (
      <p className="splash">
        {stream.status === 'error' ? stream.error : 'Connecting to the game…'}
      </p>
    );
  }

  const names: Seats<string> = [start.players[0].name, start.players[1].name];
  const event = cursor > 0 ? (moves[cursor - 1] ?? null) : null;
  const atEnd = cursor === total && end !== null;
  const before = cursor > 1 ? (moves[cursor - 2]?.board ?? start.board) : start.board;
  const scores = atEnd && end.scores ? end.scores : (event?.scores ?? start.scores);
  const racks = atEnd && end.racks ? end.racks : (event?.racks ?? start.racks);
  const toMove: 0 | 1 | null = atEnd ? null : event ? (event.player === 0 ? 1 : 0) : 0;
  const winner = atEnd && end.winner !== undefined ? end.winner : null;
  const previewing = hovered !== null && !hovered.chosen;

  const cancel = async () => {
    setCancelling(true);
    try {
      await api.cancelGame(gameId);
    } catch {
      // The end event (or its absence) reports the outcome either way.
    } finally {
      setCancelling(false);
    }
  };

  return (
    <div className="analysis viewer">
      <div className="board-column">
        <Scoreboard
          players={start.players}
          scores={scores}
          racks={racks}
          toMove={toMove}
          lastMover={event?.player ?? null}
          lastScore={event?.score ?? null}
          bag={event?.bag ?? start.bag}
          winner={winner}
          tileValues={rules.tileValues}
        />
        <Board
          board={previewing ? before : (event?.board ?? start.board)}
          premiums={rules.premiums}
          tileValues={rules.tileValues}
          preview={previewing ? hovered.move : null}
          recent={previewing ? null : (event?.move ?? null)}
        />
        <PlaybackControls playback={playback} total={total} live={live} />
        {atEnd && <EndBanner end={end} names={names} />}
        {stream.status === 'error' && <p className="verdict is-illegal">{stream.error}</p>}
        <Legend />
      </div>

      <div className="side-column">
        <div className="panel game-toolbar">
          <div className="panel-header">
            <h2>
              {start.mode === 'selfplay'
                ? `Self-play · seed ${start.seed} · ${start.rules?.toUpperCase()}`
                : start.title || 'Replay'}
            </h2>
          </div>
          <div className="button-row">
            <button
              type="button"
              className="button-secondary"
              onClick={() => {
                // Nobody will watch the rest, so free the server's worker.
                if (live) api.cancelGame(gameId).catch(() => {});
                onNewGame();
              }}
            >
              New game
            </button>
            {live && (
              <button type="button" className="button-ghost" onClick={cancel} disabled={cancelling}>
                Stop game
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
        <GameLog moves={moves} names={names} cursor={cursor} onSeek={seek} live={live} />
        <AnalysisPanel
          event={event}
          playerName={event ? names[event.player] : ''}
          hovered={hovered}
          onHover={setHovered}
        />
      </div>
    </div>
  );
}

/**
 * The game viewer: start a bot-vs-bot game or load a GCG record, then play
 * it back move by move with the engine's reasoning alongside.
 *
 * Events arrive over Server-Sent Events as fast as the server produces them
 * and are buffered in full (useGameStream); pause, step, seek and speed act
 * on that buffer only (usePlayback), so they are instant and never need a
 * round trip.
 */
export function GameViewerView({ rules, active }: { rules: Rules; active: boolean }) {
  const [gameId, setGameId] = useState<string | null>(gameIdFromHash);
  const [starting, setStarting] = useState(false);
  const [startError, setStartError] = useState<string | null>(null);
  const stream = useGameStream(gameId);

  // "#viewer/<id>" reopens a game after a reload or from a shared link,
  // for as long as the server keeps it (15 minutes after it finishes).
  useEffect(() => {
    if (!active) return;
    window.history.replaceState(null, '', gameId ? `#viewer/${gameId}` : '#viewer');
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
      <GameScreen
        key={gameId}
        gameId={gameId}
        stream={stream}
        rules={rules}
        active={active}
        onNewGame={() => setGameId(null)}
      />
    );
  }

  return (
    <div className="analysis viewer">
      <div className="board-column">
        <Board board={emptyBoard()} premiums={rules.premiums} tileValues={rules.tileValues} />
      </div>
      <div className="side-column">
        <GameSetup busy={starting} error={startError} onStart={startGame} />
      </div>
    </div>
  );
}
