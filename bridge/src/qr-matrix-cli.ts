/*
 * Prints a QR matrix as `#`/`.` rows so `tools/qr-check/` can decode it with
 * the same vendored quirc build that runs on the 3DS.
 *
 *   node dist/src/qr-matrix-cli.js "3gent://pair?v=1&..."
 */
import { encodeQr } from "./qr-encode.js";

const text = process.argv[2];
if (text === undefined) {
  console.error("usage: qr-matrix-cli <text>");
  process.exit(2);
}

const matrix = encodeQr(text);
for (const row of matrix.modules) {
  console.log(row.map((dark) => (dark ? "#" : ".")).join(""));
}
