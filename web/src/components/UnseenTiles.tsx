import type { PositionSummary } from '../api/types';

const LETTERS = [...'ABCDEFGHIJKLMNOPQRSTUVWXYZ?'];

export interface UnseenTilesProps {
  position: PositionSummary | null;
}

/** Tiles not on the board or the rack, with bag and board counts. */
export function UnseenTiles({ position }: UnseenTilesProps) {
  return (
    <section className="panel" aria-labelledby="unseen-heading">
      <div className="panel-header">
        <h2 id="unseen-heading">Unseen tiles</h2>
      </div>
      <dl className="stat-row">
        <div>
          <dt>Unseen</dt>
          <dd>{position?.unseenCount ?? '–'}</dd>
        </div>
        <div>
          <dt>In bag</dt>
          <dd>{position?.bagCount ?? '–'}</dd>
        </div>
        <div>
          <dt>On board</dt>
          <dd>{position?.tilesOnBoard ?? '–'}</dd>
        </div>
      </dl>
      <div className="unseen-grid">
        {LETTERS.map((letter) => {
          const count = position?.unseen[letter] ?? 0;
          return (
            <span key={letter} className={`unseen-cell${count === 0 ? ' is-gone' : ''}`}>
              <span className="unseen-letter">{letter}</span>
              <span className="unseen-count">{count}</span>
            </span>
          );
        })}
      </div>
      {position && position.overused.length > 0 && (
        <p className="verdict is-illegal">
          More {[...position.overused].join(', ')} than the bag contains.
        </p>
      )}
      <p className="hint">The bag count assumes the opponent holds a full rack.</p>
    </section>
  );
}
