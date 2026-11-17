import { useCallback, useEffect, useRef, useState } from 'react';

import { api } from '../api/client';
import type { AnalyzeRequest, AnalyzeResponse, Effort, PerLetter } from '../api/types';
import {
  analyzeRequest,
  EFFORT_ORDER,
  lookupEvaluating,
  rackComplete,
  type AnalysisCache,
} from '../lib/engine';
import { mainLine, nodeAt, type GameTree } from '../lib/gameTree';

export interface ReviewProgress {
  effort: Effort;
  done: number;
  total: number;
  /** Positions the engine could not analyse (e.g. an impossible tile count). */
  failed: number;
  running: boolean;
}

interface Job {
  request: AnalyzeRequest;
  notation: string;
}

/**
 * Whole-game analysis: solves, one at a time, every main-line position
 * whose mover's rack is known, asking the engine about the move actually
 * played as well as its own. Answers go into the shared analysis cache,
 * so positions already solved (by the engine panel or an earlier review)
 * are skipped, and the grades are derived from the cache as they arrive.
 *
 * Cancelling aborts the request in flight; the server drops a search
 * that has not started yet (see AnalyzeApi).
 */
export function useGameReview(
  tree: GameTree,
  cache: AnalysisCache,
  store: (request: AnalyzeRequest, result: AnalyzeResponse) => void,
  tileCounts: PerLetter,
) {
  const [progress, setProgress] = useState<ReviewProgress | null>(null);
  const controller = useRef<AbortController | null>(null);
  // The loop runs across renders, so it reads the latest cache from a ref.
  const cacheRef = useRef(cache);
  useEffect(() => {
    cacheRef.current = cache;
  }, [cache]);

  const cancel = useCallback(() => {
    controller.current?.abort();
    controller.current = null;
    setProgress((current) => (current ? { ...current, running: false } : null));
  }, []);

  useEffect(() => () => controller.current?.abort(), []);

  const run = useCallback(
    async (effort: Effort) => {
      controller.current?.abort();
      const own = new AbortController();
      controller.current = own;

      const line = mainLine(tree);
      const jobs: Job[] = [];
      for (let i = 1; i < line.length; i++) {
        const node = nodeAt(tree, line[i]!);
        const parent = nodeAt(tree, line[i - 1]!);
        if (node.origin?.kind !== 'play' || !rackComplete(parent, tileCounts)) continue;
        const notation = node.origin.move.notation;
        jobs.push({ request: { ...analyzeRequest(parent, effort), played: notation }, notation });
      }

      let done = 0;
      let failed = 0;
      setProgress({ effort, done, total: jobs.length, failed, running: true });
      for (const job of jobs) {
        if (own.signal.aborted) return;
        const cached = lookupEvaluating(cacheRef.current, job.request, job.notation);
        const deepEnough =
          cached && (cached.effort === effort || effortRank(cached.effort) > effortRank(effort));
        if (!deepEnough) {
          try {
            store(job.request, await api.analyze(job.request, own.signal));
          } catch {
            if (own.signal.aborted) return;
            failed++;
          }
        }
        done++;
        setProgress({ effort, done, total: jobs.length, failed, running: true });
      }
      if (controller.current === own) controller.current = null;
      setProgress({ effort, done, total: jobs.length, failed, running: false });
    },
    [tree, tileCounts, store],
  );

  return { progress, run, cancel, running: progress?.running ?? false };
}

function effortRank(effort: Effort): number {
  return EFFORT_ORDER.indexOf(effort);
}
