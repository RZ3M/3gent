import assert from "node:assert/strict";
import test from "node:test";

import {
  CodexAgentAdapter,
  codexSessionId,
  type CodexAgentAdapterOptions,
} from "../src/codex-agent-adapter.js";
import type {
  JsonRpcNotification,
  JsonRpcServerRequest,
  JsonRpcServerRequestResponder,
} from "../src/codex-app-server-client.js";
import { EventStore } from "../src/event-store.js";
import { encodeEventBatch } from "../src/protocol.js";

class FakeRpcClient {
  public readonly requests: Array<{method: string; params: unknown}> = [];
  public closed = false;
  public callbacks!: Parameters<NonNullable<CodexAgentAdapterOptions["clientFactory"]>>[0];
  public nextThreadId = "019-thread-existing";
  public nextTurnId = "019-turn-one";
  public turnStartHandler: (() => Promise<unknown>) | null = null;

  public async start(): Promise<unknown> {
    return {};
  }

  public async request<TResult>(method: string, params: unknown): Promise<TResult> {
    this.requests.push({method, params});
    if (method === "thread/list") {
      return {
        data: [thread(this.nextThreadId, "Existing task", "notLoaded")],
        nextCursor: null,
      } as TResult;
    }
    if (method === "thread/start") {
      this.nextThreadId = "019-thread-new";
      return {thread: thread(this.nextThreadId, "New task", "idle")} as TResult;
    }
    if (method === "thread/resume") {
      const requested = (params as {threadId: string}).threadId;
      return {thread: thread(requested, "Resumed task", "idle")} as TResult;
    }
    if (method === "turn/start") {
      if (this.turnStartHandler !== null) {
        return await this.turnStartHandler() as TResult;
      }
      return {turn: {id: this.nextTurnId, status: "inProgress"}} as TResult;
    }
    if (method === "turn/interrupt") {
      return {} as TResult;
    }
    throw new Error(`unexpected request ${method}`);
  }

  public async close(): Promise<void> {
    this.closed = true;
  }

  public async notify(method: string, params: Record<string, unknown>): Promise<void> {
    await this.callbacks.onNotification({method, params} satisfies JsonRpcNotification);
  }

  public async serverRequest(
    method: string,
    params: Record<string, unknown>,
  ): Promise<{result?: unknown; error?: unknown}> {
    const answer: {result?: unknown; error?: unknown} = {};
    const responder: JsonRpcServerRequestResponder = {
      respond: (result) => { answer.result = result; },
      respondError: (error) => { answer.error = error; },
    };
    await this.callbacks.onServerRequest(
      {id: 91, method, params} satisfies JsonRpcServerRequest,
      responder,
    );
    return answer;
  }
}

async function fixture() {
  const events = new EventStore();
  const rpc = new FakeRpcClient();
  const adapter = await CodexAgentAdapter.create({
    events,
    logger: () => {},
    clientFactory: (callbacks) => {
      rpc.callbacks = callbacks;
      return rpc;
    },
  });
  return {adapter, events, rpc};
}

test("lists opaque Codex sessions and lazily resumes before starting a turn", async () => {
  const {adapter, events, rpc} = await fixture();
  const sessionId = codexSessionId(rpc.nextThreadId);
  assert.equal(adapter.listSessions()[0]?.sessionId, sessionId);
  assert.ok(!sessionId.includes(rpc.nextThreadId));

  await adapter.submitText(sessionId, "Fix the tests");
  assert.deepEqual(
    rpc.requests.map((request) => request.method),
    ["thread/list", "thread/resume", "turn/start"],
  );
  const turnStart = rpc.requests[2]?.params as Record<string, unknown>;
  assert.deepEqual(turnStart.input, [{type: "text", text: "Fix the tests", text_elements: []}]);
  assert.equal("approvalPolicy" in turnStart, false);
  assert.equal("approvalsReviewer" in turnStart, false);
  assert.equal(adapter.session(sessionId)?.activeTurnId, rpc.nextTurnId);
  assert.ok(events.after(sessionId, 0, 32).some((event) => event.type === "capture.accepted"));
  await adapter.shutdown();
});

test("translates response deltas, diffs, completion, and interrupt", async () => {
  const {adapter, events, rpc} = await fixture();
  const sessionId = codexSessionId(rpc.nextThreadId);
  await adapter.submitText(sessionId, "Work");
  await rpc.notify("item/agentMessage/delta", {
    threadId: rpc.nextThreadId,
    turnId: rpc.nextTurnId,
    itemId: "item-1",
    delta: "🙂".repeat(200),
  });
  await rpc.notify("turn/diff/updated", {
    threadId: rpc.nextThreadId,
    turnId: rpc.nextTurnId,
    diff: "diff --git a/a b/a\n--- a/a\n+++ b/a\n+one\n-two\n",
  });
  await adapter.interrupt(sessionId);
  assert.equal(rpc.requests.at(-1)?.method, "turn/interrupt");
  await rpc.notify("turn/completed", {
    threadId: rpc.nextThreadId,
    turn: {id: rpc.nextTurnId, status: "interrupted", error: null},
  });

  const translated = events.after(sessionId, 0, 64);
  const deltas = translated.filter((event) => event.type === "assistant.text.delta");
  assert.ok(deltas.length > 1);
  assert.equal(deltas.map((event) => event.payload.text).join(""), "🙂".repeat(200));
  assert.deepEqual(
    translated.find((event) => event.type === "turn.diff.updated")?.payload,
    {turnId: rpc.nextTurnId, files: 1, additions: 1, deletions: 1},
  );
  assert.equal(adapter.session(sessionId)?.state, "idle");
  assert.ok(translated.some((event) => event.type === "turn.interrupted"));
  await adapter.shutdown();
});

test("does not reactivate a turn whose completion notification beat the start response", async () => {
  const {adapter, rpc} = await fixture();
  const sessionId = codexSessionId(rpc.nextThreadId);
  let release!: (value: unknown) => void;
  rpc.turnStartHandler = async () => await new Promise<unknown>((resolve) => {
    release = resolve;
  });
  const submission = adapter.submitText(sessionId, "Fast turn");
  while (release === undefined) {
    await new Promise((resolve) => setImmediate(resolve));
  }
  await rpc.notify("turn/started", {
    threadId: rpc.nextThreadId,
    turn: {id: rpc.nextTurnId, status: "inProgress"},
  });
  await rpc.notify("turn/completed", {
    threadId: rpc.nextThreadId,
    turn: {id: rpc.nextTurnId, status: "completed", error: null},
  });
  release({turn: {id: rpc.nextTurnId, status: "completed"}});
  await submission;
  assert.equal(adapter.session(sessionId)?.activeTurnId, null);
  assert.equal(adapter.session(sessionId)?.state, "idle");
  await adapter.shutdown();
});

test("maps Codex waiting flags to waiting-for-user state", async () => {
  const {adapter, rpc} = await fixture();
  const sessionId = codexSessionId(rpc.nextThreadId);
  await rpc.notify("thread/status/changed", {
    threadId: rpc.nextThreadId,
    status: {type: "active", activeFlags: ["waitingOnUserInput"]},
  });
  assert.equal(adapter.session(sessionId)?.state, "waiting_for_user");
  await adapter.shutdown();
});

test("keeps maximum command approval events inside the protocol line bound", async () => {
  const {adapter, events, rpc} = await fixture();
  const sessionId = codexSessionId(rpc.nextThreadId);
  await rpc.serverRequest("item/commandExecution/requestApproval", {
    threadId: rpc.nextThreadId,
    turnId: "turn_" + "t".repeat(59),
    itemId: "item-command",
    command: "x".repeat(2_000),
    cwd: "/" + "w".repeat(500),
  });
  const retained = events.after(sessionId, 0, 32);
  assert.doesNotThrow(() => encodeEventBatch(retained));
  await adapter.shutdown();
});

test("routes one-shot command and file approvals and rejects permission grants", async () => {
  const {adapter, events, rpc} = await fixture();
  const sessionId = codexSessionId(rpc.nextThreadId);
  const commandAnswerPromise = rpc.serverRequest("item/commandExecution/requestApproval", {
    threadId: rpc.nextThreadId,
    turnId: "turn-command",
    itemId: "item-command",
    command: "npm test",
    cwd: "/repo",
  });
  await commandAnswerPromise;
  const requested = events.after(sessionId, 0, 32)
    .find((event) => event.type === "approval.requested");
  const approvalId = requested?.payload.approvalId;
  assert.equal(typeof approvalId, "string");
  await adapter.respondToApproval(sessionId, String(approvalId), "approve_once");
  const commandAnswer = await commandAnswerPromise;
  assert.deepEqual(commandAnswer.result, {decision: "accept"});

  const fileAnswerPromise = rpc.serverRequest("item/fileChange/requestApproval", {
    threadId: rpc.nextThreadId,
    turnId: "turn-file",
    itemId: "item-file",
    reason: "Write generated files",
  });
  await fileAnswerPromise;
  const latestApproval = events.after(sessionId, 0, 64)
    .filter((event) => event.type === "approval.requested").at(-1);
  await adapter.respondToApproval(
    sessionId,
    String(latestApproval?.payload.approvalId),
    "decline",
  );
  assert.deepEqual((await fileAnswerPromise).result, {decision: "decline"});

  const permission = await rpc.serverRequest("item/permissions/requestApproval", {
    threadId: rpc.nextThreadId,
    turnId: "turn-permission",
    itemId: "item-permission",
  });
  assert.ok(permission.error);
  await adapter.shutdown();
});

test("starts new tasks and shuts down its app-server child", async () => {
  const {adapter, rpc} = await fixture();
  const session = await adapter.startSession({cwd: "/repo"});
  assert.equal(session.sessionId, codexSessionId("019-thread-new"));
  assert.equal(rpc.requests.at(-1)?.method, "thread/start");
  await adapter.shutdown();
  assert.equal(rpc.closed, true);
});

test("maps photo captures to Codex localImage input", async () => {
  const {adapter, rpc} = await fixture();
  const sessionId = codexSessionId(rpc.nextThreadId);
  await adapter.submitText(sessionId, "Inspect the photo", [{
    kind: "photo",
    path: "/tmp/latest.bmp",
  }]);
  const turnStart = rpc.requests.find((request) => request.method === "turn/start");
  assert.deepEqual((turnStart?.params as {input: unknown[]}).input, [
    {type: "text", text: "Inspect the photo", text_elements: []},
    {type: "localImage", path: "/tmp/latest.bmp", detail: "auto"},
  ]);
  await adapter.shutdown();
});

function thread(id: string, preview: string, status: string): Record<string, unknown> {
  return {
    id,
    preview,
    name: null,
    cwd: "/repo",
    status: {type: status},
  };
}
