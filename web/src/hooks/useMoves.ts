import { useEffect, useState } from 'react';

import { api } from '../api/client';
import type { BoardRows, MovesResponse } from '../api/types';

/** How many moves to fetch; the list renders them incrementally. */
const MOVE_LIMIT = 5000;
/** Coalesces bursts of edits (typing a word on the board) into one request. */
const DEBOUNCE_MS = 120;

export interface MovesState {
  data: MovesResponse | null;
  loading: boolean;
  error: string | null;
}

/**
 * Generates moves whenever the board or rack changes. Requests are
 * debounced, and a superseded request is aborted so a slow response can
 * never overwrite a newer one.
 */
export function useMoves(board: BoardRows, rack: string): MovesState {
  const [state, setState] = useState<MovesState>({ data: null, loading: true, error: null });

  useEffect(() => {
    const controller = new AbortController();
    setState((previous) => ({ ...previous, loading: true }));
    const timer = window.setTimeout(() => {
      api
        .moves({ board, rack, limit: MOVE_LIMIT }, controller.signal)
        .then((data) => setState({ data, loading: false, error: null }))
        .catch((error: unknown) => {
          if (controller.signal.aborted) return;
          const message = error instanceof Error ? error.message : String(error);
          setState({ data: null, loading: false, error: message });
        });
    }, DEBOUNCE_MS);
    return () => {
      window.clearTimeout(timer);
      controller.abort();
    };
  }, [board, rack]);

  return state;
}
