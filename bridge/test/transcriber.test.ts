import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import assert from "node:assert/strict";
import test from "node:test";

import { CommandTranscriber, OpenAiTranscriber } from "../src/transcriber.js";

test("runs a bounded local transcription command without a shell", async () => {
  const directory = await mkdtemp(join(tmpdir(), "3gent-transcriber-"));
  const wavPath = join(directory, "capture.wav");
  await writeFile(wavPath, "RIFF");
  try {
    const transcriber = new CommandTranscriber({
      executable: process.execPath,
      arguments: ["-e", "process.stdout.write('local transcript')"],
    });
    assert.equal(await transcriber.transcribe(wavPath), "local transcript");
  } finally {
    await rm(directory, {recursive: true, force: true});
  }
});

test("rejects failed and oversized local transcription output", async () => {
  const failed = new CommandTranscriber({
    executable: process.execPath,
    arguments: ["-e", "process.stderr.write('bad audio');process.exit(2)"],
  });
  await assert.rejects(() => failed.transcribe("unused.wav"), /bad audio/);

  const oversized = new CommandTranscriber({
    executable: process.execPath,
    arguments: ["-e", "process.stdout.write('x'.repeat(100))"],
    maximumOutputBytes: 32,
  });
  await assert.rejects(() => oversized.transcribe("unused.wav"), /exceeded/);
});

test("posts WAV multipart data to an OpenAI-compatible transcription endpoint", async () => {
  let authorization = "";
  let body = Buffer.alloc(0);
  const server = createServer(async (request, response) => {
    authorization = request.headers.authorization ?? "";
    const chunks: Buffer[] = [];
    for await (const chunk of request) {
      chunks.push(Buffer.from(chunk as Uint8Array));
    }
    body = Buffer.concat(chunks);
    response.writeHead(200, {"Content-Type": "application/json"});
    response.end(JSON.stringify({text: "hosted transcript"}));
  });
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const directory = await mkdtemp(join(tmpdir(), "3gent-openai-transcriber-"));
  const wavPath = join(directory, "capture.wav");
  await writeFile(wavPath, Buffer.from("RIFF-test-wave"));
  try {
    const port = (server.address() as AddressInfo).port;
    const transcriber = new OpenAiTranscriber({
      apiKey: "test-key",
      model: "gpt-4o-mini-transcribe",
      baseUrl: `http://127.0.0.1:${port}/v1`,
    });
    assert.equal(await transcriber.transcribe(wavPath), "hosted transcript");
    assert.equal(authorization, "Bearer test-key");
    assert.ok(body.includes(Buffer.from("gpt-4o-mini-transcribe")));
    assert.ok(body.includes(Buffer.from("RIFF-test-wave")));
  } finally {
    await rm(directory, {recursive: true, force: true});
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});
