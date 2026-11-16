import { useCallback, useEffect, useState } from 'react';

/** Delay between moves while playing, in milliseconds. */
export const SPEEDS = [
  { label: '0.5×', ms: 3000 },
  { label: '1×', ms: 1500 },
  { label: '2×', ms: 750 },
  { label: '4×', ms: 350 },
  { label: 'Max', ms: 60 },
] as const;

export interface Playback {
  /** Moves shown: 0 is the opening position, `total` the latest. */
  cursor: number;
  playing: boolean;
  speedMs: number;
  /** Jumps to a position, pausing playback. */
  seek: (cursor: number) => void;
  /** Steps by `delta` moves, pausing playback. */
  step: (delta: number) => void;
  toggle: () => void;
  setSpeedMs: (ms: number) => void;
}

/**
 * Client-side playback over a buffer of moves that may still be growing.
 *
 * The cursor is the only state; everything displayed is derived from
 * moves[cursor - 1]. While playing, a timer advances the cursor one move
 * per tick. If it catches up with a game that is still being computed
 * (`live`), it simply waits there and resumes as new moves arrive, which is
 * what makes "watch the bot think" work without any server-side pacing.
 */
export function usePlayback(total: number, live: boolean): Playback {
  const [cursor, setCursor] = useState(0);
  const [playing, setPlaying] = useState(true);
  const [speedMs, setSpeedMs] = useState<number>(SPEEDS[1].ms);

  const clamp = useCallback((n: number) => Math.min(total, Math.max(0, n)), [total]);

  useEffect(() => {
    if (!playing) return;
    if (cursor >= total) {
      // Caught up: wait for more moves, or stop at the end of the game.
      if (!live) setPlaying(false);
      return;
    }
    const timer = window.setTimeout(() => setCursor((c) => Math.min(c + 1, total)), speedMs);
    return () => window.clearTimeout(timer);
  }, [playing, cursor, total, live, speedMs]);

  const seek = useCallback(
    (next: number) => {
      setPlaying(false);
      setCursor(clamp(next));
    },
    [clamp],
  );

  const step = useCallback(
    (delta: number) => {
      setPlaying(false);
      setCursor((c) => clamp(c + delta));
    },
    [clamp],
  );

  const toggle = useCallback(() => {
    // Play from the start again once the end has been reached.
    if (!playing && cursor >= total && !live) setCursor(0);
    setPlaying(!playing);
  }, [playing, cursor, total, live]);

  return { cursor, playing, speedMs, seek, step, toggle, setSpeedMs };
}
