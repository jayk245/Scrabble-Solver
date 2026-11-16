import type { Candidate, GameMoveEvent } from '../api/types';
import { CandidateTable } from './CandidateTable';
import { SimChart } from './SimChart';

export interface AnalysisPanelProps {
  /** The move whose decision is explained; null at the opening position. */
  event: GameMoveEvent | null;
  playerName: string;
  hovered: Candidate | null;
  onHover: (candidate: Candidate | null) => void;
}

const METHOD_LABEL = {
  static: 'Static evaluation',
  simulation: 'Monte Carlo simulation',
  endgame: 'Endgame solver',
} as const;

function formatSigned(value: number): string {
  return `${value >= 0 ? '+' : ''}${value.toFixed(1)}`;
}

/** Why the engine played (or would have played) the current move. */
export function AnalysisPanel({ event, playerName, hovered, onHover }: AnalysisPanelProps) {
  const analysis = event?.analysis;
  return (
    <section className="panel analysis-panel" aria-labelledby="analysis-heading">
      <div className="panel-header">
        <h2 id="analysis-heading">Analysis</h2>
        {analysis && <span className="status-text">{METHOD_LABEL[analysis.method]}</span>}
      </div>

      {!event && <p className="empty-state">Step forward to see how each move was chosen.</p>}
      {event && !analysis && (
        <p className="empty-state">
          {event.kind === 'play'
            ? 'No analysis: the record does not give this rack.'
            : 'Not a move; nothing to analyse.'}
        </p>
      )}

      {event && analysis && (
        <>
          <p className="analysis-summary">
            <strong>{playerName}</strong> played <strong className="notation">{event.label}</strong>{' '}
            for {event.score}.{' '}
            {analysis.playedRank !== null ? (
              <>
                Static rank <strong>#{analysis.playedRank}</strong> of{' '}
                {analysis.totalMoves.toLocaleString()} moves
                {analysis.playedEquity !== null && <> (equity {analysis.playedEquity.toFixed(1)})</>}.
              </>
            ) : (
              <>Not among the {analysis.totalMoves.toLocaleString()} moves the engine generates.</>
            )}
          </p>

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
                <dd>{(analysis.simulation.seconds * 1000).toFixed(0)} ms</dd>
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
              </dl>
              <p className="hint">
                Best line: <span className="notation">{analysis.endgame.pv.join(' · ')}</span>
              </p>
            </>
          )}

          {analysis.method === 'simulation' && (
            <SimChart candidates={analysis.candidates} hovered={hovered} onHover={onHover} />
          )}

          <CandidateTable
            candidates={analysis.candidates}
            hovered={hovered}
            onHover={onHover}
            showSim={analysis.method === 'simulation'}
            chosenLabel="played"
          />
          <p className="hint">Hover a candidate to preview it on the board.</p>
        </>
      )}

      {event?.warning && <p className="verdict is-illegal">{event.warning}</p>}
      {event?.note && <p className="hint">Note: {event.note}</p>}
    </section>
  );
}
