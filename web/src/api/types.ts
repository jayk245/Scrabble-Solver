// Wire types for the C++ server's JSON API (apps/server). This file is the
// client-side mirror of apps/server/codec.h and api.h; change them together.

/**
 * A board as 15 strings of 15 characters, row 1 first. '.' is an empty
 * square, 'A'-'Z' a tile and 'a'-'z' a blank designated as that letter.
 */
export type BoardRows = string[];

export type Direction = 'across' | 'down';
export type MoveType = 'place' | 'exchange' | 'pass';

/** Count per tile, keyed 'A'-'Z' plus '?' for the blank. */
export type PerLetter = Record<string, number>;

/** One square covered by a placement. Coordinates are zero-based. */
export interface MoveSquare {
  row: number;
  col: number;
  /** Uppercase, or lowercase for a blank. */
  letter: string;
  blank: boolean;
  /** Already on the board; contributes no tile from the rack. */
  playThrough: boolean;
}

export interface MoveInfo {
  /** Standard notation, e.g. "8D RETAINS", "H8 QU(I)T", "-ABC", "-". */
  notation: string;
  type: MoveType;
  score: number;
  tilesUsed: number;
  bingo: boolean;
  /** Main word for a placement (lowercase = blank); tiles for an exchange. */
  word: string;
  /** Squares of a placement, in order; empty for exchanges and passes. */
  tiles: MoveSquare[];
  /** Placement only: zero-based start square and direction. */
  row?: number;
  col?: number;
  direction?: Direction;
  /** Rack left after the move; present when the request included a rack. */
  leave?: string;
}

export interface PositionSummary {
  /** Tiles not on the board or the player's rack. */
  unseen: PerLetter;
  unseenCount: number;
  /** Unseen tiles minus a full opponent rack, floored at zero. */
  bagCount: number;
  tilesOnBoard: number;
  /** Letters used more often than the distribution allows, e.g. "Z?". */
  overused: string;
}

/** GET /api/health */
export interface HealthResponse {
  status: 'ok';
  version: string;
  lexicon: string;
  words: number;
}

/** GET /api/rules */
export interface Rules {
  boardSize: number;
  rackSize: number;
  bingoBonus: number;
  /**
   * 15 strings: 'T' triple word, 'D' double word, 't' triple letter,
   * 'd' double letter, '*' centre (double word), '.' plain.
   */
  premiums: string[];
  tileValues: PerLetter;
  tileCounts: PerLetter;
}

/** POST /api/position. An omitted board means the empty board. */
export interface PositionRequest {
  board?: BoardRows;
  rack?: string;
}

/** POST /api/moves */
export interface MovesRequest {
  board?: BoardRows;
  /** Up to 7 tiles, '?' for a blank. */
  rack: string;
  /** Maximum moves returned (default 1000, capped at 100000). */
  limit?: number;
}

export interface MovesResponse {
  /** Best score first. */
  moves: MoveInfo[];
  /** Number of legal moves before the limit was applied. */
  total: number;
  truncated: boolean;
  /** Server-side generation and sorting time. */
  elapsedMs: number;
  position: PositionSummary;
}

/** POST /api/validate. Without a rack, tile ownership is not checked. */
export interface ValidateRequest {
  board?: BoardRows;
  rack?: string;
  move: string;
}

export type MoveErrorCode =
  | 'empty_move'
  | 'out_of_bounds'
  | 'bad_tile'
  | 'square_occupied'
  | 'play_through_empty'
  | 'play_through_mismatch'
  | 'incomplete_word'
  | 'no_rack_tiles'
  | 'too_many_tiles'
  | 'not_in_rack'
  | 'word_too_short'
  | 'first_move_off_center'
  | 'not_connected'
  | 'invalid_word'
  | 'invalid_cross_word';

export type ValidateResponse =
  | { valid: true; move: MoveInfo }
  | { valid: false; error: MoveErrorCode; message: string };

/**
 * POST /api/play. An illegal move is answered with 422 and an ApiErrorBody
 * whose `error` is a MoveErrorCode.
 */
export type PlayRequest = ValidateRequest;

export interface PlayResponse {
  board: BoardRows;
  move: MoveInfo;
  /** The leave; present when the request included a rack. */
  rack?: string;
  position: PositionSummary;
}

/** Body of every 4xx/5xx response. */
export interface ApiErrorBody {
  /** Stable snake_case code, e.g. "bad_board", "bad_notation". */
  error: string;
  message: string;
}

// ---------------------------------------------------------------------------
// Game viewer: bot-vs-bot self-play and GCG replay (apps/server/games.h).
//
//   POST   /api/games               StartGameRequest -> StartGameResponse (201)
//   GET    /api/games/{id}/events   text/event-stream of GameEvent. The SSE
//                                   "event:" field is GameEvent["type"] and
//                                   "id:" the event's index, so a
//                                   reconnecting EventSource resumes via
//                                   Last-Event-ID.
//   GET    /api/games/{id}          GameSummary
//   GET    /api/games/{id}/gcg      the game record as text/plain
//   DELETE /api/games/{id}          cancel a running game -> GameSummary
// ---------------------------------------------------------------------------

export type LeaveModel = 'learned' | 'heuristic' | 'zero';
export type EndRulesName = 'naspa' | 'wespa';

/** Per-seat bot settings. Every field is optional; the server caps them. */
export interface PlayerConfig {
  /** 1-24 characters: letters, digits, '_' or '-'. Must differ per seat. */
  name?: string;
  leaves?: LeaveModel;
  simulation?: {
    enabled?: boolean;
    /** 1-20 top static moves (plus the top-scoring move). */
    candidates?: number;
    /** 16-1000 rollouts per candidate. */
    iterations?: number;
    /** 0-4 moves played after the candidate. */
    plies?: number;
  };
  endgame?: {
    enabled?: boolean;
    /** 1,000-5,000,000 search nodes per decision. */
    nodeLimit?: number;
  };
}

export type StartGameRequest =
  | {
      mode: 'selfplay';
      /** 0 to 2^53 - 1; random when omitted (the response reports it). */
      seed?: number;
      rules?: EndRulesName;
      /** Static alternatives listed per unsimulated move (1-30). */
      analysisCandidates?: number;
      players?: [PlayerConfig, PlayerConfig];
    }
  | { mode: 'replay'; gcg: string };

export interface StartGameResponse {
  id: string;
  mode: 'selfplay' | 'replay';
  /** URL of the event stream. */
  events: string;
  seed?: number;
}

export interface GameSummary {
  id: string;
  mode: 'selfplay' | 'replay';
  status: 'running' | 'finished' | 'cancelled';
  events: number;
  hasGcg: boolean;
}

/** A pair indexed by seat; seat 0 moved first. */
export type Seats<T> = [T, T];

/** A player as described by the start event. */
export interface GamePlayer {
  name: string;
  /** Replays: the GCG full name. */
  fullName?: string;
  /** Self-play: the settings the bot ran with, after defaults. */
  leaves?: LeaveModel;
  simulation?: Required<NonNullable<PlayerConfig['simulation']>>;
  endgame?: Required<NonNullable<PlayerConfig['endgame']>>;
}

export interface GameStartEvent {
  type: 'start';
  mode: 'selfplay' | 'replay';
  players: Seats<GamePlayer>;
  board: BoardRows;
  /** Opening racks; null where a replayed record does not say. */
  racks: Seats<string | null>;
  bag: number;
  scores: Seats<number>;
  /** Self-play only. */
  seed?: number;
  rules?: EndRulesName;
  analysisCandidates?: number;
  /** Replay only. */
  title?: string;
  lexicon?: string;
}

/** Rollout statistics for one simulated candidate (spread points). */
export interface SimStats {
  mean: number;
  stdError: number;
  stddev: number;
  iterations: number;
  pruned: boolean;
  /** Chosen move's value minus this one, paired over common iterations. */
  diffFromBest: number;
  diffStdError: number;
}

export interface Candidate {
  move: MoveInfo;
  /** Static equity: score plus leave value. */
  equity: number;
  /** 1-based position in the static ranking (ties share a rank). */
  staticRank: number | null;
  /** The move that was actually played. */
  chosen: boolean;
  sim?: SimStats;
}

export interface MoveAnalysis {
  /** How the move was decided (replays are always analysed statically). */
  method: 'static' | 'simulation' | 'endgame';
  /** Legal moves in the position. */
  totalMoves: number;
  /** Static rank and equity of the played move; null if the generator does
   * not produce it (e.g. a phony). */
  playedRank: number | null;
  playedEquity: number | null;
  /** Simulated candidates (best mean first), or the top static moves. */
  candidates: Candidate[];
  simulation?: {
    iterations: number;
    rollouts: number;
    seconds: number;
    stop: 'single_candidate' | 'iterations' | 'time';
    plies: number;
  };
  endgame?: {
    /** Final spread for the mover under best play (exact if complete). */
    value: number;
    complete: boolean;
    depth: number;
    nodes: number;
    seconds: number;
    /** Principal variation, starting with the move played. */
    pv: string[];
  };
}

export interface GameMoveEvent {
  type: 'move';
  /** 0-based index among move events. */
  turn: number;
  player: 0 | 1;
  /**
   * 'play': a placement, exchange or pass. Replays add 'withdrawn' (a phony
   * taken back; the board reverts) and 'adjustment' (challenge bonus or
   * time penalty), which have no move.
   */
  kind: 'play' | 'withdrawn' | 'adjustment';
  /** Display text, e.g. "8D RETAINS", "-- (withdrawn)", "(challenge) +5". */
  label: string;
  /** Mover's rack before the move. */
  rack: string | null;
  move: MoveInfo | null;
  /** Points for this event (negative for withdrawals and penalties). */
  score: number;
  /** Self-play: the bot's estimate (static equity, sim mean or endgame value). */
  equity?: number;
  /** Cumulative scores after the event. */
  scores: Seats<number>;
  /** Self-play: tiles drawn after the move. */
  drawn?: string;
  /** Tiles left in the bag after the move and the draw. */
  bag: number;
  /** Board after the move. */
  board: BoardRows;
  /** Both racks after the move. */
  racks: Seats<string | null>;
  /** Self-play: time the bot took to decide. */
  elapsedMs?: number;
  /** Replay: the record's #note text. */
  note?: string;
  /** Replay: a word missing from the lexicon, or a score discrepancy. */
  warning?: string;
  analysis?: MoveAnalysis;
}

export interface GameEndEvent {
  type: 'end';
  reason: 'went_out' | 'scoreless_turns' | 'record_end' | 'cancelled' | 'error';
  /** Absent only when reason is 'error'. */
  scores?: Seats<number>;
  /** End-of-game rack adjustments, already included in `scores`. */
  adjustments?: Seats<number>;
  racks?: Seats<string | null>;
  /** Seat of the winner, -1 for a tie, null if cancelled. */
  winner?: 0 | 1 | -1 | null;
  turns?: number;
  gcg?: string;
  /** reason 'error': what went wrong. */
  message?: string;
}

export type GameEvent = GameStartEvent | GameMoveEvent | GameEndEvent;
