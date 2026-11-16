import { normaliseRack } from '../lib/rack';
import { RackTiles } from './RackTiles';

export interface RackInputProps {
  rack: string;
  tileValues: Record<string, number>;
  onChange: (rack: string) => void;
  /** Draws a random rack from the tiles not on the board. */
  onRandom: () => void;
}

/** Text entry for the rack, with the tiles drawn underneath. */
export function RackInput({ rack, tileValues, onChange, onRandom }: RackInputProps) {
  return (
    <section className="panel" aria-labelledby="rack-heading">
      <div className="panel-header">
        <h2 id="rack-heading">Rack</h2>
        <div className="button-row">
          <button type="button" className="button-ghost" onClick={onRandom}>
            Random
          </button>
          <button
            type="button"
            className="button-ghost"
            onClick={() => onChange('')}
            disabled={rack.length === 0}
          >
            Clear
          </button>
        </div>
      </div>
      <input
        className="text-input rack-text"
        value={rack}
        onChange={(event) => onChange(normaliseRack(event.target.value))}
        placeholder="e.g. RETAINS or AEINST?"
        aria-label="Rack tiles"
        spellCheck={false}
        autoComplete="off"
      />
      <RackTiles rack={rack} tileValues={tileValues} />
      <p className="hint">Letters, or ? for a blank. Moves update as you type.</p>
    </section>
  );
}
