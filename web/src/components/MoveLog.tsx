import type { MoveInfo } from '../api/types';

export interface MoveLogProps {
  moves: MoveInfo[];
  onUndo: () => void;
  onClear: () => void;
  canClear: boolean;
}

/** Moves played on the analysis board this session, oldest first. */
export function MoveLog({ moves, onUndo, onClear, canClear }: MoveLogProps) {
  const total = moves.reduce((sum, move) => sum + move.score, 0);
  return (
    <section className="panel" aria-labelledby="log-heading">
      <div className="panel-header">
        <h2 id="log-heading">Played</h2>
        <div className="button-row">
          <button
            type="button"
            className="button-ghost"
            onClick={onUndo}
            disabled={moves.length === 0}
            title="Undo (Ctrl+Z)"
          >
            Undo
          </button>
          <button type="button" className="button-ghost" onClick={onClear} disabled={!canClear}>
            Clear board
          </button>
        </div>
      </div>
      {moves.length === 0 ? (
        <p className="empty-state">No moves played yet.</p>
      ) : (
        <ol className="move-log">
          {moves.map((move, i) => (
            <li key={i}>
              <span className="notation">{move.notation}</span>
              <span className="numeric">{move.score}</span>
            </li>
          ))}
        </ol>
      )}
      {moves.length > 0 && <p className="hint">Total {total} points.</p>}
    </section>
  );
}
