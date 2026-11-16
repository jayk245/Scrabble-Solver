import type { Seats } from '../api/types';
import { normaliseRack } from '../lib/rack';
import type { SolverPosition } from '../hooks/useSolverHistory';

export interface PositionPanelProps {
  position: SolverPosition;
  onRename: (names: Seats<string>) => void;
  onEdit: (patch: Partial<Omit<SolverPosition, 'origin' | 'analysis'>>) => void;
}

function setSeat<T>(pair: Seats<T>, seat: 0 | 1, value: T): Seats<T> {
  return seat === 0 ? [value, pair[1]] : [pair[0], value];
}

/**
 * The game state around the board: both players, their scores, who is to
 * move and, optionally, the opponent's rack when it is known (for example
 * when going over a finished game).
 */
export function PositionPanel({ position, onRename, onEdit }: PositionPanelProps) {
  const mover = position.toMove;
  const opponent: 0 | 1 = mover === 0 ? 1 : 0;
  return (
    <section className="panel" aria-labelledby="position-heading">
      <div className="panel-header">
        <h2 id="position-heading">Position</h2>
        {position.scoreless > 0 && (
          <span className="status-text">
            {position.scoreless} scoreless turn{position.scoreless === 1 ? '' : 's'}
          </span>
        )}
      </div>
      <div className="players-grid">
        {([0, 1] as const).map((seat) => (
          <div key={seat} className={`player-edit${seat === mover ? ' is-to-move' : ''}`}>
            <label className="field">
              <span>Player {seat + 1}</span>
              <input
                className="text-input"
                value={position.names[seat]}
                maxLength={24}
                onChange={(event) => onRename(setSeat(position.names, seat, event.target.value))}
              />
            </label>
            <label className="field">
              <span>Score</span>
              <input
                className="text-input"
                type="number"
                value={position.scores[seat]}
                onChange={(event) => {
                  const value = Number.parseInt(event.target.value, 10);
                  onEdit({ scores: setSeat(position.scores, seat, Number.isNaN(value) ? 0 : value) });
                }}
              />
            </label>
            <button
              type="button"
              className={`button-ghost toggle${seat === mover ? ' is-on' : ''}`}
              aria-pressed={seat === mover}
              onClick={() => onEdit({ toMove: seat })}
            >
              {seat === mover ? 'To move' : 'Set to move'}
            </button>
          </div>
        ))}
      </div>

      <label className="checkbox">
        <input
          type="checkbox"
          checked={position.opponentKnown}
          onChange={(event) => onEdit({ opponentKnown: event.target.checked })}
        />
        I know {position.names[opponent] || 'the opponent'}&apos;s rack
      </label>
      {position.opponentKnown && (
        <input
          className="text-input rack-text"
          value={position.racks[opponent]}
          onChange={(event) =>
            onEdit({ racks: setSeat(position.racks, opponent, normaliseRack(event.target.value)) })
          }
          placeholder="Opponent's tiles"
          aria-label="Opponent's rack"
          spellCheck={false}
          autoComplete="off"
        />
      )}
      <p className="hint">
        {position.opponentKnown
          ? 'The known rack is fixed in every simulated future and removed from the bag.'
          : 'Unknown: the engine samples the opponent’s rack from the unseen tiles. Once the bag is empty it is exactly the unseen tiles.'}
      </p>
    </section>
  );
}
