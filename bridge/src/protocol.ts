import type { IncomingHttpHeaders, ServerResponse } from "node:http";

export const PROTOCOL_VERSION = 1 as const;
export const PROTOCOL_HEADER = "x-3gent-protocol-version";
export const COMMAND_ID_HEADER = "x-3gent-command-id";
export const MAX_TEXT_CAPTURE_BYTES = 4 * 1024;
export const MAX_AUDIO_SECONDS = 5 * 60;
export const AUDIO_SAMPLE_RATE = 16_364;
export const AUDIO_BYTES_PER_SECOND = AUDIO_SAMPLE_RATE * 2;
export const MAX_AUDIO_BYTES = MAX_AUDIO_SECONDS * AUDIO_BYTES_PER_SECOND;
export const EVENT_HISTORY_LIMIT = 256;
export const MAX_EVENT_POLL_LIMIT = 32;
export const MAX_EVENT_LINE_BYTES = 640;

export type SessionState =
  | "offline"
  | "idle"
  | "working"
  | "waiting_for_user"
  | "completed"
  | "failed";

export type EventType =
  | "connection.ready"
  | "session.updated"
  | "turn.started"
  | "assistant.text.delta"
  | "assistant.text.completed"
  | "approval.requested"
  | "approval.resolved"
  | "capture.accepted"
  | "capture.progress"
  | "capture.photo.ready"
  | "capture.attached"
  | "capture.transcript.delta"
  | "capture.transcribed"
  | "turn.interrupted"
  | "turn.diff.updated"
  | "turn.completed"
  | "error";

export interface EventEnvelope {
  protocolVersion: typeof PROTOCOL_VERSION;
  eventId: string;
  sequence: number;
  sessionId: string;
  type: EventType;
  createdAt: string;
  payload: Record<string, unknown>;
}

export interface SessionSummary {
  sessionId: string;
  label: string;
  adapter: string;
  state: SessionState;
  activeTurnId: string | null;
  pendingApprovalId: string | null;
  lastSequence: number;
}

export interface CommandAcknowledgement {
  protocolVersion: typeof PROTOCOL_VERSION;
  commandId: string;
  accepted: true;
  duplicate: boolean;
  sessionId: string;
  lastSequence: number;
}

export interface ProtocolErrorBody {
  protocolVersion: typeof PROTOCOL_VERSION;
  error: {
    code: string;
    message: string;
    retryable: boolean;
  };
}

export class ProtocolError extends Error {
  public constructor(
    public readonly status: number,
    public readonly code: string,
    message: string,
    public readonly retryable = false,
  ) {
    super(message);
  }
}

const IDENTIFIER_PATTERN = /^(?:cmd|ses|evt|turn|cap|apr)_[A-Za-z0-9_-]+$/;

export function requireProtocolVersion(headers: IncomingHttpHeaders): void {
  const value = headers[PROTOCOL_HEADER];
  if (value !== String(PROTOCOL_VERSION)) {
    throw new ProtocolError(
      426,
      "UNSUPPORTED_PROTOCOL_VERSION",
      `expected ${PROTOCOL_HEADER}: ${PROTOCOL_VERSION}`,
    );
  }
}

export function requireCommandId(headers: IncomingHttpHeaders): string {
  const value = headers[COMMAND_ID_HEADER];
  if (typeof value !== "string" || !isIdentifier(value, "cmd")) {
    throw new ProtocolError(
      400,
      "INVALID_COMMAND_ID",
      `${COMMAND_ID_HEADER} must be a bounded cmd_ identifier`,
    );
  }
  return value;
}

export function isIdentifier(value: string, prefix?: string): boolean {
  return value.length <= 64
    && IDENTIFIER_PATTERN.test(value)
    && (prefix === undefined || value.startsWith(`${prefix}_`));
}

export function sendJson(
  response: ServerResponse,
  status: number,
  body: unknown,
): void {
  const encoded = Buffer.from(JSON.stringify(body), "utf8");
  response.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": encoded.byteLength,
    "Cache-Control": "no-store",
    Connection: "keep-alive",
  });
  response.end(encoded);
}

export function protocolErrorBody(error: ProtocolError): ProtocolErrorBody {
  return {
    protocolVersion: PROTOCOL_VERSION,
    error: {
      code: error.code,
      message: error.message,
      retryable: error.retryable,
    },
  };
}

export function encodeEventBatch(events: readonly EventEnvelope[]): Buffer {
  if (events.length === 0) {
    return Buffer.alloc(0);
  }
  const lines = events.map((event) => JSON.stringify(event));
  if (lines.some((line) => Buffer.byteLength(line, "utf8") > MAX_EVENT_LINE_BYTES)) {
    throw new ProtocolError(
      500,
      "EVENT_TOO_LARGE",
      `event exceeds the ${MAX_EVENT_LINE_BYTES}-byte protocol limit`,
    );
  }
  return Buffer.from(
    `${lines.join("\n")}\n`,
    "utf8",
  );
}
