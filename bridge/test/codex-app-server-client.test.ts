import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import test from "node:test";

import {
  CodexAppServerClient,
  CodexAppServerClientError,
  type JsonRpcNotification,
} from "../src/codex-app-server-client.js";

const FAKE_APP_SERVER = String.raw`
let buffered = "";
function write(message, split) {
  const encoded = JSON.stringify(message) + "\n";
  if (split) {
    const midpoint = Math.max(1, Math.floor(encoded.length / 2));
    process.stdout.write(encoded.slice(0, midpoint));
    setTimeout(() => process.stdout.write(encoded.slice(midpoint)), 1);
  } else {
    process.stdout.write(encoded);
  }
}
function receive(message) {
  if (message.method === "initialize") {
    write({id: message.id, result: {
      userAgent: "fake-codex",
      codexHome: "/fake/.codex",
      platformFamily: "unix",
      platformOs: "test"
    }}, true);
    return;
  }
  if (message.method === "echo") {
    setTimeout(
      () => write({id: message.id, result: message.params}, message.params.split === true),
      message.params.delay || 0,
    );
    return;
  }
  if (message.method === "notify-and-request") {
    write({method: "item/agentMessage/delta", params: {delta: "hello"}});
    write({id: "approval-1", method: "item/commandExecution/requestApproval", params: {command: "npm test"}});
    write({id: message.id, result: {sent: true}});
    return;
  }
  if (message.method === "never") return;
  if (message.method === "exit") {
    setTimeout(() => process.exit(23), 5);
    return;
  }
  if (message.method === "oversized") {
    process.stdout.write("x".repeat(300) + "\n");
    return;
  }
  if (message.id === "approval-1") {
    write({method: "approval-response-seen", params: message.result});
    return;
  }
  write({id: message.id, error: {code: -32601, message: "unknown method"}});
}
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buffered += chunk;
  for (;;) {
    const newline = buffered.indexOf("\n");
    if (newline < 0) return;
    const line = buffered.slice(0, newline);
    buffered = buffered.slice(newline + 1);
    if (line.length > 0) receive(JSON.parse(line));
  }
});
`;

function client(options: ConstructorParameters<typeof CodexAppServerClient>[0] = {}): CodexAppServerClient {
  return new CodexAppServerClient({
    executable: process.execPath,
    arguments: ["-e", FAKE_APP_SERVER],
    requestTimeoutMs: 100,
    ...options,
  });
}

async function waitFor<T>(predicate: () => T | undefined, message: string): Promise<T> {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const value = predicate();
    if (value !== undefined) {
      return value;
    }
    await delay(2);
  }
  throw new Error(message);
}

test("initializes and correlates concurrent split-line JSON-RPC responses", async () => {
  const appServer = client();
  try {
    const initialized = await appServer.start();
    assert.equal(initialized.userAgent, "fake-codex");
    assert.equal(appServer.state, "ready");

    const slow = appServer.request<{name: string}>("echo", {name: "slow", delay: 15, split: true});
    const fast = appServer.request<{name: string}>("echo", {name: "fast", delay: 1, split: false});
    assert.deepEqual(await fast, {name: "fast", delay: 1, split: false});
    assert.deepEqual(await slow, {name: "slow", delay: 15, split: true});
  } finally {
    await appServer.close();
  }
});

test("dispatches notifications and lets the bridge answer server requests", async () => {
  const notifications: JsonRpcNotification[] = [];
  const appServer = client({
    onNotification: (notification) => {
      notifications.push(notification);
    },
    onServerRequest: (request, responder) => {
      assert.equal(request.method, "item/commandExecution/requestApproval");
      assert.deepEqual(request.params, {command: "npm test"});
      responder.respond({decision: "decline"});
    },
  });
  try {
    await appServer.start();
    assert.deepEqual(await appServer.request("notify-and-request", {}), {sent: true});
    await waitFor(
      () => notifications.find((entry) => entry.method === "approval-response-seen"),
      "fake server did not receive the server-request response",
    );
    assert.deepEqual(
      notifications.find((entry) => entry.method === "item/agentMessage/delta"),
      {method: "item/agentMessage/delta", params: {delta: "hello"}},
    );
    assert.deepEqual(
      notifications.find((entry) => entry.method === "approval-response-seen"),
      {method: "approval-response-seen", params: {decision: "decline"}},
    );
  } finally {
    await appServer.close();
  }
});

test("times out requests and rejects pending work when the child exits", async () => {
  const appServer = client({requestTimeoutMs: 500});
  try {
    await appServer.start();
    await assert.rejects(
      appServer.request("never", {}, 30),
      (error: unknown) => error instanceof CodexAppServerClientError
        && error.message.includes("timed out"),
    );

    const pending = appServer.request("exit", {}, 500);
    await assert.rejects(
      pending,
      (error: unknown) => error instanceof CodexAppServerClientError
        && error.message.includes("exited"),
    );
    assert.equal(appServer.state, "failed");
  } finally {
    await appServer.close();
  }
});

test("rejects an oversized inbound JSON-RPC line without retaining it", async () => {
  const appServer = client({maximumLineBytes: 128});
  try {
    await appServer.start();
    await assert.rejects(
      appServer.request("oversized", {}, 500),
      (error: unknown) => error instanceof CodexAppServerClientError
        && error.message.includes("exceeds 128 bytes"),
    );
    assert.equal(appServer.state, "failed");
  } finally {
    await appServer.close();
  }
});

test("fails initialization cleanly when the executable cannot be spawned", async () => {
  const appServer = new CodexAppServerClient({
    executable: "/definitely/not/a/codex-binary",
    requestTimeoutMs: 100,
  });
  try {
    await assert.rejects(appServer.start(), CodexAppServerClientError);
    assert.equal(appServer.state, "failed");
  } finally {
    await appServer.close();
  }
});
