import { randomUUID } from "node:crypto";

import {
  EVENT_HISTORY_LIMIT,
  MAX_EVENT_LINE_BYTES,
  type EventEnvelope,
  type EventType,
  PROTOCOL_VERSION,
  ProtocolError,
} from "./protocol.js";

interface SessionEvents {
  nextSequence: number;
  events: EventEnvelope[];
  eventBytes: number[];
  totalBytes: number;
}

type EventListener = (event: EventEnvelope) => void;

export const DEFAULT_EVENT_HISTORY_BYTES = 128 * 1024;

export class EventStore {
  readonly #sessions = new Map<string, SessionEvents>();

  public constructor(
    private readonly historyLimit = EVENT_HISTORY_LIMIT,
    private readonly historyBytesLimit = DEFAULT_EVENT_HISTORY_BYTES,
  ) {
    if (historyLimit <= 0) {
      throw new RangeError("historyLimit must be positive");
    }
    if (historyBytesLimit <= 0) {
      throw new RangeError("historyBytesLimit must be positive");
    }
  }

  readonly #listeners = new Map<string, Set<EventListener>>();

  public append(
    sessionId: string,
    type: EventType,
    payload: Record<string, unknown>,
  ): EventEnvelope {
    const session = this.#getOrCreateSession(sessionId);
    const event: EventEnvelope = {
      protocolVersion: PROTOCOL_VERSION,
      eventId: `evt_${randomUUID()}`,
      sequence: session.nextSequence,
      sessionId,
      type,
      createdAt: new Date().toISOString(),
      payload,
    };
    const eventBytes = Buffer.byteLength(JSON.stringify(event), "utf8") + 1;
    if (eventBytes > MAX_EVENT_LINE_BYTES + 1) {
      throw new ProtocolError(
        500,
        "EVENT_TOO_LARGE",
        `event exceeds the ${MAX_EVENT_LINE_BYTES}-byte protocol limit`,
      );
    }
    session.nextSequence += 1;
    session.events.push(event);
    session.eventBytes.push(eventBytes);
    session.totalBytes += eventBytes;
    while (
      session.events.length > this.historyLimit
      || session.totalBytes > this.historyBytesLimit
    ) {
      session.events.shift();
      session.totalBytes -= session.eventBytes.shift() ?? 0;
    }
    for (const listener of this.#listeners.get(sessionId) ?? []) {
      listener(event);
    }
    return event;
  }

  public after(
    sessionId: string,
    cursor: number,
    limit: number,
  ): EventEnvelope[] {
    const session = this.#getOrCreateSession(sessionId);
    const latestSequence = session.nextSequence - 1;
    if (cursor > latestSequence) {
      throw new ProtocolError(
        409,
        "EVENT_CURSOR_AHEAD",
        `event cursor ${cursor} exceeds latest sequence ${latestSequence}`,
        true,
      );
    }
    const first = session.events[0];
    if (first !== undefined && cursor < first.sequence - 1) {
      throw new ProtocolError(
        409,
        "EVENT_CURSOR_EXPIRED",
        `event cursor ${cursor} predates retained sequence ${first.sequence}`,
        true,
      );
    }
    return session.events
      .filter((event) => event.sequence > cursor)
      .slice(0, limit);
  }

  public latestSequence(sessionId: string): number {
    return this.#getOrCreateSession(sessionId).nextSequence - 1;
  }

  public subscribe(sessionId: string, listener: EventListener): () => void {
    let listeners = this.#listeners.get(sessionId);
    if (listeners === undefined) {
      listeners = new Set<EventListener>();
      this.#listeners.set(sessionId, listeners);
    }
    listeners.add(listener);
    return () => {
      listeners?.delete(listener);
      if (listeners?.size === 0) {
        this.#listeners.delete(sessionId);
      }
    };
  }

  #getOrCreateSession(sessionId: string): SessionEvents {
    let session = this.#sessions.get(sessionId);
    if (session === undefined) {
      session = {
        nextSequence: 1,
        events: [],
        eventBytes: [],
        totalBytes: 0,
      };
      this.#sessions.set(sessionId, session);
    }
    return session;
  }
}
