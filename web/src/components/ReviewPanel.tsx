import type { ReviewEntry } from '../api/types';

export interface ReviewPanelProps {
  review: ReviewEntry[];
  hintsUsed: number;
  hovered: ReviewEntry | null;
  onHover: (entry: ReviewEntry | null) => void;
}

/** Equity lost above which a move is flagged as a clear mistake. */
const MISTAKE = 8;

/**
 * Post-game review: each of the human's moves next to the engine's best
 * static move and the equity given up. Hovering a row shows the position
 * before that move with the engine's choice on the board.
 */
export function ReviewPanel({ review, hintsUsed, hovered, onHover }: ReviewPanelProps) {
  const lost = review.reduce((sum, entry) => sum + (entry.equityLost ?? 0), 0);
  const best = review.filter((entry) => (entry.equityLost ?? 0) === 0).length;
  return (
    <section className="panel" aria-labelledby="review-heading">
      <div className="panel-header">
        <h2 id="review-heading">Review</h2>
        <span className="status-text">static equity</span>
      </div>
      {review.length === 0 ? (
        <p className="empty-state">You made no moves this game.</p>
      ) : (
        <>
          <dl className="stat-row">
            <div>
              <dt>Best moves</dt>
              <dd>
                {best}/{review.length}
              </dd>
            </div>
            <div>
              <dt>Equity lost</dt>
              <dd>{lost.toFixed(1)}</dd>
            </div>
            <div>
              <dt>Per move</dt>
              <dd>{(lost / review.length).toFixed(1)}</dd>
            </div>
            <div>
              <dt>Hints</dt>
              <dd>{hintsUsed}</dd>
            </div>
          </dl>
          <div className="move-table-wrap candidate-table" onMouseLeave={() => onHover(null)}>
            <table className="move-table review-table">
              <thead>
                <tr>
                  <th className="numeric">Turn</th>
                  <th>You played</th>
                  <th>Engine&apos;s best</th>
                  <th className="numeric">Lost</th>
                </tr>
              </thead>
              <tbody>
                {review.map((entry) => {
                  const loss = entry.equityLost ?? 0;
                  const classes = [];
                  if (entry === hovered) classes.push('is-selected');
                  if (loss >= MISTAKE) classes.push('is-mistake');
                  return (
                    <tr
                      key={entry.turn}
                      className={classes.join(' ')}
                      onMouseEnter={() => onHover(entry)}
                    >
                      <td className="numeric muted">{entry.turn + 1}</td>
                      <td className="notation">
                        {entry.notation} <span className="muted">{entry.score}</span>
                      </td>
                      <td className="notation">
                        {entry.best && loss > 0 ? (
                          <>
                            {entry.best.move.notation}{' '}
                            <span className="muted">{entry.best.move.score}</span>
                          </>
                        ) : (
                          <span className="badge">best</span>
                        )}
                      </td>
                      <td className="numeric">{loss > 0 ? loss.toFixed(1) : '–'}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
          <p className="hint">
            Equity is score plus the engine&apos;s value of the tiles kept. Hover a row to see the
            engine&apos;s choice on the board as it was then.
          </p>
        </>
      )}
    </section>
  );
}
