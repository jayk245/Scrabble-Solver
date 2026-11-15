import { useMemo, useState, type KeyboardEvent } from 'react';

import type { MoveInfo } from '../api/types';

type SortKey = 'score' | 'notation' | 'tilesUsed' | 'leave';
type SortDirection = 'asc' | 'desc';

interface Column {
  key: SortKey;
  label: string;
  /** Direction applied on the first click of the header. */
  initial: SortDirection;
  className?: string;
}

const COLUMNS: Column[] = [
  { key: 'notation', label: 'Move', initial: 'asc' },
  { key: 'score', label: 'Score', initial: 'desc', className: 'numeric' },
  { key: 'tilesUsed', label: 'Tiles', initial: 'desc', className: 'numeric' },
  { key: 'leave', label: 'Leave', initial: 'asc' },
];

/** Rows rendered per "show more" step; keeps the DOM small for big lists. */
const PAGE_SIZE = 200;

function compare(a: MoveInfo, b: MoveInfo, key: SortKey): number {
  switch (key) {
    case 'score':
      return a.score - b.score;
    case 'tilesUsed':
      return a.tilesUsed - b.tilesUsed;
    case 'notation':
      return a.notation.localeCompare(b.notation);
    case 'leave':
      return (a.leave ?? '').localeCompare(b.leave ?? '');
  }
}

/** Matches against the notation with play-through parentheses removed. */
function matchesFilter(move: MoveInfo, filter: string): boolean {
  if (!filter) return true;
  const haystack = `${move.notation.replace(/[()]/g, '')} ${move.leave ?? ''}`.toUpperCase();
  return haystack.includes(filter);
}

export interface MoveListProps {
  moves: MoveInfo[];
  total: number;
  truncated: boolean;
  elapsedMs: number | null;
  loading: boolean;
  error: string | null;
  selected: MoveInfo | null;
  onHover: (move: MoveInfo | null) => void;
  onSelect: (move: MoveInfo | null) => void;
  onPlay: (move: MoveInfo) => void;
}

/**
 * Sortable, filterable table of generated moves. Hovering previews a move;
 * clicking selects it (sticky preview); double-click or Enter plays it.
 * Arrow keys move the selection when the table has focus.
 */
export function MoveList({
  moves,
  total,
  truncated,
  elapsedMs,
  loading,
  error,
  selected,
  onHover,
  onSelect,
  onPlay,
}: MoveListProps) {
  const [sortKey, setSortKey] = useState<SortKey>('score');
  const [direction, setDirection] = useState<SortDirection>('desc');
  const [filter, setFilter] = useState('');
  const [bingosOnly, setBingosOnly] = useState(false);
  const [visible, setVisible] = useState(PAGE_SIZE);

  // Rank is by score in server order, so it stays meaningful under any sort.
  const ranks = useMemo(() => new Map(moves.map((move, i) => [move, i + 1])), [moves]);

  const rows = useMemo(() => {
    const needle = filter.trim().toUpperCase();
    const filtered = moves.filter(
      (move) => (!bingosOnly || move.bingo) && matchesFilter(move, needle),
    );
    const sign = direction === 'asc' ? 1 : -1;
    // Ties fall back to rank so the order is stable and deterministic.
    return filtered.sort(
      (a, b) => sign * compare(a, b, sortKey) || (ranks.get(a) ?? 0) - (ranks.get(b) ?? 0),
    );
  }, [moves, filter, bingosOnly, sortKey, direction, ranks]);

  const shown = rows.slice(0, visible);

  const sortBy = (column: Column) => {
    if (column.key === sortKey) {
      setDirection(direction === 'asc' ? 'desc' : 'asc');
    } else {
      setSortKey(column.key);
      setDirection(column.initial);
    }
  };

  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (rows.length === 0) return;
    const index = selected ? rows.indexOf(selected) : -1;
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      const step = event.key === 'ArrowDown' ? 1 : -1;
      const next = Math.min(rows.length - 1, Math.max(0, index + step));
      if (next >= visible) setVisible(next + 1);
      onSelect(rows[next] ?? null);
      document.getElementById(`move-row-${next}`)?.scrollIntoView({ block: 'nearest' });
    } else if (event.key === 'Enter' && selected) {
      onPlay(selected);
    } else if (event.key === 'Escape') {
      onSelect(null);
    }
  };

  let status: string;
  if (error) status = error;
  else if (moves.length === 0 && loading) status = 'Generating…';
  else {
    status = `${total.toLocaleString()} move${total === 1 ? '' : 's'}`;
    if (truncated) status += ` (top ${moves.length.toLocaleString()} listed)`;
    if (elapsedMs !== null) status += ` · ${elapsedMs.toFixed(1)} ms`;
  }

  return (
    <section className="panel move-list" aria-labelledby="moves-heading">
      <div className="panel-header">
        <h2 id="moves-heading">Moves</h2>
        <span className={`status-text${error ? ' is-error' : ''}${loading ? ' is-loading' : ''}`}>
          {status}
        </span>
      </div>
      <div className="move-filters">
        <input
          className="text-input"
          value={filter}
          onChange={(event) => {
            setFilter(event.target.value);
            setVisible(PAGE_SIZE);
          }}
          placeholder="Filter by word, square or leave"
          aria-label="Filter moves"
          spellCheck={false}
        />
        <label className="checkbox">
          <input
            type="checkbox"
            checked={bingosOnly}
            onChange={(event) => setBingosOnly(event.target.checked)}
          />
          Bingos
        </label>
      </div>
      <div
        className="move-table-wrap"
        tabIndex={0}
        onKeyDown={onKeyDown}
        onMouseLeave={() => onHover(null)}
        aria-label="Generated moves"
      >
        <table className="move-table">
          <thead>
            <tr>
              <th className="numeric">#</th>
              {COLUMNS.map((column) => (
                <th
                  key={column.key}
                  className={column.className}
                  aria-sort={
                    column.key === sortKey
                      ? direction === 'asc'
                        ? 'ascending'
                        : 'descending'
                      : 'none'
                  }
                >
                  <button type="button" className="sort-button" onClick={() => sortBy(column)}>
                    {column.label}
                    <span className="sort-indicator" aria-hidden="true">
                      {column.key === sortKey ? (direction === 'asc' ? '▲' : '▼') : ''}
                    </span>
                  </button>
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {shown.map((move, i) => (
              <tr
                key={move.notation}
                id={`move-row-${i}`}
                className={move === selected ? 'is-selected' : undefined}
                onMouseEnter={() => onHover(move)}
                onClick={() => onSelect(move === selected ? null : move)}
                onDoubleClick={() => onPlay(move)}
              >
                <td className="numeric muted">{ranks.get(move)}</td>
                <td className="notation">
                  {move.notation}
                  {move.bingo && <span className="badge">bingo</span>}
                </td>
                <td className="numeric score">{move.score}</td>
                <td className="numeric">{move.tilesUsed}</td>
                <td className="leave">{move.leave || '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
        {shown.length === 0 && !loading && !error && (
          <p className="empty-state">
            {moves.length === 0 ? 'Enter a rack to see legal moves.' : 'No moves match the filter.'}
          </p>
        )}
        {rows.length > visible && (
          <button
            type="button"
            className="button-ghost show-more"
            onClick={() => setVisible(visible + PAGE_SIZE)}
          >
            Show more ({(rows.length - visible).toLocaleString()} remaining)
          </button>
        )}
      </div>
      {selected && (
        <div className="selected-bar">
          <span>
            <strong className="notation">{selected.notation}</strong> for {selected.score}
          </span>
          <button type="button" className="button-primary" onClick={() => onPlay(selected)}>
            Play move
          </button>
        </div>
      )}
    </section>
  );
}
