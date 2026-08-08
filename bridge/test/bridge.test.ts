import { mkdtemp, readFile, readdir, rm } from "node:fs/promises";
import { request as httpRequest } from "node:http";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import test from "node:test";
import assert from "node:assert/strict";

import { EventStore } from "../src/event-store.js";
import { FAKE_SESSION_ID } from "../src/fake-agent.js";
import {
  COMMAND_ID_HEADER,
  type EventEnvelope,
  PROTOCOL_HEADER,
  PROTOCOL_VERSION,
} from "../src/protocol.js";
import { createBridgeServer } from "../src/server.js";
import { StaticTranscriber } from "../src/transcriber.js";
import { PHOTO_RGB565_BYTES } from "../src/photo.js";

interface TestResponse {
  status: number;
  headers: Record<string, string | string[] | undefined>;
  body: Buffer;
}

interface TestBridge {
  baseUrl: URL;
  capturePath: string;
  photoPath: string;
  directory: string;
  close: () => Promise<void>;
}

interface StartBridgeOptions {
  logger?: (message: string) => void;
  verbose?: boolean;
  verbosePolls?: boolean;
}

async function startBridge(
  options: StartBridgeOptions = {},
): Promise<TestBridge> {
  const directory = await mkdtemp(join(tmpdir(), "3gent-bridge-test-"));
  const capturePath = join(directory, "latest.wav");
  const photoPath = join(directory, "latest.bmp");
  const {application, server} = createBridgeServer({
    capturePath,
    fakeDeltaIntervalMs: 5,
    logger: options.logger ?? (() => undefined),
    verbose: options.verbose ?? false,
    verbosePolls: options.verbosePolls ?? false,
    transcriber: new StaticTranscriber("transcribed voice prompt"),
    photoPath,
  });
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address() as AddressInfo;
  return {
    baseUrl: new URL(`http://127.0.0.1:${address.port}`),
    capturePath,
    photoPath,
    directory,
    close: async () => {
      application.shutdown();
      await new Promise<void>((resolve, reject) => {
        server.close((error) => error === undefined ? resolve() : reject(error));
      });
      await rm(directory, {recursive: true, force: true});
    },
  };
}

async function request(
  bridge: TestBridge,
  method: string,
  path: string,
  body: Buffer | string = Buffer.alloc(0),
  headers: Record<string, string> = {},
): Promise<TestResponse> {
  const encodedBody = typeof body === "string" ? Buffer.from(body) : body;
  return await new Promise<TestResponse>((resolve, reject) => {
    const request = httpRequest(
      new URL(path, bridge.baseUrl),
      {
        method,
        headers: {
          [PROTOCOL_HEADER]: String(PROTOCOL_VERSION),
          "Content-Length": String(encodedBody.byteLength),
          ...headers,
        },
      },
      (response) => {
        const chunks: Buffer[] = [];
        response.on("data", (chunk: Buffer) => chunks.push(chunk));
        response.on("end", () => resolve({
          status: response.statusCode ?? 0,
          headers: response.headers,
          body: Buffer.concat(chunks),
        }));
      },
    );
    request.on("error", reject);
    request.end(encodedBody);
  });
}

function parseJson(response: TestResponse): Record<string, unknown> {
  return JSON.parse(response.body.toString("utf8")) as Record<string, unknown>;
}

function parseEvents(response: TestResponse): EventEnvelope[] {
  return response.body
    .toString("utf8")
    .split("\n")
    .filter((line) => line.length > 0)
    .map((line) => JSON.parse(line) as EventEnvelope);
}

async function poll(
  bridge: TestBridge,
  after: number,
): Promise<EventEnvelope[]> {
  const response = await request(
    bridge,
    "GET",
    `/v1/events?sessionId=${FAKE_SESSION_ID}&after=${after}&limit=32`,
  );
  assert.equal(response.status, 200);
  return parseEvents(response);
}

async function waitForEvent(
  bridge: TestBridge,
  type: EventEnvelope["type"],
  initialCursor = 0,
): Promise<{events: EventEnvelope[]; cursor: number}> {
  const deadline = Date.now() + 1_000;
  const collected: EventEnvelope[] = [];
  let cursor = initialCursor;
  while (Date.now() < deadline) {
    const events = await poll(bridge, cursor);
    collected.push(...events);
    const latest = events.at(-1);
    if (latest !== undefined) {
      cursor = latest.sequence;
    }
    if (collected.some((event) => event.type === type)) {
      return {events: collected, cursor};
    }
    await delay(5);
  }
  assert.fail(`timed out waiting for ${type}`);
}

test("requires protocol version on v1 routes", async () => {
  const bridge = await startBridge();
  try {
    const response = await new Promise<TestResponse>((resolve, reject) => {
      const request = httpRequest(
        new URL("/v1/sessions", bridge.baseUrl),
        {method: "GET"},
        (incoming) => {
          const chunks: Buffer[] = [];
          incoming.on("data", (chunk: Buffer) => chunks.push(chunk));
          incoming.on("end", () => resolve({
            status: incoming.statusCode ?? 0,
            headers: incoming.headers,
            body: Buffer.concat(chunks),
          }));
        },
      );
      request.on("error", reject);
      request.end();
    });
    assert.equal(response.status, 426);
    assert.equal(
      (parseJson(response).error as Record<string, unknown>).code,
      "UNSUPPORTED_PROTOCOL_VERSION",
    );
  } finally {
    await bridge.close();
  }
});

test("discovers the fake session", async () => {
  const bridge = await startBridge();
  try {
    const response = await request(bridge, "GET", "/v1/sessions");
    assert.equal(response.status, 200);
    const sessions = parseJson(response).sessions as Array<Record<string, unknown>>;
    assert.equal(sessions.length, 1);
    assert.equal(sessions[0]?.sessionId, FAKE_SESSION_ID);
    assert.equal(sessions[0]?.state, "idle");
  } finally {
    await bridge.close();
  }
});

test("starts and resumes sessions with idempotent command IDs", async () => {
  const bridge = await startBridge();
  try {
    const headers = {
      [COMMAND_ID_HEADER]: "cmd_session_start",
      "Content-Type": "application/json",
    };
    const first = await request(bridge, "POST", "/v1/sessions", "{}", headers);
    const replay = await request(bridge, "POST", "/v1/sessions", "{}", headers);
    assert.equal(first.status, 201);
    assert.equal(replay.status, 200);
    assert.equal(parseJson(replay).duplicate, true);
    const session = parseJson(first).session as Record<string, unknown>;
    assert.equal(session.sessionId, FAKE_SESSION_ID);

    const resumed = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/resume`,
      Buffer.alloc(0),
      {[COMMAND_ID_HEADER]: "cmd_session_resume"},
    );
    assert.equal(resumed.status, 202);
    assert.equal(
      (parseJson(resumed).session as Record<string, unknown>).sessionId,
      FAKE_SESSION_ID,
    );
  } finally {
    await bridge.close();
  }
});

test("streams ordered fake-agent events and deduplicates commands", async () => {
  const bridge = await startBridge();
  try {
    const headers = {
      [COMMAND_ID_HEADER]: "cmd_test_text_1",
      "Content-Type": "text/plain; charset=utf-8",
    };
    const first = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/text`,
      "hello from test",
      headers,
    );
    assert.equal(first.status, 202);
    assert.equal(parseJson(first).duplicate, false);

    const duplicate = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/text`,
      "hello from test",
      headers,
    );
    assert.equal(duplicate.status, 200);
    assert.equal(parseJson(duplicate).duplicate, true);

    const {events} = await waitForEvent(bridge, "turn.completed");
    assert.equal(
      events.filter((event) => event.type === "capture.accepted").length,
      1,
    );
    assert.ok(events.some((event) => event.type === "assistant.text.delta"));
    assert.deepEqual(
      events.map((event) => event.sequence),
      [...events].map((event) => event.sequence).sort((left, right) => left - right),
    );
  } finally {
    await bridge.close();
  }
});

test("pauses for and resolves a fake approval", async () => {
  const bridge = await startBridge();
  try {
    const submit = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/text`,
      "please request approval",
      {
        [COMMAND_ID_HEADER]: "cmd_test_approval_1",
        "Content-Type": "text/plain",
      },
    );
    assert.equal(submit.status, 202);
    const waiting = await waitForEvent(bridge, "approval.requested");
    const approval = waiting.events.find(
      (event) => event.type === "approval.requested",
    );
    const approvalId = approval?.payload.approvalId;
    assert.equal(typeof approvalId, "string");

    const resolveResponse = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/approvals/${String(approvalId)}/respond`,
      JSON.stringify({choice: "approve_once"}),
      {
        [COMMAND_ID_HEADER]: "cmd_test_approval_response_1",
        "Content-Type": "application/json",
      },
    );
    assert.equal(resolveResponse.status, 202);
    const completed = await waitForEvent(
      bridge,
      "turn.completed",
      waiting.cursor,
    );
    assert.ok(completed.events.some((event) => event.type === "approval.resolved"));
  } finally {
    await bridge.close();
  }
});

test("interrupts the active fake turn", async () => {
  const bridge = await startBridge();
  try {
    await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/text`,
      "a turn that will be interrupted",
      {
        [COMMAND_ID_HEADER]: "cmd_test_interrupt_submit",
        "Content-Type": "text/plain",
      },
    );
    const interrupt = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/turns/current/interrupt`,
      Buffer.alloc(0),
      {[COMMAND_ID_HEADER]: "cmd_test_interrupt"},
    );
    assert.equal(interrupt.status, 202);
    const {events} = await waitForEvent(bridge, "turn.interrupted");
    assert.ok(events.some((event) => event.type === "turn.completed"));
  } finally {
    await bridge.close();
  }
});

test("streams PCM to a valid WAV and returns a reviewed transcript without starting a turn", async () => {
  const bridge = await startBridge();
  try {
    const pcm = Buffer.from([0, 0, 16, 0, 32, 0, 48, 0]);
    const response = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/audio`,
      pcm,
      {
        [COMMAND_ID_HEADER]: "cmd_test_audio_1",
        "Content-Type": "application/x-3gent-pcm; "
          + "format=s16le; rate=16364; channels=1",
      },
    );
    assert.equal(response.status, 202);
    const duplicate = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/audio`,
      pcm,
      {
        [COMMAND_ID_HEADER]: "cmd_test_audio_1",
        "Content-Type": "application/x-3gent-pcm; "
          + "format=s16le; rate=16364; channels=1",
      },
    );
    assert.equal(duplicate.status, 200);
    assert.equal(parseJson(duplicate).duplicate, true);
    const wav = await readFile(bridge.capturePath);
    assert.equal(wav.subarray(0, 4).toString("ascii"), "RIFF");
    assert.equal(wav.readUInt32LE(24), 16_364);
    assert.equal(wav.readUInt32LE(40), pcm.byteLength);
    assert.deepEqual(wav.subarray(44), pcm);
    const {events} = await waitForEvent(bridge, "capture.transcribed");
    const capture = events.find((event) => event.type === "capture.accepted");
    assert.equal(capture?.payload.kind, "audio");
    assert.equal(
      events.filter((event) => event.type === "capture.transcript.delta")
        .map((event) => event.payload.text).join(""),
      "transcribed voice prompt",
    );
    assert.ok(events.every((event) => event.type !== "turn.started"));
  } finally {
    await bridge.close();
  }
});

test("uploads a bounded RGB565 photo and attaches it to the next prompt", async () => {
  const bridge = await startBridge();
  try {
    const pixels = Buffer.alloc(PHOTO_RGB565_BYTES, 0x1f);
    const upload = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/photo`,
      pixels,
      {
        [COMMAND_ID_HEADER]: "cmd_test_photo",
        "Content-Type": "application/x-3gent-rgb565; width=400; height=240",
      },
    );
    assert.equal(upload.status, 202);
    const bmp = await readFile(bridge.photoPath);
    assert.equal(bmp.subarray(0, 2).toString("ascii"), "BM");
    assert.equal(bmp.readInt32LE(18), 400);
    assert.equal(bmp.readInt32LE(22), -240);
    assert.equal(bmp.byteLength, 66 + PHOTO_RGB565_BYTES);
    assert.deepEqual(
      (await readdir(bridge.directory)).filter((name) => name.startsWith("latest.cmd_")),
      ["latest.cmd_test_photo.bmp"],
    );

    await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/text`,
      "inspect this",
      {
        [COMMAND_ID_HEADER]: "cmd_test_photo_prompt",
        "Content-Type": "text/plain",
      },
    );
    const {events} = await waitForEvent(bridge, "turn.completed");
    const output = events
      .filter((event) => event.type === "assistant.text.delta")
      .map((event) => event.payload.text).join("");
    assert.match(output, /photo attached/);
    assert.ok(events.some((event) => event.type === "capture.attached"));
    assert.deepEqual(
      (await readdir(bridge.directory)).filter((name) => name.startsWith("latest.cmd_")),
      [],
    );
  } finally {
    await bridge.close();
  }
});

test("rejects event cursors older than retained history", () => {
  const events = new EventStore(2);
  events.append(FAKE_SESSION_ID, "session.updated", {state: "idle"});
  events.append(FAKE_SESSION_ID, "session.updated", {state: "working"});
  events.append(FAKE_SESSION_ID, "session.updated", {state: "idle"});
  assert.throws(
    () => events.after(FAKE_SESSION_ID, 0, 2),
    (error: unknown) => error instanceof Error
      && "code" in error
      && error.code === "EVENT_CURSOR_EXPIRED",
  );
});

test("rejects a cursor ahead of a restarted event stream", () => {
  const events = new EventStore();
  events.append(FAKE_SESSION_ID, "session.updated", {state: "idle"});
  assert.throws(
    () => events.after(FAKE_SESSION_ID, 20, 2),
    (error: unknown) => error instanceof Error
      && "code" in error
      && error.code === "EVENT_CURSOR_AHEAD",
  );
});

test("rejects an event line that exceeds the protocol bound", () => {
  const events = new EventStore();
  assert.throws(
    () => events.append(FAKE_SESSION_ID, "error", {
      message: "x".repeat(1_000),
    }),
    (error: unknown) => error instanceof Error
      && "code" in error
      && error.code === "EVENT_TOO_LARGE",
  );
});

test("summary logging does not expose prompt content", async () => {
  const messages: string[] = [];
  const bridge = await startBridge({logger: (message) => messages.push(message)});
  try {
    const response = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/text`,
      "private default prompt",
      {
        [COMMAND_ID_HEADER]: "cmd_test_safe_logging",
        "Content-Type": "text/plain",
      },
    );
    assert.equal(response.status, 202);
    assert.ok(messages.some((message) => message.includes("text capture accepted")));
    assert.ok(messages.every((message) => !message.includes("private default prompt")));
  } finally {
    await bridge.close();
  }
});

test("verbose logging shows the full text, protocol events, and audio chunks", async () => {
  const messages: string[] = [];
  const bridge = await startBridge({
    logger: (message) => messages.push(message),
    verbose: true,
  });
  try {
    const textResponse = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/text`,
      "full verbose prompt",
      {
        [COMMAND_ID_HEADER]: "cmd_test_verbose_logging",
        "Content-Type": "text/plain",
      },
    );
    assert.equal(textResponse.status, 202);
    await waitForEvent(bridge, "turn.completed");

    const pcm = Buffer.from([0, 0, 16, 0, 32, 0, 48, 0]);
    const audioResponse = await request(
      bridge,
      "POST",
      `/v1/sessions/${FAKE_SESSION_ID}/captures/audio`,
      pcm,
      {
        [COMMAND_ID_HEADER]: "cmd_test_verbose_audio",
        "Content-Type": "application/x-3gent-pcm; "
          + "format=s16le; rate=16364; channels=1",
      },
    );
    assert.equal(audioResponse.status, 202);

    assert.ok(messages.some((message) => message.includes(
      "command=cmd_test_verbose_logging",
    )));
    assert.ok(messages.some((message) => message.includes(
      '3DS -> bridge text "full verbose prompt"',
    )));
    assert.ok(messages.some((message) => message.includes(
      "bridge -> 3DS 202",
    )));
    assert.ok(messages.some((message) => message.includes(
      '"type":"assistant.text.delta"',
    )));
    assert.ok(messages.some((message) => message.includes(
      "3DS -> bridge audio chunk bytes=8 total=8",
    )));
    assert.ok(messages.every((message) => !message.includes("events=0")));
  } finally {
    await bridge.close();
  }
});

test("empty event polls require the explicit verbose-polls option", async () => {
  const messages: string[] = [];
  const bridge = await startBridge({
    logger: (message) => messages.push(message),
    verbosePolls: true,
  });
  try {
    const initial = await poll(bridge, 0);
    const cursor = initial.at(-1)?.sequence ?? 0;
    messages.length = 0;

    const empty = await poll(bridge, cursor);
    assert.deepEqual(empty, []);
    assert.ok(messages.some((message) => message.includes(
      "3DS -> bridge GET /v1/events",
    )));
    assert.ok(messages.some((message) => message.includes(
      "bridge -> 3DS 200 events=0 bytes=0",
    )));
  } finally {
    await bridge.close();
  }
});
