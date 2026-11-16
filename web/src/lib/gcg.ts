// GCG export of a game entered in the solver, in the same dialect as the
// engine's ToGcg() (engine/src/gcg.cc): '.' marks a tile already on the
// board, and every line carries the mover's rack before the move.

import type { MoveInfo, Seats } from '../api/types';
import { coordinate } from './placement';

export interface GcgMove {
  player: 0 | 1;
  /** Rack before the move; the tiles played are used when it is unknown. */
  rack: string;
  move: MoveInfo;
  /** The mover's total after the move. */
  cumulative: number;
}

/** GCG nicknames are single words. */
function nickname(name: string, fallback: string): string {
  const cleaned = name.trim().replace(/\s+/g, '_');
  return cleaned || fallback;
}

function moveText(move: MoveInfo): string {
  if (move.type !== 'place' || move.row === undefined || move.col === undefined) {
    return move.notation;
  }
  const word = move.tiles.map((tile) => (tile.playThrough ? '.' : tile.letter)).join('');
  return `${coordinate(move.row, move.col, move.direction ?? 'across')} ${word}`;
}

function tilesPlayed(move: MoveInfo): string {
  if (move.type === 'exchange') return move.word;
  return move.tiles
    .filter((tile) => !tile.playThrough)
    .map((tile) => (tile.blank ? '?' : tile.letter))
    .join('');
}

export function toGcg(
  names: Seats<string>,
  moves: GcgMove[],
  options: { title?: string; startScores?: Seats<number>; customStart?: boolean } = {},
): string {
  const nicks: Seats<string> = [nickname(names[0], 'player1'), nickname(names[1], 'player2')];
  if (nicks[0] === nicks[1]) nicks[1] = `${nicks[1]}_2`;
  const lines = [
    '#character-encoding UTF-8',
    `#player1 ${nicks[0]} ${names[0]}`,
    `#player2 ${nicks[1]} ${names[1]}`,
  ];
  if (options.title) lines.push(`#title ${options.title}`);
  const [a, b] = options.startScores ?? [0, 0];
  if (options.customStart || a !== 0 || b !== 0) {
    lines.push(
      `#note Started from an edited position (scores ${a}-${b}); the board setup is not recorded.`,
    );
  }
  for (const { player, rack, move, cumulative } of moves) {
    const signed = move.score >= 0 ? `+${move.score}` : `${move.score}`;
    lines.push(
      `>${nicks[player]}: ${rack || tilesPlayed(move) || '-'} ${moveText(move)} ${signed} ${cumulative}`,
    );
  }
  return `${lines.join('\n')}\n`;
}

/** Saves `text` as a file through a temporary object URL. */
export function downloadText(filename: string, text: string): void {
  const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  link.click();
  URL.revokeObjectURL(url);
}
