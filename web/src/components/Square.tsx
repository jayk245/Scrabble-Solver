import { memo } from 'react';

import type { Direction } from '../api/types';
import { isBlankTile, squareName } from '../lib/board';

/** Premium codes as sent in Rules.premiums. */
const PREMIUM_CLASS: Record<string, string> = {
  T: 'premium-tw',
  D: 'premium-dw',
  t: 'premium-tl',
  d: 'premium-dl',
  '*': 'premium-centre',
};

const PREMIUM_LABEL: Record<string, string> = {
  T: '3W',
  D: '2W',
  t: '3L',
  d: '2L',
  '*': '★',
};

/** How a square takes part in the move being previewed. */
export type PreviewRole = 'placed' | 'through' | null;

export interface SquareProps {
  row: number;
  col: number;
  premium: string;
  /** Board character: '.', 'A'-'Z', or 'a'-'z' for a blank. */
  tile: string;
  /** A tile the previewed move would place here (board character format). */
  previewTile: string | null;
  previewRole: PreviewRole;
  /** Part of the most recently played move. */
  recent: boolean;
  cursor: Direction | null;
  tileValues: Record<string, number>;
  onSelect: (row: number, col: number) => void;
}

function TileFace({ char, tileValues }: { char: string; tileValues: Record<string, number> }) {
  const blank = isBlankTile(char);
  const letter = char.toUpperCase();
  return (
    <>
      <span className="tile-letter">{letter}</span>
      {!blank && <span className="tile-value">{tileValues[letter]}</span>}
    </>
  );
}

/** One board square: a tile, a previewed tile, or the bare premium. */
function SquareComponent({
  row,
  col,
  premium,
  tile,
  previewTile,
  previewRole,
  recent,
  cursor,
  tileValues,
  onSelect,
}: SquareProps) {
  const shown = tile !== '.' ? tile : previewTile;
  const classes = ['square', PREMIUM_CLASS[premium] ?? ''];
  if (shown) {
    classes.push('has-tile');
    if (isBlankTile(shown)) classes.push('is-blank');
  }
  if (tile === '.' && previewTile) classes.push('is-preview');
  if (previewRole === 'through') classes.push('is-through');
  if (recent && !previewRole) classes.push('is-recent');
  if (cursor) classes.push('has-cursor', `cursor-${cursor}`);

  const label = shown
    ? `${squareName(row, col)} ${shown.toUpperCase()}${isBlankTile(shown) ? ' (blank)' : ''}`
    : squareName(row, col);

  return (
    <div
      className={classes.join(' ')}
      role="gridcell"
      aria-label={label}
      onMouseDown={(event) => {
        // Keep focus on the board so typing goes to the cursor.
        event.preventDefault();
        onSelect(row, col);
      }}
    >
      {shown ? (
        <span className="tile">
          <TileFace char={shown} tileValues={tileValues} />
        </span>
      ) : (
        <span className="premium-label">{PREMIUM_LABEL[premium] ?? ''}</span>
      )}
    </div>
  );
}

// 225 squares re-render on every hover over the move list; memoising keeps
// that to the handful whose props actually change.
export const Square = memo(SquareComponent);
