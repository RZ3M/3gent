import { randomUUID } from "node:crypto";
import { rm } from "node:fs/promises";
import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { resolve } from "node:path";

import type { AgentAdapter } from "./agent-adapter.js";
import { CommandRegistry } from "./command-registry.js";
import { EventStore } from "./event-store.js";
import { FakeAgentAdapter } from "./fake-agent-adapter.js";
import {
  encodeEventBatch,
  isIdentifier,
  MAX_AUDIO_BYTES,
  MAX_EVENT_POLL_LIMIT,
  MAX_TEXT_CAPTURE_BYTES,
  PROTOCOL_VERSION,
  ProtocolError,
  protocolErrorBody,
  requireCommandId,
  requireProtocolVersion,
  sendJson,
  type CommandAcknowledgement,
  type SessionSummary,
} from "./protocol.js";
import { savePcmRequestAsWav } from "./wav.js";
import type { Transcriber } from "./transcriber.js";
import {
  immutablePhotoPath,
  publishLatestPhoto,
  saveRgb565RequestAsBmp,
} from "./photo.js";

const DEFAULT_CAPTURE_PATH = resolve("data", "latest.wav");
const DEFAULT_PHOTO_PATH = resolve("data", "latest.bmp");
const SESSION_PATH = /^\/v1\/sessions\/([^/]+)$/;
const SESSION_RESUME_PATH = /^\/v1\/sessions\/([^/]+)\/resume$/;
const TEXT_CAPTURE_PATH = /^\/v1\/sessions\/([^/]+)\/captures\/text$/;
const AUDIO_CAPTURE_PATH = /^\/v1\/sessions\/([^/]+)\/captures\/audio$/;
const PHOTO_CAPTURE_PATH = /^\/v1\/sessions\/([^/]+)\/captures\/photo$/;
const INTERRUPT_PATH = /^\/v1\/sessions\/([^/]+)\/turns\/current\/interrupt$/;
const APPROVAL_PATH = /^\/v1\/sessions\/([^/]+)\/approvals\/([^/]+)\/respond$/;
const MAX_REVIEW_TRANSCRIPT_BYTES = 1600;

export interface BridgeServerOptions {
  adapter?: AgentAdapter;
  events?: EventStore;
  capturePath?: string;
  fakeDeltaIntervalMs?: number;
  logger?: (message: string) => void;
  verbose?: boolean;
  verbosePolls?: boolean;
  defaultCwd?: string;
  transcriber?: Transcriber;
  photoPath?: string;
}

export interface CommandExecution {
  acknowledgement: CommandAcknowledgement;
  duplicate: boolean;
}

export class BridgeApplication {
  public readonly events: EventStore;
  public readonly adapter: AgentAdapter;
  readonly #commands = new CommandRegistry();
  readonly #inFlightCommands = new Map<string, Promise<CommandAcknowledgement>>();
  readonly #sessionCommandResults = new Map<string, SessionSummary>();
  readonly #inFlightSessionCommands = new Map<string, Promise<SessionSummary>>();
  readonly #capturePath: string;
  readonly #logger: (message: string) => void;
  readonly #verbose: boolean;
  readonly #verbosePolls: boolean;
  readonly #defaultCwd: string;
  readonly #transcriber: Transcriber | undefined;
  readonly #photoPath: string;
  readonly #pendingPhotos = new Map<string, string>();
  #audioCaptureActive = false;
  #photoCaptureActive = false;

  public constructor(options: BridgeServerOptions = {}) {
    this.events = options.events ?? new EventStore();
    this.#capturePath = options.capturePath ?? DEFAULT_CAPTURE_PATH;
    this.#logger = options.logger ?? console.log;
    this.#verbosePolls = options.verbosePolls ?? false;
    this.#verbose = (options.verbose ?? false) || this.#verbosePolls;
    this.#defaultCwd = options.defaultCwd ?? process.cwd();
    this.#transcriber = options.transcriber;
    this.#photoPath = options.photoPath ?? DEFAULT_PHOTO_PATH;
    this.adapter = options.adapter ?? new FakeAgentAdapter(
      this.events,
      options.fakeDeltaIntervalMs ?? 80,
    );
  }

  public async handle(
    request: IncomingMessage,
    response: ServerResponse,
  ): Promise<void> {
    try {
      const url = new URL(request.url ?? "/", "http://bridge.local");
      const isEventPoll = url.pathname === "/v1/events"
        && request.method === "GET";
      if (!isEventPoll) {
        this.#logRequest(request, url);
      }
      if (url.pathname === "/health" && request.method === "GET") {
        this.#sendJson(response, 200, {
          protocolVersion: PROTOCOL_VERSION,
          status: "ready",
          bridge: "3gent-0.6-hwtest",
        });
        return;
      }

      if (!url.pathname.startsWith("/v1/")) {
        throw new ProtocolError(404, "NOT_FOUND", "route not found");
      }
      requireProtocolVersion(request.headers);

      if (url.pathname === "/v1/sessions" && request.method === "GET") {
        const limit = url.searchParams.has("limit")
          ? parseBoundedInteger(url.searchParams.get("limit"), "limit", 1, 16)
          : 8;
        const offset = url.searchParams.has("cursor")
          ? parseBoundedInteger(url.searchParams.get("cursor"), "cursor", 0, 10_000)
          : 0;
        const allSessions = this.adapter.listSessions();
        const sessions = allSessions.slice(offset, offset + limit);
        const nextCursor = offset + sessions.length < allSessions.length
          ? String(offset + sessions.length)
          : null;
        this.#sendJson(response, 200, {
          protocolVersion: PROTOCOL_VERSION,
          sessions,
          nextCursor,
        });
        return;
      }
      if (url.pathname === "/v1/sessions" && request.method === "POST") {
        const commandId = requireCommandId(request.headers);
        const body = await readJsonObject(request, 1024);
        const cwd = body.cwd ?? this.#defaultCwd;
        if (typeof cwd !== "string" || cwd.trim().length === 0 || Buffer.byteLength(cwd) > 768) {
          throw new ProtocolError(400, "INVALID_CWD", "cwd must be a non-empty bounded string");
        }
        const result = await this.#executeSessionCommand(
          commandId,
          async () => await this.adapter.startSession({cwd}),
        );
        this.#sendJson(response, result.duplicate ? 200 : 201, {
          protocolVersion: PROTOCOL_VERSION,
          commandId,
          duplicate: result.duplicate,
          session: result.session,
        });
        return;
      }
      if (url.pathname === "/v1/events" && request.method === "GET") {
        this.#sendEvents(request, url, response);
        return;
      }

      const sessionMatch = SESSION_PATH.exec(url.pathname);
      if (sessionMatch !== null && request.method === "GET") {
        const session = this.session(sessionMatch[1] ?? "");
        this.#sendJson(response, 200, {
          protocolVersion: PROTOCOL_VERSION,
          session,
        });
        return;
      }

      const resumeMatch = SESSION_RESUME_PATH.exec(url.pathname);
      if (resumeMatch !== null && request.method === "POST") {
        const sessionId = resumeMatch[1] ?? "";
        this.session(sessionId);
        await drainBody(request, 0);
        const commandId = requireCommandId(request.headers);
        const result = await this.#executeSessionCommand(
          commandId,
          async () => await this.adapter.resumeSession(sessionId),
        );
        this.#sendJson(response, result.duplicate ? 200 : 202, {
          protocolVersion: PROTOCOL_VERSION,
          commandId,
          duplicate: result.duplicate,
          session: result.session,
        });
        return;
      }

      const textMatch = TEXT_CAPTURE_PATH.exec(url.pathname);
      if (textMatch !== null && request.method === "POST") {
        const sessionId = textMatch[1] ?? "";
        this.session(sessionId);
        await this.#submitText(request, response, sessionId);
        return;
      }

      const audioMatch = AUDIO_CAPTURE_PATH.exec(url.pathname);
      if (audioMatch !== null && request.method === "POST") {
        const sessionId = audioMatch[1] ?? "";
        this.session(sessionId);
        await this.#submitAudio(request, response, sessionId);
        return;
      }

      const photoMatch = PHOTO_CAPTURE_PATH.exec(url.pathname);
      if (photoMatch !== null && request.method === "POST") {
        const sessionId = photoMatch[1] ?? "";
        this.session(sessionId);
        await this.#submitPhoto(request, response, sessionId);
        return;
      }

      const interruptMatch = INTERRUPT_PATH.exec(url.pathname);
      if (interruptMatch !== null && request.method === "POST") {
        const sessionId = interruptMatch[1] ?? "";
        this.session(sessionId);
        await drainBody(request, 0);
        const execution = await this.interruptCommand(
          sessionId,
          requireCommandId(request.headers),
        );
        this.#sendAcknowledgement(response, execution);
        this.#logger(
          `interrupt ${execution.duplicate ? "replayed" : "accepted"}`,
        );
        return;
      }

      const approvalMatch = APPROVAL_PATH.exec(url.pathname);
      if (approvalMatch !== null && request.method === "POST") {
        const sessionId = approvalMatch[1] ?? "";
        this.session(sessionId);
        const approvalId = approvalMatch[2];
        if (approvalId === undefined || !isIdentifier(approvalId, "apr")) {
          throw new ProtocolError(400, "INVALID_APPROVAL_ID", "invalid approval ID");
        }
        await this.#respondToApproval(request, response, sessionId, approvalId);
        return;
      }

      throw new ProtocolError(404, "NOT_FOUND", "route not found");
    } catch (error) {
      const protocolError = error instanceof ProtocolError
        ? error
        : new ProtocolError(500, "INTERNAL_ERROR", "bridge request failed");
      if (!(error instanceof ProtocolError)) {
        console.error(error);
      }
      if (!response.headersSent) {
        this.#sendJson(
          response,
          protocolError.status,
          protocolErrorBody(protocolError),
        );
      } else {
        response.destroy();
      }
    }
  }

  public async shutdown(): Promise<void> {
    await this.adapter.shutdown();
    await Promise.all(
      [...this.#pendingPhotos.values()].map(async (path) => await this.#removePhoto(path)),
    );
    this.#pendingPhotos.clear();
  }

  public session(sessionId: string) {
    const session = this.adapter.session(sessionId);
    if (session === undefined) {
      throw new ProtocolError(404, "SESSION_NOT_FOUND", "session does not exist");
    }
    return session;
  }

  public eventsAfter(sessionId: string, after: number, limit: number) {
    this.session(sessionId);
    return this.events.after(sessionId, after, limit);
  }

  public async submitTextCommand(
    sessionId: string,
    commandId: string,
    text: string,
  ): Promise<CommandExecution> {
    this.session(sessionId);
    if (Buffer.byteLength(text, "utf8") > MAX_TEXT_CAPTURE_BYTES) {
      throw new ProtocolError(413, "BODY_TOO_LARGE", "text capture exceeds its limit");
    }
    if (text.trim().length === 0) {
      throw new ProtocolError(400, "EMPTY_TEXT_CAPTURE", "text capture is empty");
    }
    return await this.#executeCommand(
      sessionId,
      commandId,
      async () => {
        const photoPath = this.#pendingPhotos.get(sessionId);
        await this.adapter.submitText(
          sessionId,
          text,
          photoPath === undefined ? [] : [{kind: "photo", path: photoPath}],
        );
        if (photoPath !== undefined) {
          if (this.#pendingPhotos.get(sessionId) === photoPath) {
            this.#pendingPhotos.delete(sessionId);
          }
          await this.#removePhoto(photoPath);
          this.events.append(sessionId, "capture.attached", {
            kind: "photo",
            filename: "latest.bmp",
          });
        }
      },
    );
  }

  public async interruptCommand(
    sessionId: string,
    commandId: string,
  ): Promise<CommandExecution> {
    this.session(sessionId);
    return await this.#executeCommand(
      sessionId,
      commandId,
      async () => await this.adapter.interrupt(sessionId),
    );
  }

  public async approvalCommand(
    sessionId: string,
    commandId: string,
    approvalId: string,
    choice: "approve_once" | "decline" | "cancel",
  ): Promise<CommandExecution> {
    this.session(sessionId);
    if (!isIdentifier(approvalId, "apr")) {
      throw new ProtocolError(400, "INVALID_APPROVAL_ID", "invalid approval ID");
    }
    return await this.#executeCommand(
      sessionId,
      commandId,
      async () => await this.adapter.respondToApproval(
        sessionId,
        approvalId,
        choice,
      ),
    );
  }

  #sendEvents(
    request: IncomingMessage,
    url: URL,
    response: ServerResponse,
  ): void {
    const sessionId = url.searchParams.get("sessionId");
    if (sessionId === null) {
      throw new ProtocolError(400, "INVALID_QUERY", "sessionId is required");
    }
    this.session(sessionId);
    const after = parseBoundedInteger(url.searchParams.get("after"), "after", 0, Number.MAX_SAFE_INTEGER);
    const limit = parseBoundedInteger(url.searchParams.get("limit"), "limit", 1, MAX_EVENT_POLL_LIMIT);
    const events = this.events.after(sessionId, after, limit);
    const encoded = encodeEventBatch(events);
    if (events.length > 0 || this.#verbosePolls) {
      this.#logRequest(request, url);
      this.#verboseLog(
        `bridge -> 3DS 200 events=${events.length} bytes=${encoded.byteLength}`,
      );
      for (const event of events) {
        this.#verboseLog(`bridge -> 3DS event ${JSON.stringify(event)}`);
      }
    }
    response.writeHead(200, {
      "Content-Type": "application/x-ndjson; charset=utf-8",
      "Content-Length": encoded.byteLength,
      "Cache-Control": "no-store",
      Connection: "keep-alive",
    });
    response.end(encoded);
  }

  async #submitText(
    request: IncomingMessage,
    response: ServerResponse,
    sessionId: string,
  ): Promise<void> {
    const contentType = request.headers["content-type"]?.toLowerCase() ?? "";
    if (!contentType.startsWith("text/plain")) {
      throw new ProtocolError(415, "INVALID_CONTENT_TYPE", "text/plain required");
    }
    const body = await drainBody(request, MAX_TEXT_CAPTURE_BYTES);
    const text = body.toString("utf8");
    this.#verboseLog(`3DS -> bridge text ${JSON.stringify(text)}`);
    const execution = await this.submitTextCommand(
      sessionId,
      requireCommandId(request.headers),
      text,
    );
    this.#sendAcknowledgement(response, execution);
    this.#logger(
      `text capture ${execution.duplicate ? "replayed" : "accepted"}: ${body.byteLength} bytes`,
    );
  }

  async #submitAudio(
    request: IncomingMessage,
    response: ServerResponse,
    sessionId: string,
  ): Promise<void> {
    const contentType = request.headers["content-type"]?.toLowerCase() ?? "";
    const validContentType = contentType.includes("application/x-3gent-pcm")
      && contentType.includes("format=s16le")
      && contentType.includes("rate=16364")
      && contentType.includes("channels=1");
    if (!validContentType) {
      throw new ProtocolError(
        415,
        "INVALID_AUDIO_FORMAT",
        "expected signed PCM16 mono at 16364 Hz",
      );
    }

    const commandId = requireCommandId(request.headers);
    const duplicate = this.#commands.get(commandId);
    if (duplicate !== undefined) {
      await discardBody(
        request,
        MAX_AUDIO_BYTES,
        this.#verbose
          ? (chunkBytes, totalBytes) => this.#verboseLog(
            `3DS -> bridge replayed audio chunk bytes=${chunkBytes} total=${totalBytes}`,
          )
          : undefined,
      );
      this.#sendJson(response, 200, duplicate);
      this.#logger("audio capture replayed");
      return;
    }
    if (this.#audioCaptureActive) {
      throw new ProtocolError(
        409,
        "AUDIO_CAPTURE_BUSY",
        "another audio capture is active",
        true,
      );
    }
    if (this.#transcriber === undefined) {
      throw new ProtocolError(
        503,
        "TRANSCRIPTION_NOT_CONFIGURED",
        "bridge transcription is not configured",
      );
    }

    this.#audioCaptureActive = true;
    try {
      const byteLength = await savePcmRequestAsWav(
        request,
        this.#capturePath,
        MAX_AUDIO_BYTES,
        this.#verbose
          ? (chunkBytes, totalBytes) => this.#verboseLog(
            `3DS -> bridge audio chunk bytes=${chunkBytes} total=${totalBytes}`,
          )
          : undefined,
      );
      const captureId = `cap_${randomUUID()}`;
      this.events.append(sessionId, "capture.accepted", {
        captureId,
        kind: "audio",
        bytes: byteLength,
        transcriber: this.#transcriber.id,
      });
      const transcript = await this.#transcriber.transcribe(this.#capturePath);
      if (Buffer.byteLength(transcript, "utf8") > MAX_REVIEW_TRANSCRIPT_BYTES) {
        throw new ProtocolError(
          413,
          "TRANSCRIPT_TOO_LARGE",
          `transcript exceeds the ${MAX_REVIEW_TRANSCRIPT_BYTES}-byte handheld review limit`,
        );
      }
      for (const text of splitUtf8(transcript, 280)) {
        this.events.append(sessionId, "capture.transcript.delta", {
          captureId,
          text,
        });
      }
      this.events.append(sessionId, "capture.transcribed", {
        captureId,
        bytes: Buffer.byteLength(transcript, "utf8"),
      });
      this.#sendNewAcknowledgement(response, commandId, sessionId);
      this.#logger(
        `audio capture transcribed: ${byteLength} PCM bytes, ${Buffer.byteLength(transcript, "utf8")} text bytes`,
      );
    } finally {
      this.#audioCaptureActive = false;
    }
  }

  async #respondToApproval(
    request: IncomingMessage,
    response: ServerResponse,
    sessionId: string,
    approvalId: string,
  ): Promise<void> {
    const body = await drainBody(request, 256);
    this.#verboseLog(
      `3DS -> bridge JSON ${JSON.stringify(body.toString("utf8"))}`,
    );
    let parsed: unknown;
    try {
      parsed = JSON.parse(body.toString("utf8"));
    } catch {
      throw new ProtocolError(400, "INVALID_JSON", "approval response must be JSON");
    }
    const choice = isRecord(parsed) ? parsed.choice : undefined;
    if (choice !== "approve_once" && choice !== "decline" && choice !== "cancel") {
      throw new ProtocolError(400, "INVALID_APPROVAL_CHOICE", "approval choice is not allowed");
    }
    const execution = await this.approvalCommand(
      sessionId,
      requireCommandId(request.headers),
      approvalId,
      choice,
    );
    this.#sendAcknowledgement(response, execution);
    this.#logger(
      `approval ${choice} ${execution.duplicate ? "replayed" : "accepted"}`,
    );
  }

  async #submitPhoto(
    request: IncomingMessage,
    response: ServerResponse,
    sessionId: string,
  ): Promise<void> {
    const contentType = request.headers["content-type"]?.toLowerCase() ?? "";
    if (!contentType.includes("application/x-3gent-rgb565")
      || !contentType.includes("width=400") || !contentType.includes("height=240")) {
      throw new ProtocolError(
        415,
        "INVALID_PHOTO_FORMAT",
        "expected RGB565 400x240 photo",
      );
    }
    const commandId = requireCommandId(request.headers);
    const duplicate = this.#commands.get(commandId);
    if (duplicate !== undefined) {
      await discardBody(request, 400 * 240 * 2);
      this.#sendJson(response, 200, duplicate);
      return;
    }
    if (this.#photoCaptureActive) {
      throw new ProtocolError(409, "PHOTO_CAPTURE_BUSY", "another photo upload is active", true);
    }
    this.#photoCaptureActive = true;
    const capturePath = immutablePhotoPath(this.#photoPath, commandId);
    try {
      const byteLength = await saveRgb565RequestAsBmp(request, capturePath);
      await publishLatestPhoto(capturePath, this.#photoPath);
      const replaced = this.#pendingPhotos.get(sessionId);
      this.#pendingPhotos.set(sessionId, capturePath);
      if (replaced !== undefined && replaced !== capturePath) {
        await this.#removePhoto(replaced);
      }
      this.events.append(sessionId, "capture.photo.ready", {
        kind: "photo",
        width: 400,
        height: 240,
        bytes: byteLength,
      });
      this.#sendNewAcknowledgement(response, commandId, sessionId);
      this.#logger(`photo capture accepted: ${byteLength} RGB565 bytes`);
    } catch (error) {
      await this.#removePhoto(capturePath);
      throw error;
    } finally {
      this.#photoCaptureActive = false;
    }
  }

  async #removePhoto(path: string): Promise<void> {
    try {
      await rm(path, {force: true});
    } catch (error) {
      this.#logger(
        `photo cleanup warning: ${error instanceof Error ? error.message : "unknown error"}`,
      );
    }
  }

  async #executeCommand(
    sessionId: string,
    commandId: string,
    action: () => Promise<void>,
  ): Promise<CommandExecution> {
    if (!isIdentifier(commandId, "cmd")) {
      throw new ProtocolError(400, "INVALID_COMMAND_ID", "invalid command ID");
    }
    const duplicate = this.#commands.get(commandId);
    if (duplicate !== undefined) {
      return {acknowledgement: duplicate, duplicate: true};
    }
    const inFlight = this.#inFlightCommands.get(commandId);
    if (inFlight !== undefined) {
      return {acknowledgement: await inFlight, duplicate: true};
    }
    const operation = (async (): Promise<CommandAcknowledgement> => {
      await action();
      const acknowledgement: CommandAcknowledgement = {
        protocolVersion: PROTOCOL_VERSION,
        commandId,
        accepted: true,
        duplicate: false,
        sessionId,
        lastSequence: this.events.latestSequence(sessionId),
      };
      this.#commands.remember(acknowledgement);
      return acknowledgement;
    })();
    this.#inFlightCommands.set(commandId, operation);
    try {
      return {acknowledgement: await operation, duplicate: false};
    } finally {
      this.#inFlightCommands.delete(commandId);
    }
  }

  async #executeSessionCommand(
    commandId: string,
    action: () => Promise<SessionSummary>,
  ): Promise<{session: SessionSummary; duplicate: boolean}> {
    if (!isIdentifier(commandId, "cmd")) {
      throw new ProtocolError(400, "INVALID_COMMAND_ID", "invalid command ID");
    }
    const stored = this.#sessionCommandResults.get(commandId);
    if (stored !== undefined) {
      return {session: stored, duplicate: true};
    }
    const inFlight = this.#inFlightSessionCommands.get(commandId);
    if (inFlight !== undefined) {
      return {session: await inFlight, duplicate: true};
    }
    const operation = action();
    this.#inFlightSessionCommands.set(commandId, operation);
    try {
      const session = await operation;
      this.#sessionCommandResults.set(commandId, session);
      while (this.#sessionCommandResults.size > 256) {
        const oldest = this.#sessionCommandResults.keys().next().value as string | undefined;
        if (oldest === undefined) {
          break;
        }
        this.#sessionCommandResults.delete(oldest);
      }
      return {session, duplicate: false};
    } finally {
      this.#inFlightSessionCommands.delete(commandId);
    }
  }

  #sendAcknowledgement(
    response: ServerResponse,
    execution: CommandExecution,
  ): void {
    this.#sendJson(
      response,
      execution.duplicate ? 200 : 202,
      execution.acknowledgement,
    );
  }

  #sendNewAcknowledgement(
    response: ServerResponse,
    commandId: string,
    sessionId: string,
  ): void {
    const acknowledgement: CommandAcknowledgement = {
      protocolVersion: PROTOCOL_VERSION,
      commandId,
      accepted: true,
      duplicate: false,
      sessionId,
      lastSequence: this.events.latestSequence(sessionId),
    };
    this.#commands.remember(acknowledgement);
    this.#sendJson(response, 202, acknowledgement);
  }

  #sendJson(response: ServerResponse, status: number, body: unknown): void {
    this.#verboseLog(`bridge -> 3DS ${status} ${JSON.stringify(body)}`);
    sendJson(response, status, body);
  }

  #logRequest(request: IncomingMessage, url: URL): void {
    if (!this.#verbose) {
      return;
    }
    const protocol = request.headers["x-3gent-protocol-version"] ?? "-";
    const command = request.headers["x-3gent-command-id"] ?? "-";
    const contentType = request.headers["content-type"] ?? "-";
    const framing = request.headers["content-length"] !== undefined
      ? `length=${request.headers["content-length"]}`
      : `transfer=${request.headers["transfer-encoding"] ?? "-"}`;
    this.#verboseLog(
      `3DS -> bridge ${request.method ?? "UNKNOWN"} ${url.pathname}${url.search} `
      + `protocol=${protocol} command=${command} content-type=${contentType} ${framing}`,
    );
  }

  #verboseLog(message: string): void {
    if (this.#verbose) {
      this.#logger(`[${new Date().toISOString()}] ${message}`);
    }
  }

}

export function createBridgeServer(options: BridgeServerOptions = {}): {
  application: BridgeApplication;
  server: ReturnType<typeof createServer>;
} {
  const application = new BridgeApplication(options);
  const server = createServer((request, response) => {
    void application.handle(request, response);
  });
  server.keepAliveTimeout = 10 * 60_000;
  server.headersTimeout = server.keepAliveTimeout + 5_000;
  server.on("connection", (socket) => socket.setNoDelay(true));
  return {application, server};
}

async function drainBody(
  request: IncomingMessage,
  maximumBytes: number,
): Promise<Buffer> {
  const chunks: Buffer[] = [];
  let total = 0;
  let tooLarge = false;
  for await (const rawChunk of request) {
    const chunk = Buffer.isBuffer(rawChunk)
      ? rawChunk
      : Buffer.from(rawChunk as Uint8Array);
    total += chunk.byteLength;
    if (total <= maximumBytes) {
      chunks.push(chunk);
    } else {
      tooLarge = true;
    }
  }
  if (tooLarge) {
    throw new ProtocolError(413, "BODY_TOO_LARGE", "request body exceeds its limit");
  }
  return Buffer.concat(chunks, total);
}

async function readJsonObject(
  request: IncomingMessage,
  maximumBytes: number,
): Promise<Record<string, unknown>> {
  const contentType = request.headers["content-type"]?.toLowerCase() ?? "";
  if (!contentType.startsWith("application/json")) {
    throw new ProtocolError(415, "INVALID_CONTENT_TYPE", "application/json required");
  }
  const body = await drainBody(request, maximumBytes);
  try {
    const value: unknown = JSON.parse(body.toString("utf8"));
    if (!isRecord(value)) {
      throw new Error("not an object");
    }
    return value;
  } catch {
    throw new ProtocolError(400, "INVALID_JSON", "request body must be a JSON object");
  }
}

async function discardBody(
  request: IncomingMessage,
  maximumBytes: number,
  onChunk?: (chunkBytes: number, totalBytes: number) => void,
): Promise<void> {
  let total = 0;
  for await (const rawChunk of request) {
    total += Buffer.byteLength(rawChunk as Uint8Array);
    if (total > maximumBytes) {
      throw new ProtocolError(413, "BODY_TOO_LARGE", "request body exceeds its limit");
    }
    onChunk?.(Buffer.byteLength(rawChunk as Uint8Array), total);
  }
}

function parseBoundedInteger(
  rawValue: string | null,
  name: string,
  minimum: number,
  maximum: number,
): number {
  if (rawValue === null || !/^\d+$/.test(rawValue)) {
    throw new ProtocolError(400, "INVALID_QUERY", `${name} must be an integer`);
  }
  const value = Number(rawValue);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new ProtocolError(400, "INVALID_QUERY", `${name} is out of range`);
  }
  return value;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function splitUtf8(text: string, maximumBytes: number): string[] {
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
