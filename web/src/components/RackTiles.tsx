import { RACK_SIZE } from '../lib/rack';

export interface RackTilesProps {
  /** Rack string ('?' = blank); null renders an unknown rack. */
  rack: string | null;
  tileValues: Record<string, number>;
  /** Number of slots drawn; empty slots pad the rack to this size. */
  slots?: number;
  compact?: boolean;
}

/** A rack drawn as tiles, padded with empty slots. Presentational only. */
export function RackTiles({ rack, tileValues, slots = RACK_SIZE, compact = false }: RackTilesProps) {
  const tiles = Array.from({ length: slots }, (_, i) => rack?.[i] ?? null);
  return (
    <div
      className={`rack-tiles${compact ? ' is-compact' : ''}${rack === null ? ' is-unknown' : ''}`}
      aria-label={rack === null ? 'Rack unknown' : `Rack ${rack || 'empty'}`}
      role="img"
    >
      {tiles.map((tile, i) =>
        tile ? (
          <span key={i} className={`rack-tile${tile === '?' ? ' is-blank' : ''}`}>
            <span className="tile-letter">{tile === '?' ? '' : tile}</span>
            {tile !== '?' && <span className="tile-value">{tileValues[tile]}</span>}
          </span>
        ) : (
          <span key={i} className="rack-tile is-empty" />
        ),
      )}
    </div>
  );
}
