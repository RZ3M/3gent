import { createHash, randomUUID } from "node:crypto";

import type {
  AgentAdapter,
  ApprovalChoice,
  CaptureAttachment,
  StartSessionOptions,
} from "./agent-adapter.js";
import {
  CodexAppServerClient,
  type CodexAppServerClientOptions,
  type JsonRpcNotification,
  type JsonRpcServerRequest,
  type JsonRpcServerRequestResponder,
} from "./codex-app-server-client.js";
import { EventStore } from "./event-store.js";
import {
  ProtocolError,
  type SessionState,
  type SessionSummary,
} from "./protocol.js";

interface RpcClient {
  start(): Promise<unknown>;
  request<TResult = unknown>(method: string, params: unknown): Promise<TResult>;
  close(): Promise<void>;
}

interface RpcCallbacks {
  onNotification(notification: JsonRpcNotification): void | Promise<void>;
  onServerRequest(
    request: JsonRpcServerRequest,
    responder: JsonRpcServerRequestResponder,
  ): void | Promise<void>;
  onStderr(text: string): void;
  onProtocolError(error: Error): void;
}

export interface CodexAgentAdapterOptions {
  events: EventStore;
  executable?: string;
  arguments?: readonly string[];
  cwd?: string;
  logger?: (message: string) => void;
  maximumSessions?: number;
  clientFactory?: (callbacks: RpcCallbacks) => RpcClient;
}

interface CodexThreadRecord {
  threadId: string;
  cwd: string;
  loaded: boolean;
  session: SessionSummary;
}

interface PendingApproval {
  sessionId: string;
  turnId: string;
  kind: "command" | "file_change";
  responder: JsonRpcServerRequestResponder;
  timer: NodeJS.Timeout;
}

const DEFAULT_MAXIMUM_SESSIONS = 64;
const APPROVAL_TIMEOUT_MS = 5 * 60_000;
const MAX_TEXT_CHUNK_BYTES = 280;

export class CodexAgentAdapter implements AgentAdapter {
  public readonly id = "codex";
  readonly #events: EventStore;
  readonly #client: RpcClient;
  readonly #logger: (message: string) => void;
  readonly #maximumSessions: number;
  readonly #records = new Map<string, CodexThreadRecord>();
  readonly #sessionIdByThreadId = new Map<string, string>();
  readonly #pendingApprovals = new Map<string, PendingApproval>();
  readonly #startedTurns = new Set<string>();
  readonly #terminalTurns = new Set<string>();
  #closed = false;

  private constructor(options: CodexAgentAdapterOptions) {
    this.#events = options.events;
    this.#logger = options.logger ?? console.log;
    this.#maximumSessions = options.maximumSessions ?? DEFAULT_MAXIMUM_SESSIONS;
    if (!Number.isInteger(this.#maximumSessions) || this.#maximumSessions <= 0) {
      throw new RangeError("maximumSessions must be a positive integer");
    }
    const callbacks: RpcCallbacks = {
      onNotification: async (notification) => await this.#onNotification(notification),
      onServerRequest: async (request, responder) => {
        await this.#onServerRequest(request, responder);
      },
      onStderr: (text) => this.#logger(`codex: ${text.trimEnd()}`),
      onProtocolError: (error) => this.#logger(`codex protocol warning: ${error.message}`),
    };
    this.#client = options.clientFactory?.(callbacks) ?? new CodexAppServerClient({
      ...(options.executable === undefined ? {} : {executable: options.executable}),
      ...(options.arguments === undefined ? {} : {arguments: options.arguments}),
      ...(options.cwd === undefined ? {} : {cwd: options.cwd}),
      clientInfo: {name: "3gent-bridge", title: "3gent Bridge", version: "0.2.0"},
      maximumLineBytes: 1024 * 1024,
      ...callbacks,
    } satisfies CodexAppServerClientOptions);
  }

  public static async create(
    options: CodexAgentAdapterOptions,
  ): Promise<CodexAgentAdapter> {
    const adapter = new CodexAgentAdapter(options);
    await adapter.#client.start();
    await adapter.#loadRecentThreads();
    return adapter;
  }

  public listSessions(): readonly SessionSummary[] {
    return [...this.#records.values()]
      .map((record) => this.#snapshot(record));
  }

  public session(sessionId: string): SessionSummary | undefined {
    const record = this.#records.get(sessionId);
    return record === undefined ? undefined : this.#snapshot(record);
  }

  public async startSession(options: StartSessionOptions): Promise<SessionSummary> {
    this.#ensureOpen();
    if (options.cwd.trim().length === 0) {
      throw new ProtocolError(400, "INVALID_CWD", "working directory is required");
    }
    const response = await this.#client.request("thread/start", {
      cwd: options.cwd,
    });
    const thread = requireThreadResponse(response, "thread/start");
    const record = this.#registerThread(thread, true);
    this.#emitConnectionReady(record);
    this.#emitSessionUpdated(record);
    return this.#snapshot(record);
  }

  public async resumeSession(sessionId: string): Promise<SessionSummary> {
    this.#ensureOpen();
    const record = this.#requireRecord(sessionId);
    const response = await this.#client.request("thread/resume", {
      threadId: record.threadId,
    });
    const thread = requireThreadResponse(response, "thread/resume");
    const resumed = this.#registerThread(thread, true);
    this.#emitSessionUpdated(resumed);
    return this.#snapshot(resumed);
  }

  public async submitText(
    sessionId: string,
    text: string,
    attachments: readonly CaptureAttachment[] = [],
  ): Promise<void> {
    this.#ensureOpen();
    const record = await this.#ensureLoaded(sessionId);
    if (record.session.activeTurnId !== null) {
      throw new ProtocolError(409, "SESSION_BUSY", "session already has an active turn", true);
    }
    const captureId = `cap_${randomUUID()}`;
    this.#events.append(sessionId, "capture.accepted", {
      captureId,
      kind: "text",
      bytes: Buffer.byteLength(text, "utf8"),
    });
    const response = await this.#client.request("turn/start", {
      threadId: record.threadId,
      input: [
        {type: "text", text, text_elements: []},
        ...attachments.map((attachment) => ({
          type: "localImage",
          path: attachment.path,
          detail: "auto",
        })),
      ],
    });
    const turn = requireNestedRecord(response, "turn", "turn/start");
    const turnId = requireString(turn, "id", "turn/start turn");
    this.#markTurnStarted(record, turnId, captureId);
  }

  public async interrupt(sessionId: string): Promise<void> {
    this.#ensureOpen();
    const record = this.#requireRecord(sessionId);
    const turnId = record.session.activeTurnId;
    if (turnId === null) {
      throw new ProtocolError(409, "NO_ACTIVE_TURN", "no turn is active");
    }
    await this.#client.request("turn/interrupt", {
      threadId: record.threadId,
      turnId,
    });
  }

  public async respondToApproval(
    sessionId: string,
    approvalId: string,
    choice: ApprovalChoice,
  ): Promise<void> {
    this.#ensureOpen();
    const pending = this.#pendingApprovals.get(approvalId);
    if (pending === undefined || pending.sessionId !== sessionId) {
      throw new ProtocolError(409, "APPROVAL_NOT_PENDING", "approval is no longer pending");
    }
    clearTimeout(pending.timer);
    this.#pendingApprovals.delete(approvalId);
    pending.responder.respond({decision: choice === "approve_once" ? "accept" : choice});
    this.#events.append(sessionId, "approval.resolved", {approvalId, choice});
    const record = this.#requireRecord(sessionId);
    record.session.pendingApprovalId = null;
    record.session.state = record.session.activeTurnId === null ? "idle" : "working";
    this.#emitSessionUpdated(record);
  }

  public async shutdown(): Promise<void> {
    if (this.#closed) {
      return;
    }
    this.#closed = true;
    for (const [approvalId, pending] of this.#pendingApprovals) {
      clearTimeout(pending.timer);
      pending.responder.respond({decision: "cancel"});
      this.#events.append(pending.sessionId, "approval.resolved", {
        approvalId,
        choice: "cancel",
        reason: "bridge_shutdown",
      });
    }
    this.#pendingApprovals.clear();
    await this.#client.close();
  }

  async #loadRecentThreads(): Promise<void> {
    let cursor: string | null = null;
    while (this.#records.size < this.#maximumSessions) {
      const response: unknown = await this.#client.request("thread/list", {
        cursor,
        limit: Math.min(32, this.#maximumSessions - this.#records.size),
        sortKey: "updated_at",
        sortDirection: "desc",
      });
      if (!isRecord(response) || !Array.isArray(response.data)) {
        throw new Error("Codex thread/list returned an invalid response");
      }
      for (const value of response.data) {
        if (isRecord(value) && typeof value.id === "string") {
          this.#registerThread(value, false);
        }
      }
      cursor = typeof response.nextCursor === "string" ? response.nextCursor : null;
      if (cursor === null || response.data.length === 0) {
        break;
      }
    }
  }

  async #ensureLoaded(sessionId: string): Promise<CodexThreadRecord> {
    const record = this.#requireRecord(sessionId);
    if (!record.loaded) {
      await this.resumeSession(sessionId);
    }
    return this.#requireRecord(sessionId);
  }

  #registerThread(thread: Record<string, unknown>, loaded: boolean): CodexThreadRecord {
    const threadId = requireString(thread, "id", "Codex thread");
    const sessionId = codexSessionId(threadId);
    const existing = this.#records.get(sessionId);
    const status = threadStatusToSessionState(thread.status);
    const record: CodexThreadRecord = existing ?? {
      threadId,
      cwd: "",
      loaded,
      session: {
        sessionId,
        label: "Codex task",
        adapter: this.id,
        state: status,
        activeTurnId: null,
        pendingApprovalId: null,
        lastSequence: 0,
      },
    };
    record.loaded ||= loaded;
    record.cwd = typeof thread.cwd === "string" ? thread.cwd : record.cwd;
    record.session.label = boundedLabel(thread.name, thread.preview, record.cwd);
    if (record.session.activeTurnId === null && record.session.pendingApprovalId === null) {
      record.session.state = status;
    }
    this.#records.set(sessionId, record);
    this.#sessionIdByThreadId.set(threadId, sessionId);
    return record;
  }

  #recordForThreadId(threadId: string): CodexThreadRecord {
    const existingId = this.#sessionIdByThreadId.get(threadId);
    if (existingId !== undefined) {
      return this.#requireRecord(existingId);
    }
    return this.#registerThread({id: threadId, preview: "Codex task", status: {type: "active"}}, true);
  }

  async #onNotification(notification: JsonRpcNotification): Promise<void> {
    const params = notification.params;
    if (!isRecord(params) || typeof params.threadId !== "string") {
      return;
    }
    const record = this.#recordForThreadId(params.threadId);
    if (notification.method === "thread/status/changed") {
      if (record.session.pendingApprovalId === null) {
        record.session.state = threadStatusToSessionState(params.status);
      }
      this.#emitSessionUpdated(record);
      return;
    }
    if (notification.method === "turn/started" && isRecord(params.turn)) {
      const turnId = requireString(params.turn, "id", "turn/started");
      this.#markTurnStarted(record, turnId, null);
      return;
    }
    if (notification.method === "item/agentMessage/delta"
      && typeof params.turnId === "string" && typeof params.delta === "string") {
      for (const text of splitUtf8(params.delta, MAX_TEXT_CHUNK_BYTES)) {
        this.#events.append(record.session.sessionId, "assistant.text.delta", {
          turnId: params.turnId,
          text,
        });
      }
      return;
    }
    if (notification.method === "turn/diff/updated"
      && typeof params.turnId === "string" && typeof params.diff === "string") {
      this.#events.append(record.session.sessionId, "turn.diff.updated", {
        turnId: params.turnId,
        ...summarizeDiff(params.diff),
      });
      return;
    }
    if (notification.method === "turn/completed" && isRecord(params.turn)) {
      this.#completeTurn(record, params.turn);
      return;
    }
    if (notification.method === "error") {
      const message = isRecord(params.error) && typeof params.error.message === "string"
        ? params.error.message
        : "Codex reported an error";
      this.#events.append(record.session.sessionId, "error", {
        turnId: typeof params.turnId === "string" ? params.turnId : null,
        message: truncateUtf8(message, 300),
        retrying: params.willRetry === true,
      });
      if (params.willRetry !== true) {
        record.session.state = "failed";
        this.#emitSessionUpdated(record);
      }
    }
  }

  async #onServerRequest(
    request: JsonRpcServerRequest,
    responder: JsonRpcServerRequestResponder,
  ): Promise<void> {
    const params = request.params;
    if (!isRecord(params) || typeof params.threadId !== "string"
      || typeof params.turnId !== "string") {
      responder.respondError({code: -32602, message: "Invalid Codex approval request"});
      return;
    }
    if (request.method === "item/permissions/requestApproval") {
      responder.respondError({
        code: -32001,
        message: "3gent does not grant extended permission profiles",
      });
      return;
    }
    const kind = request.method === "item/commandExecution/requestApproval"
      ? "command"
      : request.method === "item/fileChange/requestApproval"
        ? "file_change"
        : null;
    if (kind === null) {
      responder.respondError({code: -32601, message: `Unsupported Codex request: ${request.method}`});
      return;
    }
    const record = this.#recordForThreadId(params.threadId);
    if (record.session.pendingApprovalId !== null) {
      responder.respond({decision: "cancel"});
      this.#events.append(record.session.sessionId, "error", {
        turnId: params.turnId,
        message: "Codex requested another approval while one was already pending",
        retrying: false,
      });
      return;
    }
    const approvalId = `apr_${randomUUID()}`;
    const summary = approvalSummary(kind, params);
    const expiresAt = new Date(Date.now() + APPROVAL_TIMEOUT_MS).toISOString();
    const timer = setTimeout(() => {
      const pending = this.#pendingApprovals.get(approvalId);
      if (pending === undefined) {
        return;
      }
      this.#pendingApprovals.delete(approvalId);
      pending.responder.respond({decision: "cancel"});
      this.#events.append(record.session.sessionId, "approval.resolved", {
        approvalId,
        choice: "cancel",
        reason: "expired",
      });
      record.session.pendingApprovalId = null;
      record.session.state = record.session.activeTurnId === null ? "idle" : "working";
      this.#emitSessionUpdated(record);
    }, APPROVAL_TIMEOUT_MS);
    timer.unref();
    this.#pendingApprovals.set(approvalId, {
      sessionId: record.session.sessionId,
      turnId: params.turnId,
      kind,
      responder,
      timer,
    });
    record.session.pendingApprovalId = approvalId;
    record.session.state = "waiting_for_user";
    this.#events.append(record.session.sessionId, "approval.requested", {
      approvalId,
      turnId: params.turnId,
      kind,
      summary,
      choices: ["approve_once", "decline", "cancel"],
      expiresAt,
    });
    this.#emitSessionUpdated(record);
  }

  #markTurnStarted(
    record: CodexThreadRecord,
    turnId: string,
    captureId: string | null,
  ): void {
    if (this.#terminalTurns.has(turnId)) {
      return;
    }
    record.session.activeTurnId = turnId;
    record.session.state = "working";
    if (!this.#startedTurns.has(turnId)) {
      this.#startedTurns.add(turnId);
      this.#events.append(record.session.sessionId, "turn.started", {
        turnId,
        ...(captureId === null ? {} : {captureId}),
      });
    }
    this.#emitSessionUpdated(record);
  }

  #completeTurn(record: CodexThreadRecord, turn: Record<string, unknown>): void {
    const turnId = requireString(turn, "id", "turn/completed");
    const status = typeof turn.status === "string" ? turn.status : "failed";
    for (const [approvalId, pending] of this.#pendingApprovals) {
      if (pending.sessionId === record.session.sessionId && pending.turnId === turnId) {
        clearTimeout(pending.timer);
        pending.responder.respond({decision: "cancel"});
        this.#pendingApprovals.delete(approvalId);
        this.#events.append(record.session.sessionId, "approval.resolved", {
          approvalId,
          choice: "cancel",
          reason: "turn_completed",
        });
      }
    }
    this.#events.append(record.session.sessionId, "assistant.text.completed", {turnId});
    if (status === "interrupted") {
      this.#events.append(record.session.sessionId, "turn.interrupted", {turnId});
    }
    this.#events.append(record.session.sessionId, "turn.completed", {
      turnId,
      outcome: status,
      ...(isRecord(turn.error) && typeof turn.error.message === "string"
        ? {error: truncateUtf8(turn.error.message, 240)}
        : {}),
    });
    record.session.activeTurnId = null;
    record.session.pendingApprovalId = null;
    record.session.state = status === "failed" ? "failed" : "idle";
    this.#startedTurns.delete(turnId);
    this.#terminalTurns.add(turnId);
    while (this.#terminalTurns.size > 256) {
      const oldest = this.#terminalTurns.values().next().value as string | undefined;
      if (oldest === undefined) {
        break;
      }
      this.#terminalTurns.delete(oldest);
    }
    this.#emitSessionUpdated(record);
  }

  #emitConnectionReady(record: CodexThreadRecord): void {
    this.#events.append(record.session.sessionId, "connection.ready", {
      bridge: "3gent",
      adapter: this.id,
    });
  }

  #emitSessionUpdated(record: CodexThreadRecord): void {
    this.#events.append(record.session.sessionId, "session.updated", {
      state: record.session.state,
      activeTurnId: record.session.activeTurnId,
      pendingApprovalId: record.session.pendingApprovalId,
    });
  }

  #snapshot(record: CodexThreadRecord): SessionSummary {
    return {
      ...record.session,
      lastSequence: this.#events.latestSequence(record.session.sessionId),
    };
  }

  #requireRecord(sessionId: string): CodexThreadRecord {
    const record = this.#records.get(sessionId);
    if (record === undefined) {
      throw new ProtocolError(404, "SESSION_NOT_FOUND", "session does not exist");
    }
    return record;
  }

  #ensureOpen(): void {
    if (this.#closed) {
      throw new ProtocolError(503, "ADAPTER_OFFLINE", "Codex adapter is closed", true);
    }
  }
}

export function codexSessionId(threadId: string): string {
  const digest = createHash("sha256").update(threadId).digest("base64url").slice(0, 24);
  return `ses_codex_${digest}`;
}

function threadStatusToSessionState(value: unknown): SessionState {
  if (!isRecord(value) || typeof value.type !== "string") {
    return "offline";
  }
  if (value.type === "active") {
    if (Array.isArray(value.activeFlags)
      && value.activeFlags.some((flag) => flag === "waitingOnApproval" || flag === "waitingOnUserInput")) {
      return "waiting_for_user";
    }
    return "working";
  }
  if (value.type === "systemError") {
    return "failed";
  }
  return "idle";
}

function boundedLabel(name: unknown, preview: unknown, cwd: string): string {
  const candidate = typeof name === "string" && name.trim().length > 0
    ? name.trim()
    : typeof preview === "string" && preview.trim().length > 0
      ? preview.trim()
      : cwd.split("/").filter(Boolean).at(-1) ?? "Codex task";
  return truncateUtf8(candidate.replaceAll(/\s+/g, " "), 96);
}

function approvalSummary(
  kind: "command" | "file_change",
  params: Record<string, unknown>,
): string {
  if (kind === "command") {
    const command = typeof params.command === "string" ? params.command : "Run a command";
    const cwd = typeof params.cwd === "string" ? ` in ${params.cwd}` : "";
    return truncateUtf8(`${command}${cwd}`, 160);
  }
  const reason = typeof params.reason === "string" ? params.reason : "Apply file changes";
  const root = typeof params.grantRoot === "string" ? ` under ${params.grantRoot}` : "";
  return truncateUtf8(`${reason}${root}`, 160);
}

function summarizeDiff(diff: string): {files: number; additions: number; deletions: number} {
  let files = 0;
  let additions = 0;
  let deletions = 0;
  for (const line of diff.split("\n")) {
    if (line.startsWith("diff --git ")) {
      files += 1;
    } else if (line.startsWith("+") && !line.startsWith("+++")) {
      additions += 1;
    } else if (line.startsWith("-") && !line.startsWith("---")) {
      deletions += 1;
    }
  }
  return {files, additions, deletions};
}

function splitUtf8(text: string, maximumBytes: number): string[] {
  if (text.length === 0) {
    return [];
  }
  const pieces: string[] = [];
  let current = "";
  let bytes = 0;
  for (const character of text) {
    const size = Buffer.byteLength(character, "utf8");
    if (bytes + size > maximumBytes && current.length > 0) {
      pieces.push(current);
      current = "";
      bytes = 0;
    }
    current += character;
    bytes += size;
  }
  if (current.length > 0) {
    pieces.push(current);
  }
  return pieces;
}

function truncateUtf8(text: string, maximumBytes: number): string {
  if (Buffer.byteLength(text, "utf8") <= maximumBytes) {
    return text;
  }
  const suffix = "…";
  return `${splitUtf8(text, maximumBytes - Buffer.byteLength(suffix))[0] ?? ""}${suffix}`;
}

function requireThreadResponse(value: unknown, method: string): Record<string, unknown> {
  return requireNestedRecord(value, "thread", method);
}

function requireNestedRecord(
  value: unknown,
  key: string,
  context: string,
): Record<string, unknown> {
  if (!isRecord(value) || !isRecord(value[key])) {
    throw new Error(`Codex ${context} returned an invalid ${key}`);
  }
  return value[key];
}

function requireString(
  value: Record<string, unknown>,
  key: string,
  context: string,
): string {
  const field = value[key];
  if (typeof field !== "string" || field.length === 0) {
    throw new Error(`${context} has no ${key}`);
  }
  return field;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
