import { useEffect, useState } from 'react';

import { useServerInfo } from './hooks/useServerInfo';
import { GameViewerView } from './views/GameViewerView';
import { PlayView } from './views/PlayView';
import { SolverView } from './views/SolverView';

type Mode = 'solver' | 'play' | 'viewer';

const MODES: { mode: Mode; label: string; hash: string }[] = [
  { mode: 'solver', label: 'Solver', hash: '' },
  { mode: 'play', label: 'Play', hash: '#play' },
  { mode: 'viewer', label: 'Game viewer', hash: '#viewer' },
];

function modeFromHash(): Mode {
  // "#play" or "#viewer", optionally followed by "/<game id>".
  const { hash } = window.location;
  if (hash.startsWith('#play')) return 'play';
  if (hash.startsWith('#viewer')) return 'viewer';
  return 'solver';
}

/**
 * Application shell. The views are all kept mounted and only hidden when
 * inactive, so switching tabs never loses the solver's position or a game
 * in progress. The active view is mirrored in the URL hash so it survives
 * a reload and can be linked to.
 */
export function App() {
  const server = useServerInfo();
  const [mode, setMode] = useState<Mode>(modeFromHash);

  useEffect(() => {
    const onHashChange = () => setMode(modeFromHash());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  const select = (next: Mode) => {
    const hash = MODES.find((m) => m.mode === next)?.hash ?? '';
    // replaceState avoids a history entry per tab switch.
    window.history.replaceState(null, '', hash || window.location.pathname);
    setMode(next);
  };

  return (
    <div className="app">
      <header className="app-header">
        <div className="brand">
          <span className="brand-mark" aria-hidden="true">
            S
          </span>
          <span className="brand-name">Scrabble Engine</span>
        </div>
        <nav className="mode-nav" aria-label="Mode">
          {MODES.map((entry) => (
            <button
              key={entry.mode}
              type="button"
              className={`mode-tab${mode === entry.mode ? ' is-active' : ''}`}
              aria-current={mode === entry.mode ? 'page' : undefined}
              onClick={() => select(entry.mode)}
            >
              {entry.label}
            </button>
          ))}
        </nav>
        <div className="server-status">
          {server.status === 'ready' && (
            <>
              <span className="status-dot is-ok" aria-hidden="true" />
              {server.health.lexicon} &middot; {server.health.words.toLocaleString()} words
            </>
          )}
          {server.status === 'loading' && 'Connecting…'}
          {server.status === 'error' && (
            <>
              <span className="status-dot is-error" aria-hidden="true" />
              Offline
            </>
          )}
        </div>
      </header>

      <main className="app-main">
        {server.status === 'ready' && (
          <>
            <div hidden={mode !== 'solver'}>
              <SolverView rules={server.rules} />
            </div>
            <div hidden={mode !== 'play'}>
              <PlayView rules={server.rules} active={mode === 'play'} />
            </div>
            <div hidden={mode !== 'viewer'}>
              <GameViewerView rules={server.rules} active={mode === 'viewer'} />
            </div>
          </>
        )}
        {server.status === 'loading' && <p className="splash">Connecting to the engine&hellip;</p>}
        {server.status === 'error' && (
          <div className="splash">
            <h1>Engine server unavailable</h1>
            <p>{server.message}</p>
            <p>
              Start it with <code>./build/release/apps/server/scrabble_server</code>, then retry.
            </p>
            <button type="button" className="button-primary" onClick={server.retry}>
              Retry
            </button>
          </div>
        )}
      </main>
    </div>
  );
}
