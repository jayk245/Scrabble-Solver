import type { Candidate } from '../api/types';

export interface SimChartProps {
  /** Simulated candidates, best mean first; entries without `sim` are skipped. */
  candidates: Candidate[];
  hovered: Candidate | null;
  onHover: (candidate: Candidate | null) => void;
}

function formatSigned(value: number): string {
  return `${value >= 0 ? '+' : ''}${value.toFixed(1)}`;
}

/**
 * Horizontal bar chart of each candidate's mean rollout value with a
 * ±1 standard-error whisker. One series, so one hue: the played move is
 * the darker step and labelled, pruned candidates are faded and labelled,
 * so neither distinction relies on colour alone. Built from HTML rather
 * than SVG so labels stay crisp and wrap with the panel width.
 */
export function SimChart({ candidates, hovered, onHover }: SimChartProps) {
  const rows = candidates.filter((c) => c.sim !== undefined);
  if (rows.length === 0) return null;

  // Domain covers every whisker and always includes zero, the baseline.
  let lo = 0;
  let hi = 0;
  for (const { sim } of rows) {
    if (!sim) continue;
    lo = Math.min(lo, sim.mean - sim.stdError);
    hi = Math.max(hi, sim.mean + sim.stdError);
  }
  const span = hi - lo || 1;
  const pct = (value: number) => ((value - lo) / span) * 100;
  const zero = pct(0);

  return (
    <div className="sim-chart" role="table" aria-label="Simulation results">
      <div className="sim-chart-head" role="row">
        <span role="columnheader">Move</span>
        <span role="columnheader">
          Mean rollout value <span className="muted">(± 1 s.e.)</span>
        </span>
        <span role="columnheader" className="numeric">
          Mean
        </span>
      </div>
      {rows.map((candidate) => {
        const sim = candidate.sim!;
        const start = Math.min(zero, pct(sim.mean));
        const width = Math.abs(pct(sim.mean) - zero);
        const negative = sim.mean < 0;
        const classes = ['sim-row'];
        if (candidate.chosen) classes.push('is-chosen');
        if (sim.pruned) classes.push('is-pruned');
        if (candidate === hovered) classes.push('is-hovered');
        return (
          <div
            key={candidate.move.notation}
            className={classes.join(' ')}
            role="row"
            onMouseEnter={() => onHover(candidate)}
            onMouseLeave={() => onHover(null)}
            title={`${candidate.move.notation}: mean ${sim.mean.toFixed(2)} ± ${sim.stdError.toFixed(2)} over ${sim.iterations} iterations${sim.pruned ? ' (pruned)' : ''}`}
          >
            <span className="sim-label notation" role="cell">
              {candidate.move.notation}
              {candidate.chosen && <span className="badge">played</span>}
              {sim.pruned && <span className="badge badge-muted">pruned</span>}
            </span>
            <span className="sim-track" role="cell">
              <span className="sim-zero" style={{ left: `${zero}%` }} />
              <span
                className={`sim-bar${negative ? ' is-negative' : ''}`}
                style={{ left: `${start}%`, width: `${width}%` }}
              />
              <span
                className="sim-whisker"
                style={{
                  left: `${pct(sim.mean - sim.stdError)}%`,
                  width: `${pct(sim.mean + sim.stdError) - pct(sim.mean - sim.stdError)}%`,
                }}
              />
            </span>
            <span className="numeric" role="cell">
              {formatSigned(sim.mean)}
            </span>
          </div>
        );
      })}
    </div>
  );
}
