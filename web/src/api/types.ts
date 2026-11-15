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
// Planned: game streaming (self-play, GCG replay, live bot vs bot).
//
// Not implemented by the server yet (it answers 501). The shapes are fixed
// here so the analysis components can be reused by a viewer: each event
// carries a board in the same format and moves as MoveInfo.
//
//   POST /api/games               StartGameRequest -> StartGameResponse
//   GET  /api/games/{id}/events   text/event-stream of GameEvent; the SSE
//                                 "event:" field is GameEvent["type"] and
//                                 "id:" the turn number, so a reconnecting
//                                 EventSource resumes via Last-Event-ID.
// ---------------------------------------------------------------------------

export interface PlayerSpec {
  name: string;
  /** Bot identifier, e.g. "static" (best score + leave) or "sim". */
  bot: string;
}

export type StartGameRequest =
  | { mode: 'selfplay'; seed?: number; players: [PlayerSpec, PlayerSpec] }
  | { mode: 'replay'; gcg: string };

export interface StartGameResponse {
  id: string;
}

export interface GameStartEvent {
  type: 'start';
  gameId: string;
  players: [PlayerSpec, PlayerSpec];
  board: BoardRows;
  seed?: number;
}

export interface GameMoveEvent {
  type: 'move';
  turn: number;
  player: 0 | 1;
  /** Rack before the move. */
  rack: string;
  move: MoveInfo;
  /** Board after the move. */
  board: BoardRows;
  scores: [number, number];
  bagCount: number;
  /** Optional bot insight: the top alternatives it considered. */
  candidates?: MoveInfo[];
}

export interface GameEndEvent {
  type: 'end';
  scores: [number, number];
  /** Why the game ended, e.g. "out", "six_passes". */
  reason: string;
}

export type GameEvent = GameStartEvent | GameMoveEvent | GameEndEvent;
