import { useState, type FormEvent } from 'react';

import type { BotStrength, EndRulesName, StartGameRequest } from '../api/types';

const STRENGTHS: { value: BotStrength; label: string; detail: string }[] = [
  { value: 'easy', label: 'Easy', detail: 'Plays its highest-scoring word, ignoring the tiles it keeps.' },
  { value: 'normal', label: 'Normal', detail: 'Score plus learned leave values; replies instantly.' },
  { value: 'hard', label: 'Hard', detail: 'Monte Carlo simulation of its top moves; up to 3 s a move.' },
  { value: 'expert', label: 'Expert', detail: 'Larger simulation plus the exact endgame solver; up to 4 s.' },
];

type First = 'human' | 'bot' | 'random';

export interface PlaySetupProps {
  busy: boolean;
  error: string | null;
  onStart: (request: StartGameRequest) => void;
}

/** The form that starts a game against the bot. */
export function PlaySetup({ busy, error, onStart }: PlaySetupProps) {
  const [name, setName] = useState('you');
  const [strength, setStrength] = useState<BotStrength>('normal');
  const [first, setFirst] = useState<First>('human');
  const [seed, setSeed] = useState('');
  const [rules, setRules] = useState<EndRulesName>('naspa');

  const seedValue = seed.trim() === '' ? undefined : Number(seed);
  const badSeed = seedValue !== undefined && !Number.isSafeInteger(seedValue);
  const badName = name.length === 0 || name === 'bot';

  const submit = (event: FormEvent) => {
    event.preventDefault();
    onStart({
      mode: 'human',
      name,
      strength,
      humanFirst: first === 'random' ? Math.random() < 0.5 : first === 'human',
      seed: seedValue,
      rules,
    });
  };

  return (
    <form className="panel game-setup" onSubmit={submit} aria-labelledby="play-setup-heading">
      <div className="panel-header">
        <h2 id="play-setup-heading">Play against the bot</h2>
      </div>

      <div className="field">
        <span>Bot strength</span>
        <div className="segmented" role="radiogroup" aria-label="Bot strength">
          {STRENGTHS.map((option) => (
            <button
              key={option.value}
              type="button"
              role="radio"
              aria-checked={strength === option.value}
              className={strength === option.value ? 'is-active' : undefined}
              onClick={() => setStrength(option.value)}
            >
              {option.label}
            </button>
          ))}
        </div>
        <p className="hint">{STRENGTHS.find((option) => option.value === strength)?.detail}</p>
      </div>

      <div className="field">
        <span>First move</span>
        <div className="segmented" role="radiogroup" aria-label="Who moves first">
          {(
            [
              ['human', 'Me'],
              ['bot', 'Bot'],
              ['random', 'Random'],
            ] as const
          ).map(([value, label]) => (
            <button
              key={value}
              type="button"
              role="radio"
              aria-checked={first === value}
              className={first === value ? 'is-active' : undefined}
              onClick={() => setFirst(value)}
            >
              {label}
            </button>
          ))}
        </div>
      </div>

      <div className="field-row">
        <label className="field">
          <span>Your name</span>
          <input
            className="text-input"
            value={name}
            maxLength={24}
            onChange={(event) => setName(event.target.value.replace(/[^A-Za-z0-9_-]/g, ''))}
          />
        </label>
        <label className="field">
          <span>Seed</span>
          <input
            className="text-input"
            inputMode="numeric"
            placeholder="random"
            value={seed}
            onChange={(event) => setSeed(event.target.value.replace(/[^0-9]/g, ''))}
          />
        </label>
        <label className="field">
          <span>Rules</span>
          <select
            className="text-input"
            value={rules}
            onChange={(event) => setRules(event.target.value as EndRulesName)}
          >
            <option value="naspa">NASPA</option>
            <option value="wespa">WESPA</option>
          </select>
        </label>
      </div>

      {badName && <p className="verdict is-illegal">Choose a name other than “bot”.</p>}
      {badSeed && <p className="verdict is-illegal">The seed must be a whole number.</p>}
      <p className="hint">
        The server deals the tiles and checks every move against the ENABLE word list. There is
        no challenge: a word the engine does not accept is simply refused, and you try again. The
        bot&apos;s rack stays hidden until the game ends.
      </p>
      {error && <p className="verdict is-illegal">{error}</p>}
      <button type="submit" className="button-primary" disabled={busy || badName || badSeed}>
        {busy ? 'Dealing…' : 'Start game'}
      </button>
    </form>
  );
}
