// Loading a recorded game into the solver's move tree. The input is the
// event stream the server produces for every game (start, one event per
// move); /api/gcg returns the same events for a GCG record, so a pasted
// record, a self-play game and a finished game against the bot all go
// through this one function.

import type { GameMoveEvent, GameStartEvent, MoveInfo, Seats } from '../api/types';
import {
  other,
  SCORELESS_TURNS_TO_END,
  treeFromLine,
  type GameTree,
  type ImportedNode,
} from './gameTree';

function knownRacks(racks: Seats<string | null>): Seats<string> {
  return [racks[0] ?? '', racks[1] ?? ''];
}

/** A scoring stand-in for events that are not moves. */
function pseudoMove(label: string, score: number): MoveInfo {
  return { notation: label, type: 'pass', score, tilesUsed: 0, bingo: false, word: '', tiles: [] };
}

/**
 * The game as a tree whose main line is the recorded moves. Each node
 * holds both racks as the record knows them, so stepping to any position
 * shows (and can solve) the mover's actual rack. Events without a move
 * (a withdrawn phony, a challenge bonus) become nodes too, so scores and
 * the board stay exactly as recorded.
 */
export function treeFromEvents(start: GameStartEvent, moves: GameMoveEvent[]): GameTree {
  const names: Seats<string> = [start.players[0].name, start.players[1].name];
  // Who moves next after each event: the player of the next real move.
  const nextMover = (i: number, fallback: 0 | 1): 0 | 1 => {
    for (let j = i + 1; j < moves.length; j++) {
      if (moves[j]!.kind === 'play') return moves[j]!.player;
    }
    return fallback;
  };
  const firstMover = moves.find((event) => event.kind === 'play')?.player ?? start.toMove ?? 0;
  const root: ImportedNode = {
    board: start.board,
    scores: start.scores,
    toMove: firstMover,
    racks: knownRacks(start.racks),
    opponentKnown: false,
    scoreless: 0,
    origin: null,
  };

  let scoreless = 0;
  const line = moves.map((event, i): ImportedNode => {
    if (event.kind !== 'adjustment') {
      const scored = event.kind === 'play' && event.score !== 0;
      scoreless = scored ? 0 : Math.min(scoreless + 1, SCORELESS_TURNS_TO_END);
    }
    const move = event.move ?? pseudoMove(event.label, event.score);
    return {
      board: event.board,
      scores: event.scores,
      toMove: nextMover(i, other(event.player)),
      racks: knownRacks(event.racks),
      opponentKnown: false,
      scoreless,
      origin: {
        player: event.player,
        kind: event.kind,
        move,
        label: event.label !== move.notation ? event.label : undefined,
        warning: event.warning,
        note: event.note,
      },
    };
  });
  return treeFromLine(names, root, line);
}
