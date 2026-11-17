import { useState } from 'react';

import { parseMoveList } from '../lib/board';

export interface SequenceInputProps {
  /**
   * Plays the moves in order, stopping at the first illegal one. Resolves
   * to the number applied and an error message for the move that failed.
   */
  onApply: (moves: string[]) => Promise<{ applied: number; error: string | null }>;
  /** Renders without its own panel, inside another one. */
  embedded?: boolean;
}

/** Sets up a position by pasting a game's moves in notation. */
export function SequenceInput({ onApply, embedded = false }: SequenceInputProps) {
  const [text, setText] = useState('');
  const [message, setMessage] = useState<{ text: string; error: boolean } | null>(null);
  const [busy, setBusy] = useState(false);

  const apply = async () => {
    const moves = parseMoveList(text);
    if (moves.length === 0) return;
    setBusy(true);
    const { applied, error } = await onApply(moves);
    setBusy(false);
    if (error) {
      setMessage({
        text: `Applied ${applied} of ${moves.length}. Move ${applied + 1} (${moves[applied]}): ${error}`,
        error: true,
      });
    } else {
      setMessage({ text: `Applied ${applied} move${applied === 1 ? '' : 's'}.`, error: false });
      setText('');
    }
  };

  const body = (
    <>
      <textarea
        className="text-input sequence-text"
        value={text}
        onChange={(event) => setText(event.target.value)}
        placeholder={'8D RETAINS\nE5 FA(R)M\n...'}
        rows={4}
        spellCheck={false}
        aria-label="Moves in notation, one per line"
      />
      <div className="button-row">
        <button
          type="button"
          className="button-secondary"
          onClick={apply}
          disabled={busy || !text.trim()}
        >
          Play from here
        </button>
      </div>
      {message && (
        <p className={`verdict ${message.error ? 'is-illegal' : 'is-legal'}`}>{message.text}</p>
      )}
      <p className="hint">
        One move per line (or separated by ;), played from the current position. Racks are unknown,
        so each move is checked for legal placement and valid words only.
      </p>
    </>
  );

  if (embedded) return body;
  return (
    <section className="panel" aria-labelledby="sequence-heading">
      <div className="panel-header">
        <h2 id="sequence-heading">Paste moves</h2>
      </div>
      {body}
    </section>
  );
}
