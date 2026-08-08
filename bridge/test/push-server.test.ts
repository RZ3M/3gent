import { once } from "node:events";
import { connect, type Socket } from "node:net";
import type { AddressInfo } from "node:net";
import test from "node:test";
import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";

import type { AgentAdapter } from "../src/agent-adapter.js";
import { EventStore } from "../src/event-store.js";
import { FAKE_SESSION_ID } from "../src/fake-agent.js";
import {
  createPushControlServer,
  MAX_PUSH_FRAME_BYTES,
} from "../src/push-server.js";
import { BridgeApplication } from "../src/server.js";
import type { SessionSummary } from "../src/protocol.js";

interface Frame {
  protocolVersion: number;
  type: string;
  [key: string]: unknown;
}

interface TestPushServer {
  application: BridgeApplication;
  port: number;
  close: () => Promise<void>;
}

class TestClient {
  readonly socket: Socket;
  readonly frames: Frame[] = [];
  #buffer = "";
  #waiters = new Set<() => void>();

  public constructor(port: number) {
    this.socket = connect({host: "127.0.0.1", port});
    this.socket.setEncoding("utf8");
    this.socket.on("data", (chunk: string) => {
      this.#buffer += chunk;
      for (;;) {
        const newline = this.#buffer.indexOf("\n");
        if (newline < 0) {
          break;
        }
        const line = this.#buffer.slice(0, newline);
        this.#buffer = this.#buffer.slice(newline + 1);
        if (line.length > 0) {
          this.frames.push(JSON.parse(line) as Frame);
        }
      }
      for (const waiter of this.#waiters) {
        waiter();
      }
    });
  }

  public async connected(): Promise<void> {
    if (!this.socket.connecting) {
      return;
    }
    await once(this.socket, "connect");
  }

  public send(frame: Record<string, unknown>): void {
    this.socket.write(`${JSON.stringify(frame)}\n`);
  }

  public async waitFor(
    predicate: (frame: Frame) => boolean,
    timeoutMs = 1_000,
  ): Promise<Frame> {
    const existing = this.frames.find(predicate);
    if (existing !== undefined) {
      return existing;
    }
    return await new Promise<Frame>((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.#waiters.delete(check);
        reject(new Error("timed out waiting for push frame"));
      }, timeoutMs);
      const check = () => {
        const frame = this.frames.find(predicate);
        if (frame === undefined) {
          return;
        }
        clearTimeout(timeout);
        this.#waiters.delete(check);
        resolve(frame);
      };
      this.#waiters.add(check);
    });
  }

  public close(): void {
    this.socket.destroy();
  }
}

async function startPushServer(
  clientIdleTimeoutMs = 12_000,
  logger: (message: string) => void = () => undefined,
  verbose = false,
  verboseHeartbeats = false,
  dropNextAcknowledgementAfterExecution = false,
): Promise<TestPushServer> {
  const application = new BridgeApplication({
    fakeDeltaIntervalMs: 5,
    logger,
  });
  const server = createPushControlServer(application, {
    clientIdleTimeoutMs,
    logger,
    verbose,
    verboseHeartbeats,
    dropNextAcknowledgementAfterExecution,
  });
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  return {
    application,
    port: (server.address() as AddressInfo).port,
    close: async () => {
      application.shutdown();
      await new Promise<void>((resolve, reject) => {
        server.close((error) => error === undefined ? resolve() : reject(error));
      });
    },
  };
}

function hello(after = 0): Record<string, unknown> {
  return {
    protocolVersion: 1,
    type: "connection.hello",
    sessionId: FAKE_SESSION_ID,
    after,
  };
}

test("pushes replay and live agent events without polling", async () => {
  const fixture = await startPushServer();
  const client = new TestClient(fixture.port);
  try {
    await client.connected();
    client.send(hello());
    await client.waitFor((frame) => frame.type === "connection.ready");
    await client.waitFor((frame) => frame.type === "event");

    client.send({
      protocolVersion: 1,
      type: "command",
      commandId: "cmd_push_text_1",
      command: {type: "capture.text", text: "hello over push"},
    });
    await client.waitFor((frame) => frame.type === "command.ack");
    const completed = await client.waitFor((frame) => {
      if (frame.type !== "event") {
        return false;
      }
      const event = frame.event as Record<string, unknown> | undefined;
      return event?.type === "turn.completed";
    });
    assert.equal(completed.type, "event");
    assert.ok(client.frames.some((frame) => {
      const event = frame.event as Record<string, unknown> | undefined;
      return event?.type === "assistant.text.delta";
    }));
  } finally {
    client.close();
    await fixture.close();
  }
});

test("deduplicates a command replayed after reconnect", async () => {
  const fixture = await startPushServer();
  const first = new TestClient(fixture.port);
  let second: TestClient | null = null;
  try {
    await first.connected();
    first.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await first.waitFor((frame) => frame.type === "connection.ready");
    first.send({
      protocolVersion: 1,
      type: "command",
      commandId: "cmd_push_retry_1",
      command: {type: "capture.text", text: "run once"},
    });
    await first.waitFor((frame) => frame.type === "command.ack");
    first.close();

    second = new TestClient(fixture.port);
    await second.connected();
    second.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await second.waitFor((frame) => frame.type === "connection.ready");
    second.send({
      protocolVersion: 1,
      type: "command",
      commandId: "cmd_push_retry_1",
      command: {type: "capture.text", text: "run once"},
    });
    const acknowledgement = await second.waitFor(
      (frame) => frame.type === "command.ack",
    );
    const body = acknowledgement.acknowledgement as Record<string, unknown>;
    assert.equal(body.duplicate, true);
  } finally {
    first.close();
    second?.close();
    await fixture.close();
  }
});

test("replays safely when the acknowledgement is lost after execution", async () => {
  const fixture = await startPushServer(
    12_000,
    () => undefined,
    false,
    false,
    true,
  );
  const first = new TestClient(fixture.port);
  let second: TestClient | null = null;
  try {
    await first.connected();
    first.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await first.waitFor((frame) => frame.type === "connection.ready");
    first.send({
      protocolVersion: 1,
      type: "command",
      commandId: "cmd_push_lost_ack_1",
      command: {type: "capture.text", text: "execute exactly once"},
    });
    await once(first.socket, "close");
    assert.ok(first.frames.every((frame) => frame.type !== "command.ack"));

    second = new TestClient(fixture.port);
    await second.connected();
    second.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await second.waitFor((frame) => frame.type === "connection.ready");
    second.send({
      protocolVersion: 1,
      type: "command",
      commandId: "cmd_push_lost_ack_1",
      command: {type: "capture.text", text: "execute exactly once"},
    });
    const frame = await second.waitFor((candidate) => candidate.type === "command.ack");
    const acknowledgement = frame.acknowledgement as Record<string, unknown>;
    assert.equal(acknowledgement.duplicate, true);
    assert.equal(
      fixture.application.events
        .after(FAKE_SESSION_ID, 0, 256)
        .filter((event) => event.type === "capture.accepted").length,
      1,
    );
  } finally {
    first.close();
    second?.close();
    await fixture.close();
  }
});

test("reports a resync snapshot for an invalid cursor", async () => {
  const fixture = await startPushServer();
  const client = new TestClient(fixture.port);
  try {
    await client.connected();
    client.send(hello(99_999));
    const frame = await client.waitFor((candidate) => candidate.type === "resync.required");
    assert.equal(frame.code, "EVENT_CURSOR_AHEAD");
    assert.equal(typeof frame.lastSequence, "number");
    assert.equal(typeof frame.session, "object");
  } finally {
    client.close();
    await fixture.close();
  }
});

test("rejects oversized frames and closes the peer", async () => {
  const fixture = await startPushServer();
  const client = new TestClient(fixture.port);
  try {
    await client.connected();
    client.socket.write("x".repeat(MAX_PUSH_FRAME_BYTES + 1));
    const frame = await client.waitFor((candidate) => candidate.type === "error");
    const error = frame.error as Record<string, unknown>;
    assert.equal(error.code, "FRAME_TOO_LARGE");
    await once(client.socket, "close");
  } finally {
    client.close();
    await fixture.close();
  }
});

test("accepts multiple individually bounded frames in one TCP chunk", async () => {
  const fixture = await startPushServer();
  const client = new TestClient(fixture.port);
  try {
    await client.connected();
    client.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await client.waitFor((frame) => frame.type === "connection.ready");
    const first = JSON.stringify({
      protocolVersion: 1,
      type: "ping",
      nonce: 41,
      padding: "x".repeat(2_100),
    });
    const second = JSON.stringify({
      protocolVersion: 1,
      type: "ping",
      nonce: 42,
      padding: "y".repeat(2_100),
    });
    assert.ok(Buffer.byteLength(first) < MAX_PUSH_FRAME_BYTES);
    assert.ok(Buffer.byteLength(second) < MAX_PUSH_FRAME_BYTES);
    assert.ok(Buffer.byteLength(`${first}\n${second}\n`) > MAX_PUSH_FRAME_BYTES);
    client.socket.write(`${first}\n${second}\n`);
    await client.waitFor((frame) => frame.type === "pong" && frame.nonce === 42);
  } finally {
    client.close();
    await fixture.close();
  }
});

test("disconnects a client that sends no heartbeat", async () => {
  const fixture = await startPushServer(100);
  const client = new TestClient(fixture.port);
  try {
    await client.connected();
    client.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await client.waitFor((frame) => frame.type === "connection.ready");
    await once(client.socket, "close");
    assert.equal(client.socket.destroyed, true);
  } finally {
    client.close();
    await fixture.close();
  }
});

test("continues processing heartbeats while an adapter command is slow", async () => {
  const events = new EventStore();
  let release!: () => void;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  const adapter: AgentAdapter = {
    id: "delayed",
    listSessions: () => [delayedSession(events)],
    session: (sessionId) => sessionId === FAKE_SESSION_ID ? delayedSession(events) : undefined,
    startSession: async () => delayedSession(events),
    resumeSession: async () => delayedSession(events),
    submitText: async () => await gate,
    interrupt: async () => {},
    respondToApproval: async () => {},
    shutdown: async () => {},
  };
  const application = new BridgeApplication({adapter, events, logger: () => {}});
  const server = createPushControlServer(application, {
    clientIdleTimeoutMs: 80,
    logger: () => {},
  });
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const client = new TestClient((server.address() as AddressInfo).port);
  try {
    await client.connected();
    client.send(hello());
    await client.waitFor((frame) => frame.type === "connection.ready");
    client.send({
      protocolVersion: 1,
      type: "command",
      commandId: "cmd_slow_adapter",
      command: {type: "capture.text", text: "slow"},
    });
    for (let nonce = 1; nonce <= 6; nonce += 1) {
      await delay(25);
      client.send({protocolVersion: 1, type: "ping", nonce});
    }
    await client.waitFor((frame) => frame.type === "pong" && frame.nonce === 6);
    release();
    await client.waitFor((frame) => frame.type === "command.ack");
  } finally {
    release();
    client.close();
    await application.shutdown();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

test("event history enforces its byte budget", () => {
  const events = new EventStore(256, 900);
  for (let index = 0; index < 10; index += 1) {
    events.append(FAKE_SESSION_ID, "session.updated", {
      state: "idle",
      detail: "x".repeat(100),
    });
  }
  assert.throws(
    () => events.after(FAKE_SESSION_ID, 0, 256),
    (error: unknown) => error instanceof Error
      && "code" in error
      && error.code === "EVENT_CURSOR_EXPIRED",
  );
});

test("slow peers are bounded and receive resync instead of an event queue", async () => {
  const fixture = await startPushServer();
  const client = new TestClient(fixture.port);
  try {
    await client.connected();
    client.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await client.waitFor((frame) => frame.type === "connection.ready");
    client.socket.pause();
    for (let index = 0; index < 10_000; index += 1) {
      fixture.application.events.append(FAKE_SESSION_ID, "capture.progress", {
        index,
        detail: "x".repeat(300),
      });
    }
    await delay(20);
    client.socket.resume();
    const resync = await client.waitFor(
      (frame) => frame.type === "resync.required",
      2_000,
    );
    assert.equal(resync.code, "EVENT_CURSOR_EXPIRED");
  } finally {
    client.close();
    await fixture.close();
  }
});

test("ordinary verbose push logging hides heartbeat frames", async () => {
  const messages: string[] = [];
  const fixture = await startPushServer(
    12_000,
    (message) => messages.push(message),
    true,
  );
  const client = new TestClient(fixture.port);
  try {
    await client.connected();
    client.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await client.waitFor((frame) => frame.type === "connection.ready");
    messages.length = 0;
    client.send({protocolVersion: 1, type: "ping", nonce: 7});
    await client.waitFor((frame) => frame.type === "pong");
    assert.ok(messages.every((message) => !message.includes('"type":"ping"')));
    assert.ok(messages.every((message) => !message.includes('"type":"pong"')));
  } finally {
    client.close();
    await fixture.close();
  }
});

test("noisy transport logging includes heartbeat frames", async () => {
  const messages: string[] = [];
  const fixture = await startPushServer(
    12_000,
    (message) => messages.push(message),
    true,
    true,
  );
  const client = new TestClient(fixture.port);
  try {
    await client.connected();
    client.send(hello(fixture.application.events.latestSequence(FAKE_SESSION_ID)));
    await client.waitFor((frame) => frame.type === "connection.ready");
    messages.length = 0;
    client.send({protocolVersion: 1, type: "ping", nonce: 8});
    await client.waitFor((frame) => frame.type === "pong");
    assert.ok(messages.some((message) => message.includes('"type":"ping"')));
    assert.ok(messages.some((message) => message.includes('"type":"pong"')));
  } finally {
    client.close();
    await fixture.close();
  }
});

function delayedSession(events: EventStore): SessionSummary {
  return {
    sessionId: FAKE_SESSION_ID,
    label: "Delayed adapter",
    adapter: "delayed",
    state: "idle",
    activeTurnId: null,
    pendingApprovalId: null,
    lastSequence: events.latestSequence(FAKE_SESSION_ID),
  };
}
