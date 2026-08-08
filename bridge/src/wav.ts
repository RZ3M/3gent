import { mkdir, open, rename, rm } from "node:fs/promises";
import type { IncomingMessage } from "node:http";
import { dirname } from "node:path";

import {
  AUDIO_BYTES_PER_SECOND,
  AUDIO_SAMPLE_RATE,
  MAX_AUDIO_BYTES,
  ProtocolError,
} from "./protocol.js";

const WAV_HEADER_SIZE = 44;

function wavHeader(pcmBytes: number): Buffer {
  const header = Buffer.alloc(WAV_HEADER_SIZE);
  header.write("RIFF", 0, "ascii");
  header.writeUInt32LE(36 + pcmBytes, 4);
  header.write("WAVE", 8, "ascii");
  header.write("fmt ", 12, "ascii");
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22);
  header.writeUInt32LE(AUDIO_SAMPLE_RATE, 24);
  header.writeUInt32LE(AUDIO_BYTES_PER_SECOND, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write("data", 36, "ascii");
  header.writeUInt32LE(pcmBytes, 40);
  return header;
}

export async function savePcmRequestAsWav(
  request: IncomingMessage,
  capturePath: string,
  maximumBytes = MAX_AUDIO_BYTES,
  onChunk?: (chunkBytes: number, totalBytes: number) => void,
): Promise<number> {
  const temporaryPath = `${capturePath}.tmp`;
  await mkdir(dirname(capturePath), {recursive: true});
  const file = await open(temporaryPath, "w");
  let total = 0;

  try {
    await file.write(Buffer.alloc(WAV_HEADER_SIZE));
    for await (const rawChunk of request) {
      const chunk = Buffer.isBuffer(rawChunk)
        ? rawChunk
        : Buffer.from(rawChunk as Uint8Array);
      total += chunk.byteLength;
      if (total > maximumBytes) {
        throw new ProtocolError(
          413,
          "AUDIO_TOO_LARGE",
          "audio capture exceeds the five-minute Stage 1 limit",
        );
      }
      await file.write(chunk);
      onChunk?.(chunk.byteLength, total);
    }

    if (total === 0 || total % 2 !== 0) {
      throw new ProtocolError(
        400,
        "INVALID_AUDIO",
        "audio must contain complete non-empty PCM16 samples",
      );
    }
    await file.write(wavHeader(total), 0, WAV_HEADER_SIZE, 0);
    await file.close();
    await rename(temporaryPath, capturePath);
    return total;
  } catch (error) {
    await file.close().catch(() => undefined);
    await rm(temporaryPath, {force: true}).catch(() => undefined);
    throw error;
  }
}
