// Grading the moves of a game against the engine, like a chess engine's
// game review. Grades are derived from cached analyses on every render
// rather than stored, so they can never disagree with the analyses shown.

import type { AnalyzeResponse, Candidate, PerLetter } from '../api/types';
import {
  analyzeRequest,
  lookupDeepest,
  lookupEvaluating,
  rackComplete,
  type AnalysisCache,
} from './engine';
import { mainLine, nodeAt, other, rackBefore, type GameNode, type GameTree } from './gameTree';
import { RACK_SIZE } from './rack';
import { boardCounts, unseenSummary } from './tiles';

export type Grade = 'best' | 'good' | 'inaccuracy' | 'mistake' | 'blunder';

/**
 * Upper bounds (exclusive) of points of equity given up for each grade;
 * anything from `mistake` up is a blunder. Points are static equity
 * (score plus leave value) or, for simulated positions, the mean final
 * spread of the simulated futures, so a 10-point inaccuracy costs about
 * ten points of spread on average.
 */
export const GRADE_LIMITS = { best: 1, good: 4, inaccuracy: 10, mistake: 20 } as const;

export const GRADE_LABEL: Record<Grade, string> = {
  best: 'Best',
  good: 'Good',
  inaccuracy: 'Inaccuracy',
  mistake: 'Mistake',
  blunder: 'Blunder',
};

/** Chess-style annotation symbols; best and good moves carry none. */
export const GRADE_SYMBOL: Record<Grade, string> = {
  best: '',
  good: '',
  inaccuracy: '?!',
  mistake: '?',
  blunder: '??',
};

export function gradeFor(lost: number): Grade {
  if (lost < GRADE_LIMITS.best) return 'best';
  if (lost < GRADE_LIMITS.good) return 'good';
  if (lost < GRADE_LIMITS.inaccuracy) return 'inaccuracy';
  if (lost < GRADE_LIMITS.mistake) return 'mistake';
  return 'blunder';
}

/** How the points lost were measured. */
export type LossBasis = 'simulation' | 'static' | 'endgame';

export interface MoveGrade {
  /** Points given up against the engine's move, never negative. */
  lost: number;
  basis: LossBasis;
  grade: Grade;
  /** The engine's move. */
  best: Candidate;
}

/** What is known after the move, for grading endgame moves exactly. */
export interface AfterMove {
  /** The engine's answer for the position after the move, if solved. */
  analysis?: AnalyzeResponse;
  /**
   * Set when the move went out: the mover's final spread, including the
   * opponent's unplayed tiles.
   */
  finalSpread?: number;
}

/**
 * The grade of `notation` played in a position the engine analysed, or
 * null when the analysis cannot say (the move is not legal for the rack,
 * e.g. a phony).
 *
 * - Simulation: the best candidate's mean minus the played move's mean
 *   (the played move is always simulated, see AnalyzeRequest.played).
 * - Endgame: the solver's value minus the value of the position after the
 *   move (its negated endgame value, or the final spread if the move went
 *   out). Falls back to static equity when that position is not solved.
 * - Otherwise static equity.
 */
export function gradeMove(
  analysis: AnalyzeResponse,
  notation: string,
  after: AfterMove = {},
): MoveGrade | null {
  const best = analysis.candidates.find((candidate) => candidate.chosen);
  if (!best) return null;
  const result = (lost: number, basis: LossBasis): MoveGrade => {
    const clamped = Math.max(0, Math.round(lost * 10) / 10);
    return { lost: clamped, basis, grade: gradeFor(clamped), best };
  };
  if (best.move.notation === notation) {
    return result(
      0,
      analysis.method === 'simulation'
        ? 'simulation'
        : analysis.method === 'endgame'
          ? 'endgame'
          : 'static',
    );
  }
  const played =
    analysis.played?.move.notation === notation
      ? analysis.played
      : analysis.candidates.find((candidate) => candidate.move.notation === notation);
  if (!played) return null;

  if (analysis.method === 'simulation' && best.sim && played.sim) {
    return result(best.sim.mean - played.sim.mean, 'simulation');
  }
  if (analysis.method === 'endgame' && analysis.endgame) {
    if (after.finalSpread !== undefined) {
      return result(analysis.endgame.value - after.finalSpread, 'endgame');
    }
    if (after.analysis?.method === 'endgame' && after.analysis.endgame) {
      return result(analysis.endgame.value + after.analysis.endgame.value, 'endgame');
    }
  }
  if (played.equity === null) return null;
  return result(best.equity - played.equity, 'static');
}

/** A main-line move and what the review knows of it. */
export interface ReviewedMove {
  /** The node the move leads to. */
  nodeId: string;
  /** The position the move was played from. */
  parentId: string;
  player: 0 | 1;
  notation: string;
  /** The grade, once the position before the move is analysed. */
  grade: MoveGrade | null;
  /** Why the move cannot be graded, if it cannot. */
  skipped?: string;
}

const TOTAL_TILES = 100;

/** True if the move into `node` emptied the mover's rack with the bag empty. */
export function wentOut(tree: GameTree, node: GameNode): boolean {
  const origin = node.origin;
  if (!origin || origin.kind !== 'play' || origin.move.type !== 'place') return false;
  if (rackBefore(tree, node) === '' || node.racks[origin.player] !== '') return false;
  const onBoard = Object.values(boardCounts(node.board)).reduce((sum, n) => sum + n, 0);
  return TOTAL_TILES - onBoard <= RACK_SIZE;
}

/**
 * Every main-line move with its grade from the cached analyses. A move is
 * gradable when the mover's full rack before it is known; the rest are
 * listed with the reason they are skipped.
 */
export function reviewMainLine(
  tree: GameTree,
  cache: AnalysisCache,
  tileCounts: PerLetter,
  tileValues: PerLetter,
): ReviewedMove[] {
  const line = mainLine(tree);
  const out: ReviewedMove[] = [];
  for (let i = 1; i < line.length; i++) {
    const node = nodeAt(tree, line[i]!);
    const parent = nodeAt(tree, line[i - 1]!);
    const origin = node.origin;
    if (!origin || origin.kind !== 'play') continue;
    const entry: ReviewedMove = {
      nodeId: node.id,
      parentId: parent.id,
      player: origin.player,
      notation: origin.move.notation,
      grade: null,
    };
    out.push(entry);
    if (!rackComplete(parent, tileCounts)) {
      entry.skipped = 'rack unknown';
      continue;
    }
    const cached = lookupEvaluating(cache, analyzeRequest(parent, 'static'), entry.notation);
    if (!cached) continue;
    const after: AfterMove = {};
    if (wentOut(tree, node)) {
      const mover = origin.player;
      const left = unseenSummary(node.board, '', tileCounts).unseen;
      after.finalSpread =
        node.scores[mover] + 2 * tilePoints(left, tileValues) - node.scores[other(mover)];
    } else if (rackComplete(node, tileCounts)) {
      after.analysis = lookupDeepest(cache, analyzeRequest(node, 'static'))?.result;
    }
    entry.grade = gradeMove(cached.result, entry.notation, after);
    if (!entry.grade) {
      entry.skipped = cached.result.playedError ?? 'not a legal move for the rack';
    }
  }
  return out;
}

/** Points of the tiles in `counts`. */
export function tilePoints(counts: PerLetter, tileValues: PerLetter): number {
  return Object.entries(counts).reduce(
    (sum, [letter, count]) => sum + count * (tileValues[letter] ?? 0),
    0,
  );
}
