import { spawn } from "node:child_process";
import { readFile } from "node:fs/promises";

export interface Transcriber {
  readonly id: string;
  transcribe(wavPath: string): Promise<string>;
}

export class StaticTranscriber implements Transcriber {
  public readonly id = "mock";

  public constructor(private readonly transcript = "Voice capture received. This is a mock transcript.") {}

  public async transcribe(_wavPath: string): Promise<string> {
    return this.transcript;
  }
}

export interface CommandTranscriberOptions {
  executable: string;
  arguments?: readonly string[];
  timeoutMs?: number;
  maximumOutputBytes?: number;
}

export class CommandTranscriber implements Transcriber {
  public readonly id = "command";
  readonly #arguments: readonly string[];
  readonly #timeoutMs: number;
  readonly #maximumOutputBytes: number;

  public constructor(private readonly options: CommandTranscriberOptions) {
    if (options.executable.trim().length === 0) {
      throw new RangeError("transcription executable is required");
    }
    this.#arguments = options.arguments ?? [];
    this.#timeoutMs = options.timeoutMs ?? 120_000;
    this.#maximumOutputBytes = options.maximumOutputBytes ?? 16 * 1024;
  }

  public async transcribe(wavPath: string): Promise<string> {
    const child = spawn(this.options.executable, [...this.#arguments, wavPath], {
      stdio: ["ignore", "pipe", "pipe"],
    });
    const stdout: Buffer[] = [];
    const stderr: Buffer[] = [];
    let stdoutBytes = 0;
    let stderrBytes = 0;
    const timeout = setTimeout(() => child.kill(), this.#timeoutMs);
    try {
      const exit = new Promise<{code: number | null; signal: NodeJS.Signals | null}>((resolve, reject) => {
        child.once("error", reject);
        child.once("exit", (code, signal) => resolve({code, signal}));
      });
      child.stdout.on("data", (chunk: Buffer) => {
        stdoutBytes += chunk.byteLength;
        if (stdoutBytes > this.#maximumOutputBytes) {
          child.kill();
        } else {
          stdout.push(chunk);
        }
      });
      child.stderr.on("data", (chunk: Buffer) => {
        stderrBytes += chunk.byteLength;
        if (stderrBytes <= this.#maximumOutputBytes) {
          stderr.push(chunk);
        }
      });
      const result = await exit;
      if (stdoutBytes > this.#maximumOutputBytes) {
        throw new Error("transcription output exceeded its bound");
      }
      if (result.code !== 0) {
        const detail = Buffer.concat(stderr).toString("utf8").trim();
        throw new Error(`transcription command failed (${result.signal ?? `code ${String(result.code)}`})${detail.length > 0 ? `: ${detail}` : ""}`);
      }
      return requireTranscript(Buffer.concat(stdout).toString("utf8"));
    } finally {
      clearTimeout(timeout);
    }
  }
}

export interface OpenAiTranscriberOptions {
  apiKey: string;
  model?: string;
  baseUrl?: string;
  timeoutMs?: number;
}

export class OpenAiTranscriber implements Transcriber {
  public readonly id = "openai";
  readonly #model: string;
  readonly #baseUrl: string;
  readonly #timeoutMs: number;

  public constructor(private readonly options: OpenAiTranscriberOptions) {
    if (options.apiKey.trim().length === 0) {
      throw new RangeError("OpenAI API key is required");
    }
    this.#model = options.model ?? "gpt-4o-mini-transcribe";
    this.#baseUrl = (options.baseUrl ?? "https://api.openai.com/v1").replace(/\/$/, "");
    this.#timeoutMs = options.timeoutMs ?? 120_000;
  }

  public async transcribe(wavPath: string): Promise<string> {
    const wav = await readFile(wavPath);
    const form = new FormData();
    form.append("file", new Blob([wav], {type: "audio/wav"}), "capture.wav");
    form.append("model", this.#model);
    const response = await fetch(`${this.#baseUrl}/audio/transcriptions`, {
      method: "POST",
      headers: {Authorization: `Bearer ${this.options.apiKey}`},
      body: form,
      signal: AbortSignal.timeout(this.#timeoutMs),
    });
    const body = await response.text();
    if (!response.ok) {
      throw new Error(`OpenAI transcription failed with HTTP ${response.status}: ${body.slice(0, 500)}`);
    }
    let parsed: unknown;
    try {
      parsed = JSON.parse(body);
    } catch {
      throw new Error("OpenAI transcription returned invalid JSON");
    }
    if (!isRecord(parsed) || typeof parsed.text !== "string") {
      throw new Error("OpenAI transcription response has no text");
    }
    return requireTranscript(parsed.text);
  }
}

function requireTranscript(value: string): string {
  const transcript = value.trim();
  if (transcript.length === 0) {
    throw new Error("transcription was empty");
  }
  if (Buffer.byteLength(transcript, "utf8") > 4 * 1024) {
    throw new Error("transcript exceeds the 4 KiB capture limit");
  }
  return transcript;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
