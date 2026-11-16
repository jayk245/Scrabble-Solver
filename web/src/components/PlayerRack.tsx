const LETTERS = [...'ABCDEFGHIJKLMNOPQRSTUVWXYZ'];

export interface PlayerRackProps {
  /** Tiles in rack order ('?' = blank). */
  tiles: string[];
  /** Display order: a permutation of tile indices (for Shuffle). */
  order: number[];
  /** Indices currently placed on the board. */
  used: ReadonlySet<number>;
  /** Indices picked for an exchange; null outside exchange mode. */
  selected: ReadonlySet<number> | null;
  tileValues: Record<string, number>;
  disabled: boolean;
  onTileClick: (index: number) => void;
  /** Set while a blank waits for its letter. */
  blankPending: boolean;
  onBlankLetter: (letter: string) => void;
  onBlankCancel: () => void;
}

/**
 * The human's own rack as clickable tiles. Tiles on the board stay in
 * place but fade, so the rack never reflows while a word is being built.
 */
export function PlayerRack({
  tiles,
  order,
  used,
  selected,
  tileValues,
  disabled,
  onTileClick,
  blankPending,
  onBlankLetter,
  onBlankCancel,
}: PlayerRackProps) {
  return (
    <div className="player-rack">
      <div className="rack-tiles is-interactive" role="group" aria-label="Your rack">
        {order.map((index) => {
          const tile = tiles[index] ?? '';
          const classes = ['rack-tile'];
          if (tile === '?') classes.push('is-blank');
          if (used.has(index)) classes.push('is-used');
          if (selected?.has(index)) classes.push('is-selected');
          return (
            <button
              key={index}
              type="button"
              className={classes.join(' ')}
              disabled={disabled}
              aria-pressed={selected ? selected.has(index) : used.has(index)}
              aria-label={tile === '?' ? 'Blank' : tile}
              onMouseDown={(event) => event.preventDefault()}
              onClick={() => onTileClick(index)}
            >
              <span className="tile-letter">{tile === '?' ? '' : tile}</span>
              {tile !== '?' && <span className="tile-value">{tileValues[tile]}</span>}
            </button>
          );
        })}
      </div>
      {blankPending && (
        <div className="blank-picker" role="group" aria-label="Letter for the blank">
          <span className="hint">Blank stands for:</span>
          <div className="blank-letters">
            {LETTERS.map((letter) => (
              <button
                key={letter}
                type="button"
                className="button-ghost"
                onMouseDown={(event) => event.preventDefault()}
                onClick={() => onBlankLetter(letter)}
              >
                {letter}
              </button>
            ))}
          </div>
          <button type="button" className="button-ghost" onClick={onBlankCancel}>
            Cancel
          </button>
        </div>
      )}
    </div>
  );
}
