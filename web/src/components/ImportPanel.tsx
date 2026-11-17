import { useRef, useState, type ChangeEvent } from 'react';

import { SequenceInput, type SequenceInputProps } from './SequenceInput';

export interface ImportPanelProps {
  /** Replaces the game with a GCG record; resolves to an error message or null. */
  onImportGcg: (gcg: string) => Promise<string | null>;
  /** Plays pasted moves from the current position. */
  onApplyMoves: SequenceInputProps['onApply'];
}

/**
 * Bringing a game in: a GCG record (pasted or from a file) becomes the
 * main line with every rack it records, or a list of moves in notation is
 * played from the current position.
 */
export function ImportPanel({ onImportGcg, onApplyMoves }: ImportPanelProps) {
  const [mode, setMode] = useState<'gcg' | 'moves'>('gcg');
  const [text, setText] = useState('');
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<{ text: string; error: boolean } | null>(null);
  const fileInput = useRef<HTMLInputElement>(null);

  const load = async (gcg: string) => {
    setBusy(true);
    const error = await onImportGcg(gcg);
    setBusy(false);
    setMessage(error ? { text: error, error: true } : { text: 'Game loaded.', error: false });
    if (!error) setText('');
  };

  const onFile = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (file) await load(await file.text());
  };

  return (
    <section className="panel" aria-labelledby="import-heading">
      <div className="panel-header">
        <h2 id="import-heading">Import</h2>
        <div className="segmented" role="radiogroup" aria-label="Import format">
          <button
            type="button"
            role="radio"
            aria-checked={mode === 'gcg'}
            className={mode === 'gcg' ? 'is-active' : undefined}
            onClick={() => setMode('gcg')}
          >
            GCG
          </button>
          <button
            type="button"
            role="radio"
            aria-checked={mode === 'moves'}
            className={mode === 'moves' ? 'is-active' : undefined}
            onClick={() => setMode('moves')}
          >
            Moves
          </button>
        </div>
      </div>
      {mode === 'gcg' ? (
        <>
          <textarea
            className="text-input sequence-text"
            value={text}
            onChange={(event) => setText(event.target.value)}
            placeholder={'#player1 ann Ann\n#player2 ben Ben\n>ann: AEINRST 8D RETAINS +70 70'}
            rows={4}
            spellCheck={false}
            aria-label="GCG record"
          />
          <div className="button-row">
            <button
              type="button"
              className="button-secondary"
              disabled={busy || !text.trim()}
              onClick={() => void load(text)}
            >
              Load game
            </button>
            <button
              type="button"
              className="button-ghost"
              disabled={busy}
              onClick={() => fileInput.current?.click()}
            >
              Open file…
            </button>
            <input
              ref={fileInput}
              type="file"
              accept=".gcg,text/plain"
              hidden
              onChange={(event) => void onFile(event)}
            />
          </div>
          {message && (
            <p className={`verdict ${message.error ? 'is-illegal' : 'is-legal'}`}>{message.text}</p>
          )}
          <p className="hint">
            Replaces the game with the record as the main line, with every rack it records, so each
            position can be solved and the game reviewed.
          </p>
        </>
      ) : (
        <SequenceInput onApply={onApplyMoves} embedded />
      )}
    </section>
  );
}
