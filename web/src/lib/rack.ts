// Rack-string helpers. Racks are strings of 'A'-'Z' and '?' (blank).

import type { PerLetter } from '../api/types';

export const RACK_SIZE = 7;

/** Uppercases and drops anything that is not a tile, keeping at most 7. */
export function normaliseRack(text: string): string {
  return text
    .toUpperCase()
    .replace(/[^A-Z?]/g, '')
    .slice(0, RACK_SIZE);
}

/**
 * Draws a random rack from `pool` (tile counts). Used for "random rack":
 * the pool is the unseen tiles plus the current rack, i.e. everything not
 * already on the board.
 */
export function drawRack(pool: PerLetter, size = RACK_SIZE): string {
  const tiles = Object.entries(pool).flatMap(([letter, count]) =>
    Array.from({ length: count }, () => letter),
  );
  let rack = '';
  for (let i = 0; i < size && tiles.length > 0; i++) {
    const index = Math.floor(Math.random() * tiles.length);
    rack += tiles[index];
    tiles.splice(index, 1);
  }
  return sortRack(rack);
}

/** Alphabetical with blanks last, matching the server's canonical form. */
export function sortRack(rack: string): string {
  return [...rack]
    .sort((a, b) => {
      if (a === b) return 0;
      if (a === '?') return 1;
      if (b === '?') return -1;
      return a < b ? -1 : 1;
    })
    .join('');
}

/** Adds the tiles of `rack` to a per-letter count. */
export function addRackToCounts(counts: PerLetter, rack: string): PerLetter {
  const out = { ...counts };
  for (const tile of rack) out[tile] = (out[tile] ?? 0) + 1;
  return out;
}
