import { createServer, type Server, type Socket } from "node:net";

import { FAKE_SESSION_ID } from "./fake-agent.js";
import {
  EVENT_HISTORY_LIMIT,
  isIdentifier,
  PROTOCOL_VERSION,
  ProtocolError,
  type EventEnvelope,
} from "./protocol.js";
import { BridgeApplication, type CommandExecution } from "./server.js";

export const MAX_PUSH_FRAME_BYTES = 4 * 1024;
export const DEFAULT_CLIENT_IDLE_TIMEOUT_MS = 12_000;

interface PushServerOptions {
  clientIdleTimeoutMs?: number;
  blackholeAfterReady?: boolean;
  dropNextAcknowledgementAfterExecution?: boolean;
  logger?: (message: string) => void;
  verbose?: boolean;
  verboseHeartbeats?: boolean;
}

interface RecordValue {
  [key: string]: unknown;
}

type ApprovalChoice = "approve_once" | "decline" | "cancel";

class PushConnection {
  readonly #socket: Socket;
  readonly #application: BridgeApplication;
  readonly #logger: (message: string) => void;
  readonly #verbose: boolean;
  readonly #verboseHeartbeats: boolean;
  readonly #idleTimeoutMs: number;
  readonly #blackholeAfterReady: boolean;
  readonly #idleTimer: NodeJS.Timeout;
  readonly #takeDroppedAcknowledgement: () => boolean;
  #input = Buffer.alloc(0);
  #sessionId: string | null = null;
  #sendCursor = 0;
  #blocked = false;
  #closed = false;
  #lastInboundAt = Date.now();
  #unsubscribe: (() => void) | null = null;

  public constructor(
    socket: Socket,
    application: BridgeApplication,
    options: PushServerOptions,
    takeDroppedAcknowledgement: () => boolean,
  ) {
    this.#socket = socket;
    this.#application = application;
    this.#logger = options.logger ?? console.log;
    this.#verbose = options.verbose ?? false;
    this.#verboseHeartbeats = options.verboseHeartbeats ?? false;
    this.#idleTimeoutMs = options.clientIdleTimeoutMs
      ?? DEFAULT_CLIENT_IDLE_TIMEOUT_MS;
    this.#blackholeAfterReady = options.blackholeAfterReady ?? false;
    this.#takeDroppedAcknowledgement = takeDroppedAcknowledgement;

    socket.setNoDelay(true);
    socket.setKeepAlive(true, 5_000);
    socket.on("data", (chunk: Buffer) => this.#onData(chunk));
    socket.on("drain", () => this.#onDrain());
    socket.on("error", (error) => this.#debug(`push socket error: ${error.message}`));
    socket.on("close", () => this.#close());

    this.#idleTimer = setInterval(
      () => this.#checkIdle(),
      Math.min(1_000, Math.max(100, Math.floor(this.#idleTimeoutMs / 4))),
    );
    this.#idleTimer.unref();
  }

  #onData(chunk: Buffer): void {
    if (this.#closed) {
      return;
    }
    this.#lastInboundAt = Date.now();
    this.#input = Buffer.concat([this.#input, chunk]);

    for (;;) {
      const newline = this.#input.indexOf(0x0a);
      if (newline < 0) {
        if (this.#input.byteLength > MAX_PUSH_FRAME_BYTES) {
          this.#fail(new ProtocolError(413, "FRAME_TOO_LARGE", "control frame exceeds 4 KiB"), true);
        }
        return;
      }
      if (newline > MAX_PUSH_FRAME_BYTES) {
        this.#fail(new ProtocolError(413, "FRAME_TOO_LARGE", "control frame exceeds 4 KiB"), true);
        return;
      }
      const line = this.#input.subarray(0, newline);
      this.#input = this.#input.subarray(newline + 1);
      if (line.byteLength === 0) {
        continue;
      }
      this.#handleLine(line);
      if (this.#closed) {
        return;
      }
    }
  }

  #handleLine(line: Buffer): void {
    let parsed: unknown;
    try {
      parsed = JSON.parse(line.toString("utf8"));
    } catch {
      this.#fail(new ProtocolError(400, "INVALID_JSON", "control frame must be JSON"), true);
      return;
    }
    if (!isRecord(parsed)) {
      this.#fail(new ProtocolError(400, "INVALID_FRAME", "control frame must be an object"), true);
      return;
    }
    if (parsed.type !== "ping" || this.#verboseHeartbeats) {
      this.#debug(`3DS -> bridge push ${JSON.stringify(parsed)}`);
    }

    try {
      if (parsed.protocolVersion !== PROTOCOL_VERSION) {
        throw new ProtocolError(426, "UNSUPPORTED_PROTOCOL_VERSION", "unsupported push protocol version");
      }
      if (parsed.type === "connection.hello") {
        this.#handleHello(parsed);
      } else if (parsed.type === "ping") {
        this.#handlePing(parsed);
      } else if (parsed.type === "command") {
        this.#handleCommand(parsed);
      } else {
        throw new ProtocolError(400, "UNKNOWN_FRAME_TYPE", "unknown control frame type");
      }
    } catch (error) {
      const protocolError = error instanceof ProtocolError
        ? error
        : new ProtocolError(500, "INTERNAL_ERROR", "push command failed");
      this.#fail(protocolError, this.#sessionId === null);
    }
  }

  #handleHello(frame: RecordValue): void {
    if (this.#sessionId !== null) {
      throw new ProtocolError(409, "ALREADY_CONNECTED", "connection hello was already received");
    }
    const sessionId = frame.sessionId;
    const after = frame.after;
    if (typeof sessionId !== "string" || !isIdentifier(sessionId, "ses")) {
      throw new ProtocolError(400, "INVALID_SESSION_ID", "hello requires a valid session ID");
    }
    if (!Number.isSafeInteger(after) || (after as number) < 0) {
      throw new ProtocolError(400, "INVALID_CURSOR", "hello requires a non-negative event cursor");
    }

    const session = this.#application.session(sessionId);
    this.#sessionId = sessionId;
    this.#sendCursor = after as number;
    this.#unsubscribe = this.#application.events.subscribe(
      sessionId,
      () => this.#flushEvents(),
    );

    try {
      this.#application.eventsAfter(sessionId, this.#sendCursor, 1);
    } catch (error) {
      if (error instanceof ProtocolError
        && (error.code === "EVENT_CURSOR_EXPIRED" || error.code === "EVENT_CURSOR_AHEAD")) {
        this.#sendResync(error);
        return;
      }
      throw error;
    }

    this.#send({
      protocolVersion: PROTOCOL_VERSION,
      type: "connection.ready",
      session,
      lastSequence: session.lastSequence,
    });
    this.#logger(`push client connected: session=${sessionId} after=${this.#sendCursor}`);
    this.#flushEvents();
  }

  #handlePing(frame: RecordValue): void {
    if (this.#sessionId === null) {
      throw new ProtocolError(409, "HELLO_REQUIRED", "connection hello is required first");
    }
    const nonce = frame.nonce;
    if (!Number.isSafeInteger(nonce) || (nonce as number) < 0) {
      throw new ProtocolError(400, "INVALID_PING", "ping nonce must be a non-negative integer");
    }
    this.#send({
      protocolVersion: PROTOCOL_VERSION,
      type: "pong",
      nonce,
    });
  }

  #handleCommand(frame: RecordValue): void {
    if (this.#sessionId === null) {
      throw new ProtocolError(409, "HELLO_REQUIRED", "connection hello is required first");
    }
    const commandId = frame.commandId;
    const command = frame.command;
    if (typeof commandId !== "string" || !isIdentifier(commandId, "cmd")) {
      throw new ProtocolError(400, "INVALID_COMMAND_ID", "invalid command ID");
    }
    if (!isRecord(command) || typeof command.type !== "string") {
      throw new ProtocolError(400, "INVALID_COMMAND", "command payload is invalid");
    }

    let execution: CommandExecution;
    if (command.type === "capture.text") {
      if (typeof command.text !== "string") {
        throw new ProtocolError(400, "INVALID_TEXT_CAPTURE", "text capture is invalid");
      }
      execution = this.#application.submitTextCommand(
        this.#sessionId,
        commandId,
        command.text,
      );
      this.#logger(
        `push text capture ${execution.duplicate ? "replayed" : "accepted"}: `
        + `${Buffer.byteLength(command.text, "utf8")} bytes`,
      );
    } else if (command.type === "turn.interrupt") {
      execution = this.#application.interruptCommand(this.#sessionId, commandId);
      this.#logger(`push interrupt ${execution.duplicate ? "replayed" : "accepted"}`);
    } else if (command.type === "approval.respond") {
      if (typeof command.approvalId !== "string" || !isApprovalChoice(command.choice)) {
        throw new ProtocolError(400, "INVALID_APPROVAL_RESPONSE", "approval response is invalid");
      }
      execution = this.#application.approvalCommand(
        this.#sessionId,
        commandId,
        command.approvalId,
        command.choice,
      );
      this.#logger(
        `push approval ${command.choice} ${execution.duplicate ? "replayed" : "accepted"}`,
      );
    } else {
      throw new ProtocolError(400, "UNKNOWN_COMMAND", "unknown command type");
    }

    if (!execution.duplicate && this.#takeDroppedAcknowledgement()) {
      this.#debug("test hook dropped connection after command execution");
      this.#socket.destroy();
      return;
    }

    this.#send({
      protocolVersion: PROTOCOL_VERSION,
      type: "command.ack",
      acknowledgement: execution.acknowledgement,
    });
  }

  #flushEvents(): void {
    if (this.#closed || this.#blocked || this.#sessionId === null) {
      return;
    }
    try {
      while (!this.#blocked) {
        const events = this.#application.eventsAfter(
          this.#sessionId,
          this.#sendCursor,
          EVENT_HISTORY_LIMIT,
        );
        if (events.length === 0) {
          return;
        }
        for (const event of events) {
          if (!this.#sendEvent(event)) {
            return;
          }
        }
      }
    } catch (error) {
      const protocolError = error instanceof ProtocolError
        ? error
        : new ProtocolError(500, "INTERNAL_ERROR", "event push failed");
      if (protocolError.code === "EVENT_CURSOR_EXPIRED"
        || protocolError.code === "EVENT_CURSOR_AHEAD") {
        this.#sendResync(protocolError);
      } else {
        this.#fail(protocolError, true);
      }
    }
  }

  #sendEvent(event: EventEnvelope): boolean {
    const writable = this.#send({
      protocolVersion: PROTOCOL_VERSION,
      type: "event",
      event,
    });
    this.#sendCursor = event.sequence;
    return writable;
  }

  #send(frame: RecordValue): boolean {
    if (this.#closed) {
      return false;
    }
    if (this.#blocked) {
      this.#socket.destroy(new Error("push peer remained backpressured"));
      return false;
    }
    if (this.#blackholeAfterReady
      && this.#sessionId !== null
      && frame.type !== "connection.ready") {
      this.#debug(`test hook blackholed push frame type=${String(frame.type)}`);
      return true;
    }
    const line = `${JSON.stringify(frame)}\n`;
    if (Buffer.byteLength(line, "utf8") > MAX_PUSH_FRAME_BYTES) {
      throw new ProtocolError(500, "FRAME_TOO_LARGE", "outbound control frame exceeds 4 KiB");
    }
    if (frame.type !== "pong" || this.#verboseHeartbeats) {
      this.#debug(`bridge -> 3DS push ${line.trimEnd()}`);
    }
    const writable = this.#socket.write(line, "utf8");
    if (!writable) {
      this.#blocked = true;
      this.#socket.pause();
    }
    return writable;
  }

  #onDrain(): void {
    if (this.#closed) {
      return;
    }
    this.#blocked = false;
    this.#socket.resume();
    this.#flushEvents();
  }

  #fail(error: ProtocolError, closeAfterWrite: boolean): void {
    try {
      this.#send({
        protocolVersion: PROTOCOL_VERSION,
        type: "error",
        error: {
          code: error.code,
          message: error.message,
          retryable: error.retryable,
        },
      });
    } finally {
      if (closeAfterWrite) {
        this.#socket.end();
      }
    }
  }

  #sendResync(error: ProtocolError): void {
    if (this.#sessionId === null) {
      this.#fail(error, true);
      return;
    }
    const session = this.#application.session(this.#sessionId);
    this.#send({
      protocolVersion: PROTOCOL_VERSION,
      type: "resync.required",
      code: error.code,
      session,
      lastSequence: session.lastSequence,
    });
    this.#socket.end();
  }

  #checkIdle(): void {
    if (Date.now() - this.#lastInboundAt >= this.#idleTimeoutMs) {
      this.#debug("push client heartbeat timed out");
      this.#socket.destroy();
    }
  }

  #close(): void {
    if (this.#closed) {
      return;
    }
    this.#closed = true;
    clearInterval(this.#idleTimer);
    this.#unsubscribe?.();
    this.#unsubscribe = null;
    if (this.#sessionId !== null) {
      this.#logger(`push client disconnected: session=${this.#sessionId}`);
    }
  }

  #debug(message: string): void {
    if (this.#verbose) {
      this.#logger(`[${new Date().toISOString()}] ${message}`);
    }
  }
}

export function createPushControlServer(
  application: BridgeApplication,
  options: PushServerOptions = {},
): Server {
  let dropNextAcknowledgement = options.dropNextAcknowledgementAfterExecution
    ?? false;
  const takeDroppedAcknowledgement = () => {
    if (!dropNextAcknowledgement) {
      return false;
    }
    dropNextAcknowledgement = false;
    return true;
  };
  return createServer((socket) => {
    new PushConnection(
      socket,
      application,
      options,
      takeDroppedAcknowledgement,
    );
  });
}

function isRecord(value: unknown): value is RecordValue {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isApprovalChoice(value: unknown): value is ApprovalChoice {
  return value === "approve_once" || value === "decline" || value === "cancel";
}
