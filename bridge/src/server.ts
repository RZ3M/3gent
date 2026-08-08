import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { resolve } from "node:path";

import { CommandRegistry } from "./command-registry.js";
import { EventStore } from "./event-store.js";
import { FAKE_SESSION_ID, FakeAgent } from "./fake-agent.js";
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
} from "./protocol.js";
import { savePcmRequestAsWav } from "./wav.js";

const DEFAULT_CAPTURE_PATH = resolve("data", "latest.wav");
const SESSION_PATH = /^\/v1\/sessions\/([^/]+)$/;
const TEXT_CAPTURE_PATH = /^\/v1\/sessions\/([^/]+)\/captures\/text$/;
const AUDIO_CAPTURE_PATH = /^\/v1\/sessions\/([^/]+)\/captures\/audio$/;
const INTERRUPT_PATH = /^\/v1\/sessions\/([^/]+)\/turns\/current\/interrupt$/;
const APPROVAL_PATH = /^\/v1\/sessions\/([^/]+)\/approvals\/([^/]+)\/respond$/;

interface BridgeServerOptions {
  capturePath?: string;
  fakeDeltaIntervalMs?: number;
  logger?: (message: string) => void;
  verbose?: boolean;
  verbosePolls?: boolean;
}

export class BridgeApplication {
  public readonly events = new EventStore();
  public readonly fakeAgent: FakeAgent;
  readonly #commands = new CommandRegistry();
  readonly #capturePath: string;
  readonly #logger: (message: string) => void;
  readonly #verbose: boolean;
  readonly #verbosePolls: boolean;
  #audioCaptureActive = false;

  public constructor(options: BridgeServerOptions = {}) {
    this.#capturePath = options.capturePath ?? DEFAULT_CAPTURE_PATH;
    this.#logger = options.logger ?? console.log;
    this.#verbosePolls = options.verbosePolls ?? false;
    this.#verbose = (options.verbose ?? false) || this.#verbosePolls;
    this.fakeAgent = new FakeAgent(
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
          bridge: "3gent-stage1",
        });
        return;
      }

      if (!url.pathname.startsWith("/v1/")) {
        throw new ProtocolError(404, "NOT_FOUND", "route not found");
      }
      requireProtocolVersion(request.headers);

      if (url.pathname === "/v1/sessions" && request.method === "GET") {
        this.#sendJson(response, 200, {
          protocolVersion: PROTOCOL_VERSION,
          sessions: [this.fakeAgent.session()],
        });
        return;
      }
      if (url.pathname === "/v1/events" && request.method === "GET") {
        this.#sendEvents(request, url, response);
        return;
      }

      const sessionMatch = SESSION_PATH.exec(url.pathname);
      if (sessionMatch !== null && request.method === "GET") {
        this.#requireFakeSession(sessionMatch[1]);
        this.#sendJson(response, 200, {
          protocolVersion: PROTOCOL_VERSION,
          session: this.fakeAgent.session(),
        });
        return;
      }

      const textMatch = TEXT_CAPTURE_PATH.exec(url.pathname);
      if (textMatch !== null && request.method === "POST") {
        this.#requireFakeSession(textMatch[1]);
        await this.#submitText(request, response);
        return;
      }

      const audioMatch = AUDIO_CAPTURE_PATH.exec(url.pathname);
      if (audioMatch !== null && request.method === "POST") {
        this.#requireFakeSession(audioMatch[1]);
        await this.#submitAudio(request, response);
        return;
      }

      const interruptMatch = INTERRUPT_PATH.exec(url.pathname);
      if (interruptMatch !== null && request.method === "POST") {
        this.#requireFakeSession(interruptMatch[1]);
        await drainBody(request, 0);
        const duplicate = this.#runCommand(
          request,
          response,
          () => this.fakeAgent.interrupt(),
        );
        this.#logger(`interrupt ${duplicate ? "replayed" : "accepted"}`);
        return;
      }

      const approvalMatch = APPROVAL_PATH.exec(url.pathname);
      if (approvalMatch !== null && request.method === "POST") {
        this.#requireFakeSession(approvalMatch[1]);
        const approvalId = approvalMatch[2];
        if (approvalId === undefined || !isIdentifier(approvalId, "apr")) {
          throw new ProtocolError(400, "INVALID_APPROVAL_ID", "invalid approval ID");
        }
        await this.#respondToApproval(request, response, approvalId);
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

  public shutdown(): void {
    this.fakeAgent.shutdown();
  }

  #sendEvents(
    request: IncomingMessage,
    url: URL,
    response: ServerResponse,
  ): void {
    const sessionId = url.searchParams.get("sessionId");
    this.#requireFakeSession(sessionId ?? undefined);
    const after = parseBoundedInteger(url.searchParams.get("after"), "after", 0, Number.MAX_SAFE_INTEGER);
    const limit = parseBoundedInteger(url.searchParams.get("limit"), "limit", 1, MAX_EVENT_POLL_LIMIT);
    const events = this.events.after(FAKE_SESSION_ID, after, limit);
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
  ): Promise<void> {
    const contentType = request.headers["content-type"]?.toLowerCase() ?? "";
    if (!contentType.startsWith("text/plain")) {
      throw new ProtocolError(415, "INVALID_CONTENT_TYPE", "text/plain required");
    }
    const body = await drainBody(request, MAX_TEXT_CAPTURE_BYTES);
    const text = body.toString("utf8");
    this.#verboseLog(`3DS -> bridge text ${JSON.stringify(text)}`);
    if (text.trim().length === 0) {
      throw new ProtocolError(400, "EMPTY_TEXT_CAPTURE", "text capture is empty");
    }
    const duplicate = this.#runCommand(
      request,
      response,
      () => this.fakeAgent.submitText(text),
    );
    this.#logger(
      `text capture ${duplicate ? "replayed" : "accepted"}: ${body.byteLength} bytes`,
    );
  }

  async #submitAudio(
    request: IncomingMessage,
    response: ServerResponse,
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
    if (this.fakeAgent.session().activeTurnId !== null) {
      throw new ProtocolError(
        409,
        "SESSION_BUSY",
        "session already has an active turn",
        true,
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
      this.fakeAgent.submitAudio(byteLength);
      this.#sendNewAcknowledgement(response, commandId);
      this.#logger(`audio capture accepted: ${byteLength} PCM bytes`);
    } finally {
      this.#audioCaptureActive = false;
    }
  }

  async #respondToApproval(
    request: IncomingMessage,
    response: ServerResponse,
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
    const duplicate = this.#runCommand(
      request,
      response,
      () => this.fakeAgent.respondToApproval(approvalId, choice),
    );
    this.#logger(
      `approval ${choice} ${duplicate ? "replayed" : "accepted"}`,
    );
  }

  #runCommand(
    request: IncomingMessage,
    response: ServerResponse,
    action: () => void,
  ): boolean {
    const commandId = requireCommandId(request.headers);
    const duplicate = this.#commands.get(commandId);
    if (duplicate !== undefined) {
      this.#sendJson(response, 200, duplicate);
      return true;
    }
    action();
    this.#sendNewAcknowledgement(response, commandId);
    return false;
  }

  #sendNewAcknowledgement(
    response: ServerResponse,
    commandId: string,
  ): void {
    const acknowledgement: CommandAcknowledgement = {
      protocolVersion: PROTOCOL_VERSION,
      commandId,
      accepted: true,
      duplicate: false,
      sessionId: FAKE_SESSION_ID,
      lastSequence: this.events.latestSequence(FAKE_SESSION_ID),
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

  #requireFakeSession(sessionId: string | undefined): void {
    if (sessionId !== FAKE_SESSION_ID) {
      throw new ProtocolError(404, "SESSION_NOT_FOUND", "session does not exist");
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
