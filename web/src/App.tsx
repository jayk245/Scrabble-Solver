import { useServerInfo } from './hooks/useServerInfo';
import { AnalysisView } from './views/AnalysisView';

/**
 * Application shell. Views are selected by mode; the game viewer (replays
 * and live self-play over Server-Sent Events) will slot in next to the
 * analysis board and reuse its Board and MoveList components.
 */
export function App() {
  const server = useServerInfo();

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
          <button type="button" className="mode-tab is-active" aria-current="page">
            Analysis
          </button>
          <button type="button" className="mode-tab" disabled title="Coming with the self-play arena">
            Game viewer <span className="badge">soon</span>
          </button>
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
        {server.status === 'ready' && <AnalysisView rules={server.rules} />}
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
