import { useState } from 'react';

import type { Effort, Seats } from '../api/types';
import type { ReviewProgress } from '../hooks/useGameReview';
import {
  GRADE_LABEL,
  GRADE_LIMITS,
  GRADE_SYMBOL,
  type Grade,
  type ReviewedMove,
} from '../lib/review';

const EFFORTS: { value: Effort; label: string; seconds: number }[] = [
  { value: 'static', label: 'Static', seconds: 0.05 },
  { value: 'quick', label: 'Quick', seconds: 1 },
  { value: 'normal', label: 'Normal', seconds: 2.5 },
  { value: 'deep', label: 'Deep', seconds: 4.5 },
];

const COUNTED: Grade[] = ['inaccuracy', 'mistake', 'blunder'];

export interface GameReviewPanelProps {
  names: Seats<string>;
  moves: ReviewedMove[];
  progress: ReviewProgress | null;
  currentId: string;
  onRun: (effort: Effort) => void;
  onCancel: () => void;
  onGoto: (id: string) => void;
}

function Summary({ names, moves }: { names: Seats<string>; moves: ReviewedMove[] }) {
  return (
    <table className="move-table review-summary">
      <thead>
        <tr>
          <th>Player</th>
          <th className="numeric" title="Average points given up per graded move">
            Avg loss
          </th>
          {COUNTED.map((grade) => (
            <th key={grade} className="numeric" title={GRADE_LABEL[grade]}>
              {GRADE_SYMBOL[grade]}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {([0, 1] as const).map((seat) => {
          const graded = moves.filter((move) => move.player === seat && move.grade);
          const total = graded.reduce((sum, move) => sum + move.grade!.lost, 0);
          return (
            <tr key={seat}>
              <td>
                <span className={`seat-swatch seat-${seat}`} aria-hidden="true" />
                {names[seat]}
              </td>
              <td className="numeric">
                {graded.length ? (total / graded.length).toFixed(1) : '–'}
              </td>
              {COUNTED.map((grade) => (
                <td key={grade} className="numeric">
                  {graded.filter((move) => move.grade!.grade === grade).length}
                </td>
              ))}
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

/**
 * Points lost per move across the game: one bar per move, the first
 * player's above the axis and the second's below, so who erred is read
 * from position as well as colour. Bars of inaccuracies and worse carry
 * their symbol; clicking a bar shows that move.
 */
function LossChart({
  moves,
  currentId,
  onGoto,
}: {
  moves: ReviewedMove[];
  currentId: string;
  onGoto: (id: string) => void;
}) {
  const [hovered, setHovered] = useState<ReviewedMove | null>(null);
  if (moves.length === 0) return null;
  const width = 600;
  const height = 160;
  const mid = height / 2;
  const reach = mid - 14;
  const max = Math.max(GRADE_LIMITS.mistake, ...moves.map((move) => move.grade?.lost ?? 0));
  const slot = width / moves.length;
  const bar = Math.max(2, Math.min(14, slot - 2));
  const scale = (lost: number) => Math.max(lost > 0 ? 2 : 0, (lost / max) * reach);

  return (
    <div className="loss-chart">
      <div className="loss-plot">
        <svg
          viewBox={`0 0 ${width} ${height}`}
          role="img"
          aria-label="Points lost per move"
          onMouseLeave={() => setHovered(null)}
        >
          {[GRADE_LIMITS.inaccuracy, GRADE_LIMITS.mistake].map((limit) => (
            <g key={limit} className="loss-grid">
              <line x1={0} x2={width} y1={mid - scale(limit)} y2={mid - scale(limit)} />
              <line x1={0} x2={width} y1={mid + scale(limit)} y2={mid + scale(limit)} />
            </g>
          ))}
          <line className="loss-axis" x1={0} x2={width} y1={mid} y2={mid} />
          {moves.map((move, i) => {
            const x = i * slot + slot / 2;
            const lost = move.grade?.lost ?? 0;
            const h = scale(lost);
            const up = move.player === 0;
            const current = move.nodeId === currentId;
            return (
              <g
                key={move.nodeId}
                className={`loss-bar seat-${move.player}${current ? ' is-current' : ''}${move.grade ? '' : ' is-missing'}`}
                onMouseEnter={() => setHovered(move)}
                onClick={() => onGoto(move.nodeId)}
              >
                {/* A hit target the full height of the slot, wider than the bar. */}
                <rect className="loss-hit" x={i * slot} y={0} width={slot} height={height} />
                {current && <line className="loss-cursor" x1={x} x2={x} y1={0} y2={height} />}
                {move.grade ? (
                  <rect
                    x={x - bar / 2}
                    y={up ? mid - h : mid}
                    width={bar}
                    height={h}
                    rx={Math.min(2, bar / 2)}
                  />
                ) : (
                  <circle cx={x} cy={mid} r={1.5} />
                )}
              </g>
            );
          })}
        </svg>
        <div className="loss-labels" aria-hidden="true">
          {moves.map((move, i) => {
            const symbol = move.grade ? GRADE_SYMBOL[move.grade.grade] : '';
            if (!symbol) return null;
            const top = move.player === 0;
            return (
              <span
                key={move.nodeId}
                className={`loss-symbol${top ? ' is-top' : ' is-bottom'}`}
                style={{ left: `${((i + 0.5) / moves.length) * 100}%` }}
              >
                {symbol}
              </span>
            );
          })}
        </div>
      </div>
      <p className="loss-tooltip" aria-live="polite">
        {hovered ? (
          hovered.grade ? (
            <>
              <strong className="notation">{hovered.notation}</strong>{' '}
              {GRADE_LABEL[hovered.grade.grade]}, {hovered.grade.lost.toFixed(1)} points below{' '}
              <span className="notation">{hovered.grade.best.move.notation}</span> (
              {hovered.grade.basis})
            </>
          ) : (
            <>
              <strong className="notation">{hovered.notation}</strong>{' '}
              {hovered.skipped ? `not graded: ${hovered.skipped}` : 'not analysed yet'}
            </>
          )
        ) : (
          'Hover a bar for the move; click to go there.'
        )}
      </p>
    </div>
  );
}

/**
 * Whole-game analysis, like a chess engine's game review: every main-line
 * move with a known rack is compared with the engine's choice, graded,
 * summarised per player and charted across the game.
 */
export function GameReviewPanel({
  names,
  moves,
  progress,
  currentId,
  onRun,
  onCancel,
  onGoto,
}: GameReviewPanelProps) {
  const [effort, setEffort] = useState<Effort>('quick');
  const gradable = moves.filter((move) => !move.skipped || move.grade).length;
  const graded = moves.filter((move) => move.grade).length;
  const running = progress?.running ?? false;
  const estimate = Math.ceil(
    gradable * (EFFORTS.find((option) => option.value === effort)?.seconds ?? 1),
  );

  return (
    <section className="panel review-panel" aria-labelledby="review-heading">
      <div className="panel-header">
        <h2 id="review-heading">Game review</h2>
        {graded > 0 && (
          <span className="status-text">
            {graded} of {moves.length} moves graded
          </span>
        )}
      </div>
      <div className="solver-controls">
        <div className="segmented" role="radiogroup" aria-label="Review effort">
          {EFFORTS.map((option) => (
            <button
              key={option.value}
              type="button"
              role="radio"
              aria-checked={effort === option.value}
              className={effort === option.value ? 'is-active' : undefined}
              disabled={running}
              onClick={() => setEffort(option.value)}
            >
              {option.label}
            </button>
          ))}
        </div>
        {running ? (
          <button type="button" className="button-secondary" onClick={onCancel}>
            Cancel
          </button>
        ) : (
          <button
            type="button"
            className="button-primary"
            disabled={gradable === 0}
            onClick={() => onRun(effort)}
            title={`About ${estimate} s for ${gradable} positions`}
          >
            Review game
          </button>
        )}
      </div>
      {progress && (
        <div className="review-progress">
          <progress max={Math.max(1, progress.total)} value={progress.done} />
          <span className="hint">
            {progress.running
              ? `Analysing ${progress.done + 1 > progress.total ? progress.total : progress.done + 1} of ${progress.total} (${progress.effort})…`
              : `Analysed ${progress.done} of ${progress.total} positions (${progress.effort}).`}
            {progress.failed > 0 && ` ${progress.failed} could not be analysed.`}
          </span>
        </div>
      )}
      {moves.length === 0 ? (
        <p className="empty-state">The main line has no moves to review yet.</p>
      ) : gradable === 0 ? (
        <p className="hint">
          No move has a known rack, so there is nothing to compare. Enter racks as you record moves,
          or import a GCG with racks.
        </p>
      ) : graded === 0 ? (
        <p className="hint">
          Review the game to compare {gradable} move{gradable === 1 ? '' : 's'} with the
          engine&apos;s choice.
        </p>
      ) : (
        <>
          <Summary names={names} moves={moves} />
          <LossChart moves={moves} currentId={currentId} onGoto={onGoto} />
          <ul className="chart-legend">
            {([0, 1] as const).map((seat) => (
              <li key={seat}>
                <span className={`seat-swatch seat-${seat}`} aria-hidden="true" />
                {names[seat]} {seat === 0 ? '(above)' : '(below)'}
              </li>
            ))}
          </ul>
        </>
      )}
      <p className="hint">
        Points lost against the engine&apos;s move: simulated mean spread when the review simulates,
        exact endgame value with the bag empty, otherwise static equity. Best &lt;{' '}
        {GRADE_LIMITS.best}, Good &lt; {GRADE_LIMITS.good}, Inaccuracy (?!) &lt;{' '}
        {GRADE_LIMITS.inaccuracy}, Mistake (?) &lt; {GRADE_LIMITS.mistake}, Blunder (??) beyond.
        {gradable < moves.length &&
          ` ${moves.length - gradable} of ${moves.length} moves cannot be graded (rack unknown, or not a legal play for it).`}
      </p>
    </section>
  );
}
