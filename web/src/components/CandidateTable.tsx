import type { Candidate } from '../api/types';

export interface CandidateTableProps {
  candidates: Candidate[];
  hovered: Candidate | null;
  onHover: (candidate: Candidate | null) => void;
  /** Adds the rollout mean ± standard error column. */
  showSim: boolean;
  chosenLabel: string;
  /** Adds a per-row action button (e.g. "Play" or "Use"). */
  action?: { label: string; onClick: (candidate: Candidate) => void; disabled?: boolean };
}

function formatSigned(value: number): string {
  return `${value >= 0 ? '+' : ''}${value.toFixed(1)}`;
}

/** Ranked engine candidates; hovering a row previews it on the board. */
export function CandidateTable({
  candidates,
  hovered,
  onHover,
  showSim,
  chosenLabel,
  action,
}: CandidateTableProps) {
  return (
    <div className="move-table-wrap candidate-table" onMouseLeave={() => onHover(null)}>
      <table className="move-table">
        <thead>
          <tr>
            <th className="numeric">#</th>
            <th>Move</th>
            <th className="numeric">Score</th>
            <th className="numeric">Equity</th>
            {showSim && <th className="numeric">Sim</th>}
            <th>Leave</th>
            {action && <th aria-label="Actions" />}
          </tr>
        </thead>
        <tbody>
          {candidates.map((candidate) => (
            <tr
              key={candidate.move.notation}
              className={[
                candidate.chosen ? 'is-chosen' : '',
                candidate === hovered ? 'is-selected' : '',
              ].join(' ')}
              onMouseEnter={() => onHover(candidate)}
              onFocus={() => onHover(candidate)}
            >
              <td className="numeric muted">{candidate.staticRank ?? '–'}</td>
              <td className="notation">
                {candidate.move.notation}
                {candidate.chosen && <span className="badge">{chosenLabel}</span>}
                {candidate.sim?.pruned && <span className="badge badge-muted">pruned</span>}
              </td>
              <td className="numeric score">{candidate.move.score}</td>
              <td className="numeric">{candidate.equity.toFixed(1)}</td>
              {showSim && (
                <td className="numeric">
                  {candidate.sim
                    ? `${formatSigned(candidate.sim.mean)} ± ${candidate.sim.stdError.toFixed(1)}`
                    : '–'}
                </td>
              )}
              <td className="leave">{candidate.move.leave || '—'}</td>
              {action && (
                <td className="row-action">
                  <button
                    type="button"
                    className="button-ghost"
                    disabled={action.disabled}
                    onClick={() => action.onClick(candidate)}
                  >
                    {action.label}
                  </button>
                </td>
              )}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
