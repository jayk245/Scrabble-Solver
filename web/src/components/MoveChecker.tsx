import { useState, type FormEvent } from 'react';

import { api, ApiError } from '../api/client';
import type { BoardRows, MoveInfo } from '../api/types';

type Result =
  | { kind: 'legal'; move: MoveInfo }
  | { kind: 'illegal'; message: string }
  | null;

export interface MoveCheckerProps {
  board: BoardRows;
  rack: string;
  /** Plays a move by notation; resolves to an error message or null. */
  onPlay: (notation: string) => Promise<string | null>;
  onPreview: (move: MoveInfo | null) => void;
}

/**
 * Validates a typed move against the board (and the rack, if one is
 * entered) and shows the engine's verdict: the score, or the MoveError
 * message explaining why it is illegal.
 */
export function MoveChecker({ board, rack, onPlay, onPreview }: MoveCheckerProps) {
  const [notation, setNotation] = useState('');
  const [result, setResult] = useState<Result>(null);
  const [busy, setBusy] = useState(false);

  const check = async (event?: FormEvent) => {
    event?.preventDefault();
    if (!notation.trim()) return;
    setBusy(true);
    try {
      const response = await api.validate({
        board,
        rack: rack || undefined,
        move: notation,
      });
      if (response.valid) {
        setResult({ kind: 'legal', move: response.move });
        onPreview(response.move);
      } else {
        setResult({ kind: 'illegal', message: response.message });
        onPreview(null);
      }
    } catch (error) {
      setResult({
        kind: 'illegal',
        message: error instanceof ApiError ? error.message : String(error),
      });
      onPreview(null);
    } finally {
      setBusy(false);
    }
  };

  const play = async () => {
    if (!notation.trim()) return;
    setBusy(true);
    const error = await onPlay(notation);
    setBusy(false);
    if (error) {
      setResult({ kind: 'illegal', message: error });
    } else {
      setResult(null);
      setNotation('');
    }
  };

  return (
    <section className="panel" aria-labelledby="checker-heading">
      <div className="panel-header">
        <h2 id="checker-heading">Check a move</h2>
      </div>
      <form className="inline-form" onSubmit={check}>
        <input
          className="text-input notation"
          value={notation}
          onChange={(event) => {
            setNotation(event.target.value);
            setResult(null);
          }}
          placeholder="e.g. 8D RETAINS or H8 QU(I)T"
          aria-label="Move notation"
          spellCheck={false}
          autoComplete="off"
        />
        <button type="submit" className="button-secondary" disabled={busy}>
          Check
        </button>
        <button type="button" className="button-primary" disabled={busy} onClick={play}>
          Play
        </button>
      </form>
      {result?.kind === 'legal' && (
        <p className="verdict is-legal">
          Legal: <strong className="notation">{result.move.notation}</strong> scores{' '}
          <strong>{result.move.score}</strong>
          {result.move.leave !== undefined && <> leaving {result.move.leave || 'nothing'}</>}
        </p>
      )}
      {result?.kind === 'illegal' && <p className="verdict is-illegal">{result.message}</p>}
      <p className="hint">
        {rack
          ? 'Checked against the rack above.'
          : 'No rack entered, so only the board and dictionary are checked.'}{' '}
        Lowercase letters are blanks; parentheses or dots mark tiles already on the board.
      </p>
    </section>
  );
}
