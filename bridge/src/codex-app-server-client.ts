import { spawn, type ChildProcessWithoutNullStreams, type SpawnOptionsWithoutStdio } from "node:child_process";

export interface JsonRpcNotification {
  method: string;
  params: unknown;
}

export interface JsonRpcServerRequest {
  id: string | number;
  method: string;
  params: unknown;
}

export interface JsonRpcRemoteErrorBody {
  code: number;
  message: string;
  data?: unknown;
}

export interface CodexAppServerClientOptions {
  /** Defaults to the Codex executable on PATH. */
  executable?: string;
  /** Defaults to the supported local stdio app-server transport. */
  arguments?: readonly string[];
  cwd?: string;
  env?: NodeJS.ProcessEnv;
  clientInfo?: {
    name: string;
    title?: string;
    version: string;
  };
  requestTimeoutMs?: number;
  maximumLineBytes?: number;
  maximumPendingRequests?: number;
  onNotification?: (notification: JsonRpcNotification) => void | Promise<void>;
  onServerRequest?: (
    request: JsonRpcServerRequest,
    responder: JsonRpcServerRequestResponder,
  ) => void | Promise<void>;
  onStderr?: (text: string) => void;
  onProtocolError?: (error: Error) => void;
}

export interface JsonRpcServerRequestResponder {
  respond(result: unknown): void;
  respondError(error: JsonRpcRemoteErrorBody): void;
}

export interface CodexAppServerInitializeResult {
  userAgent: string;
  codexHome: string;
  platformFamily: string;
  platformOs: string;
}

export class CodexAppServerRemoteError extends Error {
  public constructor(
    public readonly body: JsonRpcRemoteErrorBody,
  ) {
    super(`Codex app-server error ${body.code}: ${body.message}`);
  }
}

export class CodexAppServerClientError extends Error {}

interface PendingRequest {
  method: string;
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
  timeout: NodeJS.Timeout;
}

type ClientState = "new" | "starting" | "ready" | "closed" | "failed";

const DEFAULT_ARGUMENTS = ["app-server", "--listen", "stdio://"] as const;
const DEFAULT_REQUEST_TIMEOUT_MS = 15_000;
const DEFAULT_MAXIMUM_LINE_BYTES = 64 * 1024;
const DEFAULT_MAXIMUM_PENDING_REQUESTS = 64;

/**
 * Narrow JSON-RPC/JSONL transport for a local Codex app-server child process.
 * This class deliberately understands transport only; Codex events are left as
 * opaque JSON for an adapter to translate at its own boundary.
 */
export class CodexAppServerClient {
  readonly #options: Required<Pick<
    CodexAppServerClientOptions,
    "executable" | "arguments" | "requestTimeoutMs" | "maximumLineBytes" | "maximumPendingRequests"
  >> & CodexAppServerClientOptions;
  readonly #pending = new Map<number, PendingRequest>();
  #process: ChildProcessWithoutNullStreams | null = null;
  #state: ClientState = "new";
  #nextRequestId = 1;
  #inputRemainder = Buffer.alloc(0);
  #startPromise: Promise<CodexAppServerInitializeResult> | null = null;

  public constructor(options: CodexAppServerClientOptions = {}) {
    const requestTimeoutMs = options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
    const maximumLineBytes = options.maximumLineBytes ?? DEFAULT_MAXIMUM_LINE_BYTES;
    const maximumPendingRequests = options.maximumPendingRequests
      ?? DEFAULT_MAXIMUM_PENDING_REQUESTS;
    if (!Number.isInteger(requestTimeoutMs) || requestTimeoutMs <= 0) {
      throw new RangeError("requestTimeoutMs must be a positive integer");
    }
    if (!Number.isInteger(maximumLineBytes) || maximumLineBytes < 32) {
      throw new RangeError("maximumLineBytes must be an integer of at least 32");
    }
    if (!Number.isInteger(maximumPendingRequests) || maximumPendingRequests <= 0) {
      throw new RangeError("maximumPendingRequests must be a positive integer");
    }
    this.#options = {
      ...options,
      executable: options.executable ?? "codex",
      arguments: options.arguments ?? DEFAULT_ARGUMENTS,
      requestTimeoutMs,
      maximumLineBytes,
      maximumPendingRequests,
    };
  }

  public get state(): ClientState {
    return this.#state;
  }

  public async start(): Promise<CodexAppServerInitializeResult> {
    if (this.#state === "ready") {
      throw new CodexAppServerClientError("Codex app-server client is already started");
    }
    if (this.#startPromise !== null) {
      return await this.#startPromise;
    }
    if (this.#state !== "new") {
      throw new CodexAppServerClientError("Codex app-server client is unavailable");
    }

    this.#state = "starting";
    this.#startPromise = this.#startInternal();
    try {
      return await this.#startPromise;
    } finally {
      this.#startPromise = null;
    }
  }

  public async request<TResult = unknown>(
    method: string,
    params: unknown,
    timeoutMs = this.#options.requestTimeoutMs,
  ): Promise<TResult> {
    if (this.#state !== "ready") {
      throw new CodexAppServerClientError("Codex app-server client is not ready");
    }
    return await this.#requestInternal<TResult>(method, params, timeoutMs);
  }

  public notify(method: string, params: unknown): void {
    if (this.#state !== "ready") {
      throw new CodexAppServerClientError("Codex app-server client is not ready");
    }
    this.#write({jsonrpc: "2.0", method, params});
  }

  public respond(id: string | number, result: unknown): void {
    this.#write({jsonrpc: "2.0", id, result: result ?? null});
  }

  public respondError(id: string | number, error: JsonRpcRemoteErrorBody): void {
    this.#write({jsonrpc: "2.0", id, error});
  }

  public async close(): Promise<void> {
    if (this.#state === "closed") {
      return;
    }
    const process = this.#process;
    this.#fail(new CodexAppServerClientError("Codex app-server client was closed"), "closed");
    if (process === null || process.exitCode !== null || process.signalCode !== null) {
      return;
    }
    await new Promise<void>((resolve) => {
      process.once("exit", () => resolve());
      process.kill();
    });
  }

  async #startInternal(): Promise<CodexAppServerInitializeResult> {
    try {
      const spawnOptions: SpawnOptionsWithoutStdio = {
        cwd: this.#options.cwd,
        env: this.#options.env,
        stdio: "pipe",
      };
      this.#process = spawn(
        this.#options.executable,
        [...this.#options.arguments],
        spawnOptions,
      );
      this.#attachProcess(this.#process);
      const clientInfo = this.#options.clientInfo ?? {
        name: "3gent-bridge",
        title: "3gent Bridge",
        version: "0.1.0",
      };
      const result = await this.#requestInternal<CodexAppServerInitializeResult>(
        "initialize",
        {
          clientInfo,
          capabilities: {
            experimentalApi: false,
            requestAttestation: false,
          },
        },
        this.#options.requestTimeoutMs,
      );
      this.#assertInitializeResult(result);
      this.#state = "ready";
      return result;
    } catch (error) {
      const normalized = asError(error, "Codex app-server initialization failed");
      this.#fail(normalized, "failed");
      throw normalized;
    }
  }

  #attachProcess(process: ChildProcessWithoutNullStreams): void {
    process.stdout.on("data", (chunk: Buffer) => this.#onStdout(chunk));
    process.stderr.on("data", (chunk: Buffer) => {
      this.#options.onStderr?.(chunk.toString("utf8"));
    });
    process.stdin.on("error", (error) => this.#fail(error, "failed"));
    process.on("error", (error) => this.#fail(error, "failed"));
    process.on("exit", (code, signal) => {
      const reason = new CodexAppServerClientError(
        `Codex app-server exited (${signal ?? `code ${code ?? "unknown"}`})`,
      );
      if (this.#state !== "closed") {
        this.#fail(reason, "failed");
      }
    });
  }

  #onStdout(chunk: Buffer): void {
    if (this.#state === "closed" || this.#state === "failed") {
      return;
    }
    let offset = 0;
    while (offset < chunk.length) {
      const newline = chunk.indexOf(0x0a, offset);
      const end = newline === -1 ? chunk.length : newline;
      const segment = chunk.subarray(offset, end);
      if (this.#inputRemainder.length + segment.length > this.#options.maximumLineBytes) {
        this.#fail(
          new CodexAppServerClientError(
            `Codex app-server JSON-RPC line exceeds ${this.#options.maximumLineBytes} bytes`,
          ),
          "failed",
        );
        return;
      }
      if (segment.length > 0) {
        this.#inputRemainder = Buffer.concat([this.#inputRemainder, segment]);
      }
      if (newline === -1) {
        return;
      }

      const line = this.#inputRemainder;
      this.#inputRemainder = Buffer.alloc(0);
      offset = newline + 1;
      if (line.length === 0) {
        continue;
      }
      this.#handleLine(line);
    }
  }

  #handleLine(line: Buffer): void {
    let parsed: unknown;
    try {
      parsed = JSON.parse(line.toString("utf8"));
    } catch {
      this.#reportProtocolError(new CodexAppServerClientError("Codex app-server emitted invalid JSON"));
      return;
    }
    if (!isRecord(parsed)) {
      this.#reportProtocolError(new CodexAppServerClientError("Codex app-server emitted a non-object JSON-RPC message"));
      return;
    }
    if (isJsonRpcId(parsed.id) && typeof parsed.method === "string") {
      this.#dispatchServerRequest({
        id: parsed.id,
        method: parsed.method,
        params: parsed.params ?? null,
      });
      return;
    }
    if (typeof parsed.method === "string" && !("id" in parsed)) {
      this.#dispatchNotification({method: parsed.method, params: parsed.params ?? null});
      return;
    }
    if (isJsonRpcId(parsed.id) && ("result" in parsed || "error" in parsed)) {
      this.#dispatchResponse(parsed.id, parsed);
      return;
    }
    this.#reportProtocolError(new CodexAppServerClientError("Codex app-server emitted an unsupported JSON-RPC message"));
  }

  #dispatchResponse(id: string | number, message: Record<string, unknown>): void {
    if (typeof id !== "number") {
      this.#reportProtocolError(new CodexAppServerClientError("Codex app-server response ID must be numeric"));
      return;
    }
    const pending = this.#pending.get(id);
    if (pending === undefined) {
      this.#reportProtocolError(new CodexAppServerClientError(`Codex app-server replied to unknown request ${id}`));
      return;
    }
    this.#pending.delete(id);
    clearTimeout(pending.timeout);
    if (isRecord(message.error) && typeof message.error.code === "number"
      && typeof message.error.message === "string") {
      pending.reject(new CodexAppServerRemoteError({
        code: message.error.code,
        message: message.error.message,
        ...("data" in message.error ? {data: message.error.data} : {}),
      }));
      return;
    }
    if (!("result" in message)) {
      pending.reject(new CodexAppServerClientError(`Codex app-server response to ${pending.method} has no result`));
      return;
    }
    pending.resolve(message.result);
  }

  #dispatchNotification(notification: JsonRpcNotification): void {
    if (this.#options.onNotification === undefined) {
      return;
    }
    void Promise.resolve(this.#options.onNotification(notification)).catch((error: unknown) => {
      this.#reportProtocolError(asError(error, `notification handler failed for ${notification.method}`));
    });
  }

  #dispatchServerRequest(request: JsonRpcServerRequest): void {
    if (this.#options.onServerRequest === undefined) {
      this.respondError(request.id, {
        code: -32601,
        message: `No 3gent handler for ${request.method}`,
      });
      return;
    }
    let responded = false;
    const responder: JsonRpcServerRequestResponder = {
      respond: (result: unknown): void => {
        if (responded) {
          this.#reportProtocolError(new CodexAppServerClientError(`duplicate response to ${request.method}`));
          return;
        }
        responded = true;
        this.respond(request.id, result);
      },
      respondError: (error: JsonRpcRemoteErrorBody): void => {
        if (responded) {
          this.#reportProtocolError(new CodexAppServerClientError(`duplicate response to ${request.method}`));
          return;
        }
        responded = true;
        this.respondError(request.id, error);
      },
    };
    void Promise.resolve(this.#options.onServerRequest(request, responder)).catch((error: unknown) => {
      if (!responded) {
        responded = true;
        this.respondError(request.id, {
          code: -32603,
          message: asError(error, "server request handler failed").message,
        });
      }
    });
  }

  #requestInternal<TResult>(method: string, params: unknown, timeoutMs: number): Promise<TResult> {
    if (!Number.isInteger(timeoutMs) || timeoutMs <= 0) {
      throw new RangeError("timeoutMs must be a positive integer");
    }
    if (this.#pending.size >= this.#options.maximumPendingRequests) {
      throw new CodexAppServerClientError("Codex app-server pending request limit reached");
    }
    const id = this.#nextRequestId;
    this.#nextRequestId += 1;
    return new Promise<unknown>((resolve, reject) => {
      const timeout = setTimeout(() => {
        const pending = this.#pending.get(id);
        if (pending === undefined) {
          return;
        }
        this.#pending.delete(id);
        pending.reject(new CodexAppServerClientError(`Codex app-server request timed out: ${method}`));
      }, timeoutMs);
      this.#pending.set(id, {method, resolve, reject, timeout});
      try {
        this.#write({jsonrpc: "2.0", id, method, params});
      } catch (error) {
        const pending = this.#pending.get(id);
        if (pending !== undefined) {
          this.#pending.delete(id);
          clearTimeout(pending.timeout);
          pending.reject(asError(error, `failed to write ${method}`));
        }
      }
    }) as Promise<TResult>;
  }

  #write(message: Record<string, unknown>): void {
    const process = this.#process;
    if (process === null || process.stdin.destroyed || process.stdin.writableEnded) {
      throw new CodexAppServerClientError("Codex app-server stdin is unavailable");
    }
    process.stdin.write(`${JSON.stringify(message)}\n`);
  }

  #assertInitializeResult(result: CodexAppServerInitializeResult): void {
    if (typeof result.userAgent !== "string" || typeof result.codexHome !== "string"
      || typeof result.platformFamily !== "string" || typeof result.platformOs !== "string") {
      throw new CodexAppServerClientError("Codex app-server returned an invalid initialize result");
    }
  }

  #reportProtocolError(error: Error): void {
    this.#options.onProtocolError?.(error);
  }

  #fail(error: Error, state: Extract<ClientState, "closed" | "failed">): void {
    if (this.#state === "closed" || this.#state === "failed") {
      return;
    }
    this.#state = state;
    this.#inputRemainder = Buffer.alloc(0);
    for (const pending of this.#pending.values()) {
      clearTimeout(pending.timeout);
      pending.reject(error);
    }
    this.#pending.clear();
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isJsonRpcId(value: unknown): value is string | number {
  return typeof value === "string" || typeof value === "number";
}

function asError(value: unknown, fallback: string): Error {
  if (value instanceof CodexAppServerClientError || value instanceof CodexAppServerRemoteError) {
    return value;
  }
  if (value instanceof Error) {
    return new CodexAppServerClientError(`${fallback}: ${value.message}`);
  }
  return new CodexAppServerClientError(fallback);
}
