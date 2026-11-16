import { useEffect, useRef } from 'react';

import type { GameMoveEvent, Seats } from '../api/types';

export interface GameLogProps {
  moves: GameMoveEvent[];
  names: Seats<string>;
  /** Moves currently shown on the board (0 = opening position). */
  cursor: number;
  onSeek: (cursor: number) => void;
  /** Whether more moves are still being computed. */
  live: boolean;
  /** Shown under the list while `live`. */
  thinkingLabel?: string;
}

/**
 * Every move of the game; clicking one shows the position after it. The
 * row for the current position stays scrolled into view during playback.
 */
export function GameLog({
  moves,
  names,
  cursor,
  onSeek,
  live,
  thinkingLabel = 'The bots are thinking…',
}: GameLogProps) {
  const list = useRef<HTMLOListElement>(null);
  const current = useRef<HTMLLIElement>(null);
  // Scrolls only the list, never the page (scrollIntoView would scroll
  // every scrollable ancestor, including the window).
  useEffect(() => {
    const container = list.current;
    const row = current.current;
    if (!container || !row) return;
    // The list is position: relative, so offsetTop is relative to it.
    const top = row.offsetTop;
    if (top < container.scrollTop) {
      container.scrollTop = top;
    } else if (top + row.offsetHeight > container.scrollTop + container.clientHeight) {
      container.scrollTop = top + row.offsetHeight - container.clientHeight;
    }
  }, [cursor]);

  return (
    <section className="panel" aria-labelledby="game-log-heading">
      <div className="panel-header">
        <h2 id="game-log-heading">Moves</h2>
        <button
          type="button"
          className="button-ghost"
          onClick={() => onSeek(0)}
          disabled={cursor === 0}
        >
          Opening
        </button>
      </div>
      <ol className="game-log" ref={list}>
        {moves.map((event, i) => {
          const active = i + 1 === cursor;
          const classes = [`seat-${event.player}`];
          if (active) classes.push('is-current');
          if (i + 1 > cursor) classes.push('is-future');
          if (event.kind !== 'play') classes.push('is-aside');
          return (
            <li key={event.turn} ref={active ? current : undefined} className={classes.join(' ')}>
              <button type="button" onClick={() => onSeek(i + 1)} aria-current={active}>
                <span className="game-log-turn">{event.turn + 1}</span>
                <span className="game-log-player">{names[event.player]}</span>
                <span className="notation">
                  {event.label}
                  {event.move?.bingo && <span className="badge">bingo</span>}
                  {event.warning && (
                    <span className="badge badge-warning" title={event.warning}>
                      !
                    </span>
                  )}
                </span>
                <span className="numeric">{event.score >= 0 ? `+${event.score}` : event.score}</span>
                <span className="numeric muted">
                  {event.scores[0]}–{event.scores[1]}
                </span>
              </button>
            </li>
          );
        })}
      </ol>
      {moves.length === 0 && <p className="empty-state">Waiting for the first move…</p>}
      {live && moves.length > 0 && thinkingLabel && (
        <p className="hint is-loading">{thinkingLabel}</p>
      )}
    </section>
  );
}
