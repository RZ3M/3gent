import { createWriteStream } from "node:fs";
import { copyFile, mkdir, rename, rm } from "node:fs/promises";
import type { IncomingMessage } from "node:http";
import { dirname, extname } from "node:path";
import { once } from "node:events";

import { ProtocolError } from "./protocol.js";

export const PHOTO_WIDTH = 400;
export const PHOTO_HEIGHT = 240;
export const PHOTO_RGB565_BYTES = PHOTO_WIDTH * PHOTO_HEIGHT * 2;
const BMP_HEADER_BYTES = 66;

export function immutablePhotoPath(latestPath: string, commandId: string): string {
  const extension = extname(latestPath);
  const stem = extension.length === 0
    ? latestPath
    : latestPath.slice(0, -extension.length);
  return `${stem}.${commandId}${extension || ".bmp"}`;
}

export async function publishLatestPhoto(
  capturePath: string,
  latestPath: string,
): Promise<void> {
  const temporary = `${latestPath}.partial`;
  await mkdir(dirname(latestPath), {recursive: true});
  try {
    await copyFile(capturePath, temporary);
    await rename(temporary, latestPath);
  } catch (error) {
    await rm(temporary, {force: true});
    throw error;
  }
}

export async function saveRgb565RequestAsBmp(
  request: IncomingMessage,
  destination: string,
): Promise<number> {
  await mkdir(dirname(destination), {recursive: true});
  const temporary = `${destination}.partial`;
  const output = createWriteStream(temporary, {flags: "w"});
  let bytes = 0;
  let failed = false;
  try {
    output.write(bmpHeader());
    for await (const rawChunk of request) {
      const chunk = Buffer.isBuffer(rawChunk) ? rawChunk : Buffer.from(rawChunk as Uint8Array);
      bytes += chunk.byteLength;
      if (bytes > PHOTO_RGB565_BYTES) {
        failed = true;
        continue;
      }
      if (!output.write(chunk)) {
        await once(output, "drain");
      }
    }
    if (failed || bytes !== PHOTO_RGB565_BYTES) {
      throw new ProtocolError(
        400,
        "INVALID_PHOTO_SIZE",
        `RGB565 photo must contain exactly ${PHOTO_RGB565_BYTES} bytes`,
      );
    }
    output.end();
    await once(output, "close");
    await rename(temporary, destination);
    return bytes;
  } catch (error) {
    output.destroy();
    await rm(temporary, {force: true});
    throw error;
  }
}

function bmpHeader(): Buffer {
  const header = Buffer.alloc(BMP_HEADER_BYTES);
  header.write("BM", 0, "ascii");
  header.writeUInt32LE(BMP_HEADER_BYTES + PHOTO_RGB565_BYTES, 2);
  header.writeUInt32LE(BMP_HEADER_BYTES, 10);
  header.writeUInt32LE(40, 14);
  header.writeInt32LE(PHOTO_WIDTH, 18);
  header.writeInt32LE(-PHOTO_HEIGHT, 22);
  header.writeUInt16LE(1, 26);
  header.writeUInt16LE(16, 28);
  header.writeUInt32LE(3, 30); // BI_BITFIELDS
  header.writeUInt32LE(PHOTO_RGB565_BYTES, 34);
  header.writeUInt32LE(0x001f, 54); // camera buffer red mask
  header.writeUInt32LE(0x07e0, 58);
  header.writeUInt32LE(0xf800, 62); // camera buffer blue mask
  return header;
}
