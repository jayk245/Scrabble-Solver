import type { AnalyzeResponse, Candidate, Effort } from '../api/types';
import { CandidateTable } from './CandidateTable';
import { SimChart } from './SimChart';

const EFFORTS: { value: Effort; label: string; title: string }[] = [
  { value: 'static', label: 'Static', title: 'Score plus leave value only; instant' },
  { value: 'quick', label: 'Quick', title: 'Short simulation (about 1 s)' },
  { value: 'normal', label: 'Normal', title: 'Simulation of 12 candidates (up to 2.5 s)' },
  { value: 'deep', label: 'Deep', title: 'Simulation of 20 candidates (up to 4.5 s)' },
];

const METHOD_LABEL = {
  static: 'Static evaluation',
  simulation: 'Monte Carlo simulation',
  endgame: 'Endgame solver',
} as const;

function formatSigned(value: number): string {
  return `${value >= 0 ? '+' : ''}${value.toFixed(1)}`;
}

export interface SolverResultsProps {
  moverName: string;
  analysis: AnalyzeResponse | null;
  loading: boolean;
  error: string | null;
  effort: Effort;
  onEffort: (effort: Effort) => void;
  /** Null when solving is possible; otherwise why not. */
  blocked: string | null;
  onSolve: () => void;
  autoSolve: boolean;
  onAutoSolve: (on: boolean) => void;
  hovered: Candidate | null;
  onHover: (candidate: Candidate | null) => void;
  onPlay: (candidate: Candidate) => void;
  playing: boolean;
}

/** Effort controls, the Solve button and the engine's ranked answer. */
export function SolverResults({
  moverName,
  analysis,
  loading,
  error,
  effort,
  onEffort,
  blocked,
  onSolve,
  autoSolve,
  onAutoSolve,
  hovered,
  onHover,
  onPlay,
  playing,
}: SolverResultsProps) {
  const best = analysis?.candidates.find((candidate) => candidate.chosen) ?? null;
  return (
    <section className="panel solver-panel" aria-labelledby="solver-heading">
      <div className="panel-header">
        <h2 id="solver-heading">Best move</h2>
        {analysis && <span className="status-text">{METHOD_LABEL[analysis.method]}</span>}
      </div>
      <div className="solver-controls">
        <div className="segmented" role="radiogroup" aria-label="Search effort">
          {EFFORTS.map((option) => (
            <button
              key={option.value}
              type="button"
              role="radio"
              aria-checked={effort === option.value}
              title={option.title}
              className={effort === option.value ? 'is-active' : undefined}
              onClick={() => onEffort(option.value)}
            >
              {option.label}
            </button>
          ))}
        </div>
        <button
          type="button"
          className="button-primary"
          disabled={blocked !== null || loading}
          onClick={onSolve}
        >
          {loading ? 'Solving…' : 'Solve'}
        </button>
        <label className="checkbox" title="Solve each new position as soon as its rack is entered">
          <input
            type="checkbox"
            checked={autoSolve}
            onChange={(event) => onAutoSolve(event.target.checked)}
          />
          Auto
        </label>
      </div>
      {blocked && !analysis && <p className="hint">{blocked}</p>}
      {error && <p className="verdict is-illegal">{error}</p>}
      {loading && !analysis && <p className="hint is-loading">The engine is thinking…</p>}

      {analysis && best && (
        <>
          <div className="best-move">
            <div>
              <span className="muted">{moverName} should play</span>
              <strong className="notation best-notation">{best.move.notation}</strong>
              <span className="muted">
                {best.move.score} points
                {best.move.leave !== undefined && <>, keeping {best.move.leave || 'nothing'}</>}
              </span>
            </div>
            <button
              type="button"
              className="button-primary"
              disabled={playing}
              onClick={() => onPlay(best)}
            >
              Play it
            </button>
          </div>

          {analysis.simulation && (
            <dl className="stat-row">
              <div>
                <dt>Iterations</dt>
                <dd>{analysis.simulation.iterations}</dd>
              </div>
              <div>
                <dt>Rollouts</dt>
                <dd>{analysis.simulation.rollouts.toLocaleString()}</dd>
              </div>
              <div>
                <dt>Plies</dt>
                <dd>{analysis.simulation.plies}</dd>
              </div>
              <div>
                <dt>Time</dt>
                <dd>{Math.round(analysis.elapsedMs)} ms</dd>
              </div>
            </dl>
          )}
          {analysis.endgame && (
            <>
              <dl className="stat-row">
                <div>
                  <dt>Final spread</dt>
                  <dd>{formatSigned(analysis.endgame.value)}</dd>
                </div>
                <div>
                  <dt>Search</dt>
                  <dd>{analysis.endgame.complete ? 'exact' : `depth ${analysis.endgame.depth}`}</dd>
                </div>
                <div>
                  <dt>Nodes</dt>
                  <dd>{analysis.endgame.nodes.toLocaleString()}</dd>
                </div>
                <div>
                  <dt>Time</dt>
                  <dd>{Math.round(analysis.elapsedMs)} ms</dd>
                </div>
              </dl>
              <p className="hint">
                Best line: <span className="notation">{analysis.endgame.pv.join(' · ')}</span>
                {!analysis.endgame.complete && ' (time limit reached; not proven optimal)'}
              </p>
            </>
          )}
          {analysis.method === 'static' && (
            <p className="hint">
              {analysis.totalMoves.toLocaleString()} legal moves, ranked by score plus the value of
              the tiles kept ({Math.round(analysis.elapsedMs)} ms).
            </p>
          )}

          {analysis.method === 'simulation' && (
            <SimChart
              candidates={analysis.candidates}
              hovered={hovered}
              onHover={onHover}
              chosenLabel="best"
            />
          )}
          <CandidateTable
            candidates={analysis.candidates}
            hovered={hovered}
            onHover={onHover}
            showSim={analysis.method === 'simulation'}
            chosenLabel="best"
            action={{ label: 'Play', onClick: onPlay, disabled: playing }}
          />
          <p className="hint">
            {analysis.method === 'simulation'
              ? 'Sim is the mean spread over the simulated futures (± 1 standard error); the best mean wins. '
              : ''}
            Hover a move to preview it on the board.
          </p>
        </>
      )}
    </section>
  );
}
