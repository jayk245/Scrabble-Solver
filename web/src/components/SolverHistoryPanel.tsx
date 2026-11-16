import type { SolverPosition } from '../hooks/useSolverHistory';

export interface SolverHistoryPanelProps {
  positions: SolverPosition[];
  index: number;
  onGoto: (index: number) => void;
  onUndo: () => void;
  onClear: () => void;
  onExport: () => void;
}

/**
 * The moves played in the solver, oldest first. Each row is the position
 * after that move; Back/Forward walk the list without changing it, Undo
 * drops the current position.
 */
export function SolverHistoryPanel({
  positions,
  index,
  onGoto,
  onUndo,
  onClear,
  onExport,
}: SolverHistoryPanelProps) {
  const last = positions.length - 1;
  return (
    <section className="panel" aria-labelledby="solver-history-heading">
      <div className="panel-header">
        <h2 id="solver-history-heading">Game record</h2>
      </div>
      {last === 0 ? (
        <p className="empty-state">No moves yet. Solve, then play the engine&apos;s move (or your own).</p>
      ) : (
        <ol className="game-log solver-log">
          <li className={index === 0 ? 'is-current' : undefined}>
            <button type="button" onClick={() => onGoto(0)} aria-current={index === 0}>
              <span className="game-log-turn">0</span>
              <span className="game-log-player">start</span>
              <span className="notation muted">set-up position</span>
              <span className="numeric" />
              <span className="numeric muted">
                {positions[0]?.scores[0]}–{positions[0]?.scores[1]}
              </span>
            </button>
          </li>
          {positions.slice(1).map((position, i) => {
            const at = i + 1;
            const origin = position.origin;
            if (!origin) return null;
            const classes = [`seat-${origin.player}`];
            if (at === index) classes.push('is-current');
            if (at > index) classes.push('is-future');
            return (
              <li key={at} className={classes.join(' ')}>
                <button type="button" onClick={() => onGoto(at)} aria-current={at === index}>
                  <span className="game-log-turn">{at}</span>
                  <span className="game-log-player">{position.names[origin.player]}</span>
                  <span className="notation">{origin.move.notation}</span>
                  <span className="numeric">+{origin.move.score}</span>
                  <span className="numeric muted">
                    {position.scores[0]}–{position.scores[1]}
                  </span>
                </button>
              </li>
            );
          })}
        </ol>
      )}
      <div className="button-row">
        <button
          type="button"
          className="button-ghost"
          onClick={() => onGoto(index - 1)}
          disabled={index === 0}
          title="Previous position (Alt+←)"
        >
          ◀ Back
        </button>
        <button
          type="button"
          className="button-ghost"
          onClick={() => onGoto(index + 1)}
          disabled={index === last}
          title="Next position (Alt+→)"
        >
          Forward ▶
        </button>
        <button type="button" className="button-ghost" onClick={onUndo} disabled={index === 0}>
          Undo move
        </button>
        <button type="button" className="button-ghost" onClick={onExport} disabled={last === 0}>
          Export GCG
        </button>
        <button type="button" className="button-ghost" onClick={onClear}>
          New position
        </button>
      </div>
      {index < last && (
        <p className="hint">Editing this earlier position discards the moves after it.</p>
      )}
    </section>
  );
}
