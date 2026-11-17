// What the solver asks the engine about a position, and how answers are
// cached.
//
// The cache is keyed by the request itself (board, racks, scores, effort),
// not by a node of the move tree: the same position reached twice, or
// re-entered after an edit, finds its answer again, and an edited
// position can never be shown a stale one.

import type { AnalyzeRequest, AnalyzeResponse, Effort, PerLetter } from '../api/types';
import { SCORELESS_TURNS_TO_END, type GameNode } from './gameTree';
import { RACK_SIZE } from './rack';
import { unseenSummary } from './tiles';

export const EFFORT_ORDER: readonly Effort[] = ['static', 'quick', 'normal', 'deep'];

/** The request for the engine's best move at `node`, from the mover's side. */
export function analyzeRequest(node: GameNode, effort: Effort): AnalyzeRequest {
  const mover = node.toMove;
  const opponent = mover === 0 ? 1 : 0;
  return {
    board: node.board,
    rack: node.racks[mover],
    opponentRack: node.opponentKnown ? node.racks[opponent] : undefined,
    score: node.scores[mover],
    opponentScore: node.scores[opponent],
    scorelessTurns: Math.min(node.scoreless, SCORELESS_TURNS_TO_END - 1),
    effort,
  };
}

/** Whether the mover's rack is known in full, so the position can be solved. */
export function rackComplete(node: GameNode, tileCounts: PerLetter): boolean {
  const rack = node.racks[node.toMove];
  if (rack.length === 0) return false;
  if (rack.length >= RACK_SIZE) return true;
  // Fewer than seven tiles is a full rack only once the bag is empty.
  return unseenSummary(node.board, rack, tileCounts).bagCount === 0;
}

/** The cache key: everything about the position except the effort and played move. */
function positionKey(request: AnalyzeRequest): string {
  const position: AnalyzeRequest = { ...request };
  delete position.effort;
  delete position.played;
  return JSON.stringify(position);
}

/** A cached answer and the request that produced it. */
export interface CachedAnalysis {
  effort: Effort;
  /** The `played` move the request asked about, if any. */
  played?: string;
  result: AnalyzeResponse;
}

/** Answers by position key, at most one per effort. Treated as immutable. */
export type AnalysisCache = ReadonlyMap<string, readonly CachedAnalysis[]>;

export function storeAnalysis(
  cache: AnalysisCache,
  request: AnalyzeRequest,
  result: AnalyzeResponse,
): AnalysisCache {
  const key = positionKey(request);
  const effort = request.effort ?? 'normal';
  const kept = (cache.get(key) ?? []).filter((entry) => entry.effort !== effort);
  const next = new Map(cache);
  next.set(key, [...kept, { effort, played: request.played, result }]);
  return next;
}

/**
 * The best cached answer for `request`: at its effort, or failing that
 * the deepest answer above it (a deeper search is at least as good).
 */
export function lookupAnalysis(
  cache: AnalysisCache,
  request: AnalyzeRequest,
): CachedAnalysis | undefined {
  const entries = cache.get(positionKey(request));
  if (!entries) return undefined;
  const wanted = request.effort ?? 'normal';
  const exact = entries.find((entry) => entry.effort === wanted);
  if (exact) return exact;
  const rank = (effort: Effort) => EFFORT_ORDER.indexOf(effort);
  return entries
    .filter((entry) => rank(entry.effort) > rank(wanted))
    .sort((a, b) => rank(b.effort) - rank(a.effort))[0];
}

/** The deepest cached answer for the position that evaluates `notation`. */
export function lookupEvaluating(
  cache: AnalysisCache,
  request: AnalyzeRequest,
  notation: string,
): CachedAnalysis | undefined {
  const rank = (effort: Effort) => EFFORT_ORDER.indexOf(effort);
  return [...(cache.get(positionKey(request)) ?? [])]
    .sort((a, b) => rank(b.effort) - rank(a.effort))
    .find((entry) => evaluates(entry, notation));
}

/** The deepest cached answer for the position at any effort. */
export function lookupDeepest(
  cache: AnalysisCache,
  request: AnalyzeRequest,
): CachedAnalysis | undefined {
  const rank = (effort: Effort) => EFFORT_ORDER.indexOf(effort);
  return [...(cache.get(positionKey(request)) ?? [])].sort(
    (a, b) => rank(b.effort) - rank(a.effort),
  )[0];
}

/**
 * Whether a cached answer already says how good `notation` is: it was
 * the move asked about, or it is among the listed (and, when simulating,
 * simulated) candidates.
 */
export function evaluates(entry: CachedAnalysis, notation: string): boolean {
  if (entry.played === notation) return true;
  return entry.result.candidates.some(
    (candidate) =>
      candidate.move.notation === notation &&
      (entry.result.method !== 'simulation' || candidate.sim !== undefined),
  );
}
