import { randomUUID } from "node:crypto";

import {
  EVENT_HISTORY_LIMIT,
  type EventEnvelope,
  type EventType,
  PROTOCOL_VERSION,
  ProtocolError,
} from "./protocol.js";

interface SessionEvents {
  nextSequence: number;
  events: EventEnvelope[];
}

export class EventStore {
  readonly #sessions = new Map<string, SessionEvents>();

  public constructor(
    private readonly historyLimit = EVENT_HISTORY_LIMIT,
  ) {
    if (historyLimit <= 0) {
      throw new RangeError("historyLimit must be positive");
    }
  }

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
    session.nextSequence += 1;
    session.events.push(event);
    if (session.events.length > this.historyLimit) {
      session.events.splice(0, session.events.length - this.historyLimit);
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

  #getOrCreateSession(sessionId: string): SessionEvents {
    let session = this.#sessions.get(sessionId);
    if (session === undefined) {
      session = {nextSequence: 1, events: []};
      this.#sessions.set(sessionId, session);
    }
    return session;
  }
}
