import { useCallback, useEffect, useState } from 'react';

import { api } from '../api/client';
import type { HealthResponse, Rules } from '../api/types';

export type ServerInfo =
  | { status: 'loading' }
  | { status: 'error'; message: string; retry: () => void }
  | { status: 'ready'; health: HealthResponse; rules: Rules };

/**
 * Fetches the static rules and lexicon info once at start-up. Everything
 * the board renders (premium layout, tile values) comes from the server so
 * the engine remains the single source of truth.
 */
export function useServerInfo(): ServerInfo {
  const [attempt, setAttempt] = useState(0);
  const [info, setInfo] = useState<ServerInfo>({ status: 'loading' });
  const retry = useCallback(() => setAttempt((n) => n + 1), []);

  useEffect(() => {
    let cancelled = false;
    setInfo({ status: 'loading' });
    Promise.all([api.health(), api.rules()])
      .then(([health, rules]) => {
        if (!cancelled) setInfo({ status: 'ready', health, rules });
      })
      .catch((error: unknown) => {
        if (cancelled) return;
        const message = error instanceof Error ? error.message : String(error);
        setInfo({ status: 'error', message, retry });
      });
    return () => {
      cancelled = true;
    };
  }, [attempt, retry]);

  return info;
}
