import { SPEEDS, type Playback } from '../hooks/usePlayback';

export interface PlaybackControlsProps {
  playback: Playback;
  total: number;
  live: boolean;
}

/**
 * Transport bar: first / back / play-pause / forward / last, a scrubber and
 * the speed selector. Keyboard shortcuts are bound by the view.
 */
export function PlaybackControls({ playback, total, live }: PlaybackControlsProps) {
  const { cursor, playing, speedMs, seek, step, toggle, setSpeedMs } = playback;
  return (
    <div className="playback" role="group" aria-label="Playback">
      <div className="button-row">
        <button type="button" className="button-ghost" onClick={() => seek(0)} title="Start (Home)">
          ⏮
        </button>
        <button type="button" className="button-ghost" onClick={() => step(-1)} title="Back (←)">
          ◀
        </button>
        <button
          type="button"
          className="button-primary play-button"
          onClick={toggle}
          title="Play / pause (Space)"
        >
          {playing ? 'Pause' : 'Play'}
        </button>
        <button type="button" className="button-ghost" onClick={() => step(1)} title="Forward (→)">
          ▶
        </button>
        <button type="button" className="button-ghost" onClick={() => seek(total)} title="Latest (End)">
          ⏭
        </button>
      </div>
      <input
        className="scrubber"
        type="range"
        min={0}
        max={total}
        value={cursor}
        onChange={(event) => seek(Number(event.target.value))}
        aria-label="Move"
      />
      <span className="status-text">
        {cursor}/{total}
        {live ? '+' : ''}
      </span>
      <select
        className="text-input speed-select"
        value={speedMs}
        onChange={(event) => setSpeedMs(Number(event.target.value))}
        aria-label="Speed"
      >
        {SPEEDS.map((speed) => (
          <option key={speed.ms} value={speed.ms}>
            {speed.label}
          </option>
        ))}
      </select>
    </div>
  );
}
