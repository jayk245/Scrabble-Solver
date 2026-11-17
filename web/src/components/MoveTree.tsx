import { useEffect, useRef, type ReactNode } from 'react';

import { nodeAt, rackBefore, type GameNode, type GameTree } from '../lib/gameTree';
import { GRADE_LABEL, GRADE_SYMBOL, type MoveGrade } from '../lib/review';

export interface MoveTreeProps {
  tree: GameTree;
  /** Review grades by node id (the node a move leads to). */
  grades: ReadonlyMap<string, MoveGrade>;
  showRacks: boolean;
  onGoto: (id: string) => void;
}

function moveText(node: GameNode): string {
  const origin = node.origin!;
  return origin.label ?? origin.move.notation;
}

function signed(score: number): string {
  return score >= 0 ? `+${score}` : `${score}`;
}

function GradeBadge({ grade }: { grade: MoveGrade | undefined }) {
  if (!grade || GRADE_SYMBOL[grade.grade] === '') return null;
  return (
    <span
      className={`grade-badge grade-${grade.grade}`}
      title={`${GRADE_LABEL[grade.grade]}: ${grade.lost.toFixed(1)} points below ${grade.best.move.notation}`}
    >
      {GRADE_SYMBOL[grade.grade]}
    </span>
  );
}

/**
 * The game as a move tree, the way a chess analysis board shows one: the
 * main line as rows, and each variation indented under the move it
 * replaces, as a compact run of moves that can itself branch. Clicking
 * any move shows the position after it.
 */
export function MoveTree({ tree, grades, showRacks, onGoto }: MoveTreeProps) {
  const list = useRef<HTMLDivElement>(null);

  // Keeps the current move in view by scrolling the list only, never the page.
  useEffect(() => {
    const container = list.current;
    const row = container?.querySelector<HTMLElement>('[aria-current="true"]');
    if (!container || !row) return;
    const top = row.offsetTop;
    if (top < container.scrollTop) {
      container.scrollTop = top;
    } else if (top + row.offsetHeight > container.scrollTop + container.clientHeight) {
      container.scrollTop = top + row.offsetHeight - container.clientHeight;
    }
  }, [tree.currentId]);

  const rackOf = (node: GameNode) => {
    if (!showRacks) return null;
    const rack = rackBefore(tree, node);
    return rack ? <span className="tree-rack">{rack}</span> : null;
  };

  /** A variation: a run of moves from `id`, with nested branches in brackets. */
  const variation = (id: string, ply: number): ReactNode => {
    const parts: ReactNode[] = [];
    let at: GameNode | null = nodeAt(tree, id);
    let n = ply;
    while (at) {
      const node: GameNode = at;
      const current = node.id === tree.currentId;
      parts.push(
        <button
          key={node.id}
          type="button"
          className={`tree-move seat-${node.origin!.player}${current ? ' is-current' : ''}`}
          aria-current={current}
          onClick={() => onGoto(node.id)}
          title={`${tree.names[node.origin!.player]}: ${moveText(node)} (${node.scores[0]}–${node.scores[1]})`}
        >
          <span className="tree-ply">{n}.</span>
          {rackOf(node)}
          <span className="notation">{moveText(node)}</span>
          <span className="tree-score">{signed(node.origin!.move.score)}</span>
          <GradeBadge grade={grades.get(node.id)} />
        </button>,
      );
      // Alternatives to this move branch off here; the first move of a
      // variation has its alternatives listed by the caller.
      const parent = nodeAt(tree, node.parent!);
      if (parent.children[0] === node.id && node.id !== id) {
        for (const sibling of parent.children.slice(1)) {
          parts.push(
            <span key={`v${sibling}`} className="tree-nested">
              ({variation(sibling, n)})
            </span>,
          );
        }
      }
      at = node.children.length > 0 ? nodeAt(tree, node.children[0]!) : null;
      n++;
    }
    return parts;
  };

  const rows: ReactNode[] = [];
  let at: GameNode | null =
    nodeAt(tree, tree.rootId).children.length > 0
      ? nodeAt(tree, nodeAt(tree, tree.rootId).children[0]!)
      : null;
  let ply = 1;
  while (at) {
    const node: GameNode = at;
    const current = node.id === tree.currentId;
    const origin = node.origin!;
    rows.push(
      <button
        key={node.id}
        type="button"
        className={`tree-row seat-${origin.player}${current ? ' is-current' : ''}${origin.kind !== 'play' ? ' is-aside' : ''}`}
        aria-current={current}
        onClick={() => onGoto(node.id)}
      >
        <span className="tree-ply">{ply}</span>
        <span className="tree-player">{tree.names[origin.player]}</span>
        <span className="tree-main">
          {rackOf(node)}
          <span className="notation">{moveText(node)}</span>
          {origin.move.bingo && <span className="badge">bingo</span>}
          {origin.warning && (
            <span className="badge badge-warning" title={origin.warning}>
              !
            </span>
          )}
          <GradeBadge grade={grades.get(node.id)} />
        </span>
        <span className="numeric">{signed(origin.move.score)}</span>
        <span className="numeric muted">
          {node.scores[0]}–{node.scores[1]}
        </span>
      </button>,
    );
    const parent = nodeAt(tree, node.parent!);
    for (const sibling of parent.children.slice(1)) {
      rows.push(
        <div key={`v${sibling}`} className="tree-variation">
          {variation(sibling, ply)}
        </div>,
      );
    }
    at = node.children.length > 0 ? nodeAt(tree, node.children[0]!) : null;
    ply++;
  }

  const rootCurrent = tree.currentId === tree.rootId;
  const root = nodeAt(tree, tree.rootId);
  return (
    <div className="move-tree" ref={list}>
      <button
        type="button"
        className={`tree-row is-start${rootCurrent ? ' is-current' : ''}`}
        aria-current={rootCurrent}
        onClick={() => onGoto(tree.rootId)}
      >
        <span className="tree-ply">0</span>
        <span className="tree-player muted">start</span>
        <span className="tree-main muted">starting position</span>
        <span className="numeric" />
        <span className="numeric muted">
          {root.scores[0]}–{root.scores[1]}
        </span>
      </button>
      {rows.length === 0 ? (
        <p className="empty-state">
          No moves yet. Place tiles on the board and press Enter, play the engine&apos;s move, or
          import a game.
        </p>
      ) : (
        rows
      )}
    </div>
  );
}
