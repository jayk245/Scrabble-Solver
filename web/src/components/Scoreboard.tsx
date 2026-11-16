import type { GamePlayer, Seats } from '../api/types';
import { RackTiles } from './RackTiles';

export interface ScoreboardProps {
  players: Seats<GamePlayer>;
  scores: Seats<number>;
  racks: Seats<string | null>;
  /** Seat to move next; null once the game is over. */
  toMove: 0 | 1 | null;
  /** Seat that just moved, for its score delta. */
  lastMover: 0 | 1 | null;
  lastScore: number | null;
  bag: number;
  winner: 0 | 1 | -1 | null;
  tileValues: Record<string, number>;
}

function describe(player: GamePlayer): string {
  if (player.fullName && player.fullName !== player.name) return player.fullName;
  if (!player.leaves) return '';
  const parts = [`${player.leaves} leaves`];
  if (player.simulation?.enabled) {
    parts.push(
      `sim ${player.simulation.candidates}×${player.simulation.iterations}×${player.simulation.plies}`,
    );
  }
  if (player.endgame?.enabled) parts.push('endgame');
  return parts.join(' · ');
}

/** Both players' scores and racks, with the bag count between them. */
export function Scoreboard({
  players,
  scores,
  racks,
  toMove,
  lastMover,
  lastScore,
  bag,
  winner,
  tileValues,
}: ScoreboardProps) {
  return (
    <section className="scoreboard" aria-label="Score">
      {([0, 1] as const).map((seat) => {
        const classes = ['player-card'];
        if (seat === toMove) classes.push('is-to-move');
        if (seat === winner) classes.push('is-winner');
        return (
          <div key={seat} className={classes.join(' ')}>
            <div className="player-head">
              <span className="player-name">
                {players[seat].name}
                {seat === toMove && <span className="badge">to move</span>}
                {seat === winner && <span className="badge">winner</span>}
              </span>
              <span className="player-score">
                {scores[seat]}
                {seat === lastMover && lastScore !== null && (
                  <span className="score-delta">
                    {lastScore >= 0 ? `+${lastScore}` : lastScore}
                  </span>
                )}
              </span>
            </div>
            <span className="player-meta">{describe(players[seat])}</span>
            <RackTiles rack={racks[seat]} tileValues={tileValues} compact />
          </div>
        );
      })}
      <div className="bag-count" aria-label={`${bag} tiles in the bag`}>
        <span className="bag-number">{bag}</span>
        <span className="bag-label">in bag</span>
      </div>
    </section>
  );
}
