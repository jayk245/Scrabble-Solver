// Thin fetch wrappers for the API. Every call goes to a relative /api URL:
// in development Vite proxies it to the C++ server, and in production the
// C++ server serves this page itself, so no host configuration is needed.

import type {
  AnalyzeRequest,
  AnalyzeResponse,
  ApiErrorBody,
  GameSummary,
  HealthResponse,
  HumanMoveRequest,
  HumanMoveResponse,
  MoveAnalysis,
  MovesRequest,
  ParseGcgRequest,
  ParseGcgResponse,
  MovesResponse,
  PlayRequest,
  PlayResponse,
  PositionRequest,
  PositionSummary,
  Rules,
  StartGameRequest,
  StartGameResponse,
  ValidateRequest,
  ValidateResponse,
} from './types';

/** A non-2xx response, carrying the server's error code and message. */
export class ApiError extends Error {
  readonly status: number;
  readonly code: string;

  constructor(status: number, code: string, message: string) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
  }
}

function isErrorBody(value: unknown): value is ApiErrorBody {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as ApiErrorBody).error === 'string' &&
    typeof (value as ApiErrorBody).message === 'string'
  );
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  let response: Response;
  try {
    response = await fetch(path, init);
  } catch (error) {
    if (error instanceof DOMException && error.name === 'AbortError') throw error;
    throw new ApiError(0, 'network', 'Cannot reach the engine server.');
  }
  const body: unknown = await response.json().catch(() => null);
  if (!response.ok) {
    if (isErrorBody(body)) {
      throw new ApiError(response.status, body.error, body.message);
    }
    throw new ApiError(
      response.status,
      'http_error',
      `Server responded ${response.status} ${response.statusText}`,
    );
  }
  return body as T;
}

function post<T>(path: string, payload: unknown, signal?: AbortSignal): Promise<T> {
  return request<T>(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
    signal,
  });
}

export const api = {
  health: () => request<HealthResponse>('/api/health'),
  rules: () => request<Rules>('/api/rules'),
  position: (body: PositionRequest, signal?: AbortSignal) =>
    post<PositionSummary>('/api/position', body, signal),
  moves: (body: MovesRequest, signal?: AbortSignal) =>
    post<MovesResponse>('/api/moves', body, signal),
  validate: (body: ValidateRequest, signal?: AbortSignal) =>
    post<ValidateResponse>('/api/validate', body, signal),
  play: (body: PlayRequest, signal?: AbortSignal) =>
    post<PlayResponse>('/api/play', body, signal),
  startGame: (body: StartGameRequest) => post<StartGameResponse>('/api/games', body),
  cancelGame: (id: string) =>
    request<GameSummary>(`/api/games/${encodeURIComponent(id)}`, { method: 'DELETE' }),
  humanMove: (id: string, body: HumanMoveRequest) =>
    post<HumanMoveResponse>(`/api/games/${encodeURIComponent(id)}/moves`, body),
  hint: (id: string) => request<MoveAnalysis>(`/api/games/${encodeURIComponent(id)}/hint`),
  analyze: (body: AnalyzeRequest, signal?: AbortSignal) =>
    post<AnalyzeResponse>('/api/analyze', body, signal),
  parseGcg: (body: ParseGcgRequest) => post<ParseGcgResponse>('/api/gcg', body),
};

/** Server-Sent Events stream of a game (consumed with EventSource). */
export function gameEventsUrl(id: string): string {
  return `/api/games/${encodeURIComponent(id)}/events`;
}

/** The game record; the server sends it as a download. */
export function gameGcgUrl(id: string): string {
  return `/api/games/${encodeURIComponent(id)}/gcg`;
}
