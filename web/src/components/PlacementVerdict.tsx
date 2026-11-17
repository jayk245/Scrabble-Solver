import type { Verdict } from '../hooks/usePlacement';

/** The engine's live verdict on the tiles placed so far, plus any notice. */
export function PlacementVerdict({ verdict, notice }: { verdict: Verdict; notice: string | null }) {
  return (
    <>
      {verdict.kind === 'legal' && (
        <p className="verdict is-legal">
          <strong className="notation">{verdict.move.notation}</strong> scores{' '}
          <strong>{verdict.move.score}</strong>
          {verdict.move.bingo && ' (bingo!)'}
        </p>
      )}
      {verdict.kind === 'illegal' && <p className="verdict is-illegal">{verdict.message}</p>}
      {verdict.kind === 'checking' && <p className="hint">Checking…</p>}
      {notice && <p className="verdict is-illegal">{notice}</p>}
    </>
  );
}
