import { useState, type ChangeEvent, type FormEvent } from 'react';

import type { EndRulesName, LeaveModel, PlayerConfig, StartGameRequest } from '../api/types';

/** Form state for one seat; mirrors PlayerConfig with every field set. */
interface SeatForm {
  name: string;
  leaves: LeaveModel;
  simulate: boolean;
  candidates: number;
  iterations: number;
  plies: number;
  endgame: boolean;
}

const STATIC_SEAT: SeatForm = {
  name: 'static',
  leaves: 'learned',
  simulate: false,
  candidates: 10,
  iterations: 200,
  plies: 2,
  endgame: false,
};

const SIM_SEAT: SeatForm = { ...STATIC_SEAT, name: 'sim', simulate: true, endgame: true };

const PRESETS: { label: string; seats: [SeatForm, SeatForm] }[] = [
  { label: 'Simulation vs static', seats: [SIM_SEAT, STATIC_SEAT] },
  {
    label: 'Static vs static (instant)',
    seats: [
      { ...STATIC_SEAT, name: 'learned' },
      { ...STATIC_SEAT, name: 'heuristic', leaves: 'heuristic' },
    ],
  },
  {
    label: 'Simulation vs simulation',
    seats: [
      { ...SIM_SEAT, name: 'sim-a' },
      { ...SIM_SEAT, name: 'sim-b' },
    ],
  },
];

/** Server-side limits (apps/server/games.h), enforced here for early feedback. */
const LIMITS = { candidates: [1, 20], iterations: [16, 1000], plies: [0, 4] } as const;

function toPlayerConfig(seat: SeatForm): PlayerConfig {
  return {
    name: seat.name,
    leaves: seat.leaves,
    simulation: {
      enabled: seat.simulate,
      candidates: seat.candidates,
      iterations: seat.iterations,
      plies: seat.plies,
    },
    endgame: { enabled: seat.endgame },
  };
}

function NumberField({
  label,
  value,
  range,
  disabled,
  onChange,
}: {
  label: string;
  value: number;
  range: readonly [number, number];
  disabled: boolean;
  onChange: (value: number) => void;
}) {
  return (
    <label className="field">
      <span>{label}</span>
      <input
        className="text-input"
        type="number"
        min={range[0]}
        max={range[1]}
        value={value}
        disabled={disabled}
        onChange={(event) => {
          const parsed = Number.parseInt(event.target.value, 10);
          if (!Number.isNaN(parsed)) onChange(parsed);
        }}
      />
    </label>
  );
}

function SeatEditor({
  seat,
  index,
  onChange,
}: {
  seat: SeatForm;
  index: number;
  onChange: (seat: SeatForm) => void;
}) {
  const set = <K extends keyof SeatForm>(key: K, value: SeatForm[K]) =>
    onChange({ ...seat, [key]: value });
  return (
    <fieldset className="seat-editor">
      <legend>{index === 0 ? 'Player 1 (moves first)' : 'Player 2'}</legend>
      <div className="field-row">
        <label className="field">
          <span>Name</span>
          <input
            className="text-input"
            value={seat.name}
            maxLength={24}
            onChange={(event) => set('name', event.target.value.replace(/[^A-Za-z0-9_-]/g, ''))}
          />
        </label>
        <label className="field">
          <span>Leaves</span>
          <select
            className="text-input"
            value={seat.leaves}
            onChange={(event) => set('leaves', event.target.value as LeaveModel)}
          >
            <option value="learned">Learned</option>
            <option value="heuristic">Heuristic</option>
            <option value="zero">None (greedy)</option>
          </select>
        </label>
      </div>
      <div className="field-row">
        <label className="checkbox">
          <input
            type="checkbox"
            checked={seat.simulate}
            onChange={(event) => set('simulate', event.target.checked)}
          />
          Monte Carlo simulation
        </label>
        <label className="checkbox">
          <input
            type="checkbox"
            checked={seat.endgame}
            onChange={(event) => set('endgame', event.target.checked)}
          />
          Endgame solver
        </label>
      </div>
      <div className="field-row">
        <NumberField
          label="Candidates"
          value={seat.candidates}
          range={LIMITS.candidates}
          disabled={!seat.simulate}
          onChange={(value) => set('candidates', value)}
        />
        <NumberField
          label="Iterations"
          value={seat.iterations}
          range={LIMITS.iterations}
          disabled={!seat.simulate}
          onChange={(value) => set('iterations', value)}
        />
        <NumberField
          label="Plies"
          value={seat.plies}
          range={LIMITS.plies}
          disabled={!seat.simulate}
          onChange={(value) => set('plies', value)}
        />
      </div>
    </fieldset>
  );
}

export interface GameSetupProps {
  busy: boolean;
  error: string | null;
  onStart: (request: StartGameRequest) => void;
}

/** Starts a self-play game or loads a GCG record for replay. */
export function GameSetup({ busy, error, onStart }: GameSetupProps) {
  const [mode, setMode] = useState<'selfplay' | 'replay'>('selfplay');
  const [seats, setSeats] = useState<[SeatForm, SeatForm]>(PRESETS[0].seats);
  const [seed, setSeed] = useState('');
  const [rules, setRules] = useState<EndRulesName>('naspa');
  const [gcg, setGcg] = useState('');

  const sameNames = seats[0].name === seats[1].name;
  const emptyName = seats.some((seat) => seat.name.length === 0);
  const seedValue = seed.trim() === '' ? undefined : Number(seed);
  const badSeed =
    seedValue !== undefined && (!Number.isSafeInteger(seedValue) || seedValue < 0);

  const submit = (event: FormEvent) => {
    event.preventDefault();
    if (mode === 'replay') {
      onStart({ mode: 'replay', gcg });
      return;
    }
    onStart({
      mode: 'selfplay',
      seed: seedValue,
      rules,
      players: [toPlayerConfig(seats[0]), toPlayerConfig(seats[1])],
    });
  };

  const loadFile = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    if (file) setGcg(await file.text());
  };

  const invalid =
    mode === 'selfplay' ? sameNames || emptyName || badSeed : gcg.trim().length === 0;

  return (
    <form className="panel game-setup" onSubmit={submit} aria-labelledby="setup-heading">
      <div className="panel-header">
        <h2 id="setup-heading">New game</h2>
        <div className="segmented" role="tablist" aria-label="Game source">
          <button
            type="button"
            role="tab"
            aria-selected={mode === 'selfplay'}
            className={mode === 'selfplay' ? 'is-active' : undefined}
            onClick={() => setMode('selfplay')}
          >
            Self-play
          </button>
          <button
            type="button"
            role="tab"
            aria-selected={mode === 'replay'}
            className={mode === 'replay' ? 'is-active' : undefined}
            onClick={() => setMode('replay')}
          >
            Replay GCG
          </button>
        </div>
      </div>

      {mode === 'selfplay' ? (
        <>
          <div className="field-row">
            <label className="field">
              <span>Preset</span>
              <select
                className="text-input"
                onChange={(event) => {
                  const preset = PRESETS[Number(event.target.value)];
                  if (preset) setSeats(preset.seats);
                }}
              >
                {PRESETS.map((preset, i) => (
                  <option key={preset.label} value={i}>
                    {preset.label}
                  </option>
                ))}
              </select>
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
          {seats.map((seat, i) => (
            <SeatEditor
              key={i}
              seat={seat}
              index={i}
              onChange={(next) =>
                setSeats(i === 0 ? [next, seats[1]] : [seats[0], next])
              }
            />
          ))}
          {sameNames && <p className="verdict is-illegal">The players need different names.</p>}
          {badSeed && <p className="verdict is-illegal">The seed must be a whole number.</p>}
          <p className="hint">
            Simulation costs about 0.1–2 s per move depending on candidates × iterations; the
            server runs up to two games at a time.
          </p>
        </>
      ) : (
        <>
          <textarea
            className="text-input sequence-text gcg-text"
            value={gcg}
            onChange={(event) => setGcg(event.target.value)}
            placeholder={'#player1 alice Alice\n#player2 bob Bob\n>alice: AEINRST 8D RETAINS +76 76\n…'}
            aria-label="GCG record"
            spellCheck={false}
            rows={8}
          />
          <label className="button-ghost file-button">
            Open .gcg file
            <input type="file" accept=".gcg,text/plain" onChange={loadFile} hidden />
          </label>
          <p className="hint">
            Moves are checked against the board. Words missing from the lexicon are kept and
            flagged; each position is analysed with the static evaluator.
          </p>
        </>
      )}

      {error && <p className="verdict is-illegal">{error}</p>}
      <button type="submit" className="button-primary" disabled={busy || invalid}>
        {busy ? 'Starting…' : mode === 'selfplay' ? 'Start game' : 'Load replay'}
      </button>
    </form>
  );
}
