import { useEffect, useState } from 'react';

import { gameEventsUrl } from '../api/client';
import type { GameEndEvent, GameMoveEvent, GameStartEvent } from '../api/types';

export type StreamStatus = 'idle' | 'connecting' | 'streaming' | 'done' | 'error';

export interface GameStream {
  status: StreamStatus;
  start: GameStartEvent | null;
  /** Every move received so far, in turn order. */
  moves: GameMoveEvent[];
  end: GameEndEvent | null;
  error: string | null;
}

const IDLE: GameStream = { status: 'idle', start: null, moves: [], end: null, error: null };

/**
 * Subscribes to a game's Server-Sent Events stream and buffers every event.
 *
 * The server sends events as fast as the bots decide; playback speed is a
 * purely client-side concern (see usePlayback), so this hook only
 * accumulates. EventSource reconnects by itself after a dropped connection
 * and sends Last-Event-ID, so the server resumes where we left off; the
 * turn check below makes a replayed event harmless anyway. The stream is
 * closed explicitly on "end", since EventSource would otherwise treat the
 * server closing the response as a drop and reconnect.
 */
export function useGameStream(gameId: string | null): GameStream {
  const [stream, setStream] = useState<GameStream>(IDLE);

  useEffect(() => {
    if (!gameId) {
      setStream(IDLE);
      return;
    }
    setStream({ ...IDLE, status: 'connecting' });
    const source = new EventSource(gameEventsUrl(gameId));

    const parse = <T,>(event: Event): T => JSON.parse((event as MessageEvent<string>).data) as T;

    source.addEventListener('start', (event) => {
      const start = parse<GameStartEvent>(event);
      setStream((s) => ({ ...s, status: 'streaming', start }));
    });
    source.addEventListener('move', (event) => {
      const move = parse<GameMoveEvent>(event);
      setStream((s) =>
        move.turn < s.moves.length ? s : { ...s, status: 'streaming', moves: [...s.moves, move] },
      );
    });
    source.addEventListener('end', (event) => {
      const end = parse<GameEndEvent>(event);
      source.close();
      setStream((s) => ({
        ...s,
        status: end.reason === 'error' ? 'error' : 'done',
        end,
        error: end.reason === 'error' ? (end.message ?? 'The game failed.') : null,
      }));
    });
    source.onerror = () => {
      // CONNECTING means the browser is retrying on its own; CLOSED means it
      // gave up (e.g. a 404 for an expired game).
      if (source.readyState === EventSource.CLOSED) {
        setStream((s) =>
          s.status === 'done'
            ? s
            : { ...s, status: 'error', error: 'Lost the connection to the game stream.' },
        );
      }
    };

    return () => source.close();
  }, [gameId]);

  return stream;
}
