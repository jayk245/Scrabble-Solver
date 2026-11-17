import type { Seats } from '../api/types';
import { setSeat, type GameNode, type RackPatch, type SetupPatch } from '../lib/gameTree';
import { normaliseRack } from '../lib/rack';

export interface PositionPanelProps {
  names: Seats<string>;
  node: GameNode;
  /** Scores and the player to move can be changed only in Edit board mode. */
  editable: boolean;
  showRacks: boolean;
  onRename: (names: Seats<string>) => void;
  onSetup: (patch: SetupPatch) => void;
  onRacks: (patch: RackPatch) => void;
}

/**
 * The game state around the board: both players, their scores, who is to
 * move and, optionally, the opponent's rack when it is known (for example
 * when going over a finished game).
 */
export function PositionPanel({
  names,
  node,
  editable,
  showRacks,
  onRename,
  onSetup,
  onRacks,
}: PositionPanelProps) {
  const mover = node.toMove;
  const opponent: 0 | 1 = mover === 0 ? 1 : 0;
  return (
    <section className="panel" aria-labelledby="position-heading">
      <div className="panel-header">
        <h2 id="position-heading">Position</h2>
        {node.scoreless > 0 && (
          <span className="status-text">
            {node.scoreless} scoreless turn{node.scoreless === 1 ? '' : 's'}
          </span>
        )}
      </div>
      <div className="players-grid">
        {([0, 1] as const).map((seat) => (
          <div
            key={seat}
            className={`player-edit seat-${seat}${seat === mover ? ' is-to-move' : ''}`}
          >
            <label className="field">
              <span>Player {seat + 1}</span>
              <input
                className="text-input"
                value={names[seat]}
                maxLength={24}
                onChange={(event) => onRename(setSeat(names, seat, event.target.value))}
              />
            </label>
            <label className="field">
              <span>Score</span>
              <input
                className="text-input"
                type="number"
                value={node.scores[seat]}
                disabled={!editable}
                title={editable ? undefined : 'Switch to Edit board to change scores'}
                onChange={(event) => {
                  const value = Number.parseInt(event.target.value, 10);
                  onSetup({ scores: setSeat(node.scores, seat, Number.isNaN(value) ? 0 : value) });
                }}
              />
            </label>
            <button
              type="button"
              className={`button-ghost toggle${seat === mover ? ' is-on' : ''}`}
              aria-pressed={seat === mover}
              disabled={!editable && seat !== mover}
              title={editable ? undefined : 'Switch to Edit board to change who moves'}
              onClick={() => onSetup({ toMove: seat })}
            >
              {seat === mover ? 'To move' : 'Set to move'}
            </button>
          </div>
        ))}
      </div>

      {showRacks && (
        <>
          <label className="checkbox">
            <input
              type="checkbox"
              checked={node.opponentKnown}
              onChange={(event) => onRacks({ opponentKnown: event.target.checked })}
            />
            I know {names[opponent] || 'the opponent'}&apos;s rack
          </label>
          {node.opponentKnown && (
            <input
              className="text-input rack-text"
              value={node.racks[opponent]}
              onChange={(event) =>
                onRacks({ racks: setSeat(node.racks, opponent, normaliseRack(event.target.value)) })
              }
              placeholder="Opponent's tiles"
              aria-label="Opponent's rack"
              spellCheck={false}
              autoComplete="off"
            />
          )}
          <p className="hint">
            {node.opponentKnown
              ? 'The known rack is fixed in every simulated future and removed from the bag.'
              : 'Unknown: the engine samples the opponent’s rack from the unseen tiles. Once the bag is empty it is exactly the unseen tiles.'}
          </p>
        </>
      )}
    </section>
  );
}
