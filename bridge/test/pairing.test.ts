import { mkdtemp, rm } from "node:fs/promises";
import { request as httpRequest } from "node:http";
import { connect, type AddressInfo, type Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import assert from "node:assert/strict";

import {
  buildManualCode,
  buildPairingUrl,
  formatPairingCode,
  normalizePairingCode,
  PairingStore,
  sanitizeBridgeName,
} from "../src/pairing.js";
import { PROTOCOL_HEADER, PROTOCOL_VERSION } from "../src/protocol.js";
import { createPushControlServer } from "../src/push-server.js";
import { byteCapacity, encodeQr, QR_MAX_VERSION } from "../src/qr-encode.js";
import { renderQrToSvg, renderQrToTerminal } from "../src/qr-render.js";
import { createBridgeServer } from "../src/server.js";
import { FAKE_SESSION_ID } from "../src/fake-agent.js";

const ENDPOINT = {
  host: "192.168.1.42",
  httpPort: 8080,
  pushPort: 8081,
  bridgeName: "jm-mbp",
} as const;

/* -------------------------------------------------------- QR round trip -- */

/*
 * Reads a matrix back the way a decoder does: recover the mask from the format
 * information, undo it, walk the same zigzag, de-interleave the blocks and
 * parse the byte-mode segment. It shares no code with the encoder, so a
 * placement or masking mistake fails here rather than on a camera.
 */
function decodeMatrix(matrix: ReturnType<typeof encodeQr>): string {
  const {size, version, modules} = matrix;
  const dark = (row: number, column: number): boolean =>
    modules[row]?.[column] === true;

  let formatBits = 0;
  for (let index = 0; index < 15; index += 1) {
    let bit: boolean;
    if (index < 8) {
      bit = dark(8, size - 1 - index);
    } else {
      bit = dark(size - 15 + index, 8);
    }
    if (bit) {
      formatBits |= 1 << index;
    }
  }
  formatBits ^= 0b101010000010010;
  const eccIndicator = (formatBits >>> 13) & 0b11;
  assert.equal(eccIndicator, 0b00, "expected error-correction level M");
  const mask = (formatBits >>> 10) & 0b111;

  const functionModule = buildFunctionMap(size, version);
  const maskBit = (row: number, column: number): boolean => {
    switch (mask) {
      case 0: return (row + column) % 2 === 0;
      case 1: return row % 2 === 0;
      case 2: return column % 3 === 0;
      case 3: return (row + column) % 3 === 0;
      case 4: return (Math.floor(row / 2) + Math.floor(column / 3)) % 2 === 0;
      case 5: return ((row * column) % 2) + ((row * column) % 3) === 0;
      case 6: return ((((row * column) % 2) + ((row * column) % 3)) % 2) === 0;
      default: return ((((row + column) % 2) + ((row * column) % 3)) % 2) === 0;
    }
  };

  const bits: number[] = [];
  let upward = true;
  for (let right = size - 1; right >= 1; right -= 2) {
    const rightColumn = right <= 6 ? right - 1 : right;
    for (let step = 0; step < size; step += 1) {
      const row = upward ? size - 1 - step : step;
      for (const column of [rightColumn, rightColumn - 1]) {
        if (functionModule[row]?.[column] === true) {
          continue;
        }
        const value = dark(row, column) !== maskBit(row, column);
        bits.push(value ? 1 : 0);
      }
    }
    upward = !upward;
  }

  const codewords: number[] = [];
  for (let index = 0; index + 8 <= bits.length; index += 8) {
    let byte = 0;
    for (let offset = 0; offset < 8; offset += 1) {
      byte = (byte << 1) | (bits[index + offset] ?? 0);
    }
    codewords.push(byte);
  }

  const layout = BLOCK_LAYOUT[version - 1];
  assert.ok(layout !== undefined, `no test layout for version ${version}`);
  const blockSizes: number[] = [];
  for (const [count, dataSize] of layout.groups) {
    for (let block = 0; block < count; block += 1) {
      blockSizes.push(dataSize);
    }
  }
  const blocks: number[][] = blockSizes.map(() => []);
  let cursor = 0;
  const longest = Math.max(...blockSizes);
  for (let index = 0; index < longest; index += 1) {
    for (let block = 0; block < blockSizes.length; block += 1) {
      if (index < (blockSizes[block] ?? 0)) {
        blocks[block]?.push(codewords[cursor] ?? 0);
        cursor += 1;
      }
    }
  }

  const data = blocks.flat();
  const mode = (data[0] ?? 0) >>> 4;
  assert.equal(mode, 0b0100, "expected byte mode");
  const countBits = version < 10 ? 8 : 16;
  let stream = 4;
  const readBits = (length: number): number => {
    let value = 0;
    for (let index = 0; index < length; index += 1) {
      const position = stream + index;
      const byte = data[position >>> 3] ?? 0;
      value = (value << 1) | ((byte >>> (7 - (position & 7))) & 1);
    }
    stream += length;
    return value;
  };
  const length = readBits(countBits);
  const payload = Buffer.alloc(length);
  for (let index = 0; index < length; index += 1) {
    payload[index] = readBits(8);
  }
  return payload.toString("utf8");
}

const BLOCK_LAYOUT: readonly {groups: readonly (readonly [number, number])[]}[] = [
  {groups: [[1, 16]]},
  {groups: [[1, 28]]},
  {groups: [[1, 44]]},
  {groups: [[2, 32]]},
  {groups: [[2, 43]]},
  {groups: [[4, 27]]},
  {groups: [[4, 31]]},
  {groups: [[2, 38], [2, 39]]},
  {groups: [[3, 36], [2, 37]]},
  {groups: [[4, 43], [1, 44]]},
];

const ALIGNMENT: readonly (readonly number[])[] = [
  [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34],
  [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50],
];

function buildFunctionMap(size: number, version: number): boolean[][] {
  const map: boolean[][] = Array.from(
    {length: size},
    () => Array.from({length: size}, () => false),
  );
  const mark = (row: number, column: number): void => {
    const line = map[row];
    if (line !== undefined && column >= 0 && column < size) {
      line[column] = true;
    }
  };
  for (const [originRow, originColumn] of [[0, 0], [0, size - 7], [size - 7, 0]]) {
    for (let y = -1; y <= 7; y += 1) {
      for (let x = -1; x <= 7; x += 1) {
        const row = (originRow ?? 0) + y;
        if (row >= 0 && row < size) {
          mark(row, (originColumn ?? 0) + x);
        }
      }
    }
  }
  for (let index = 0; index < size; index += 1) {
    mark(6, index);
    mark(index, 6);
  }
  const centres = ALIGNMENT[version - 1] ?? [];
  for (const centreY of centres) {
    for (const centreX of centres) {
      const nearFinder = (centreY <= 8 && centreX <= 8)
        || (centreY <= 8 && centreX >= size - 9)
        || (centreY >= size - 9 && centreX <= 8);
      if (nearFinder) {
        continue;
      }
      for (let y = -2; y <= 2; y += 1) {
        for (let x = -2; x <= 2; x += 1) {
          mark(centreY + y, centreX + x);
        }
      }
    }
  }
  for (let index = 0; index <= 8; index += 1) {
    mark(8, index);
    mark(index, 8);
  }
  for (let index = 0; index < 8; index += 1) {
    mark(8, size - 1 - index);
    mark(size - 1 - index, 8);
  }
  if (version >= 7) {
    for (let y = 0; y < 6; y += 1) {
      for (let x = 0; x < 3; x += 1) {
        mark(y, size - 11 + x);
        mark(size - 11 + x, y);
      }
    }
  }
  return map;
}

test("QR matrices round trip through an independent reader", () => {
  const samples = [
    "A",
    "HELLO",
    buildPairingUrl(ENDPOINT, "K7M2QX4T9BWF"),
    "x".repeat(byteCapacity(7)),
    "y".repeat(byteCapacity(QR_MAX_VERSION)),
  ];
  for (const sample of samples) {
    const matrix = encodeQr(sample);
    assert.equal(matrix.size, matrix.version * 4 + 17);
    assert.equal(decodeMatrix(matrix), sample, `round trip failed for ${sample.length} bytes`);
  }
});

test("QR version is the smallest that fits, and capacity is enforced", () => {
  assert.equal(encodeQr("x".repeat(byteCapacity(1))).version, 1);
  assert.equal(encodeQr("x".repeat(byteCapacity(1) + 1)).version, 2);
  assert.equal(encodeQr("x".repeat(byteCapacity(5))).version, 5);
  assert.throws(
    () => encodeQr("x".repeat(byteCapacity(QR_MAX_VERSION) + 1)),
    /exceeds the version 10 capacity/,
  );
});

test("a pairing URL stays inside a comfortably scannable QR version", () => {
  const matrix = encodeQr(buildPairingUrl(
    {host: "192.168.100.200", httpPort: 65535, pushPort: 65534, bridgeName: "a".repeat(40)},
    "K7M2QX4T9BWF",
  ));
  assert.ok(matrix.version <= 7, `pairing QR grew to version ${matrix.version}`);
});

test("QR renderings describe the same matrix", () => {
  const matrix = encodeQr("3gent");
  const terminal = renderQrToTerminal(matrix);
  assert.equal(terminal.split("\n").length, Math.ceil((matrix.size + 8) / 2));
  const svg = renderQrToSvg(matrix, 8);
  const dark = matrix.modules.flat().filter(Boolean).length;
  assert.equal(svg.match(/<rect x=/g)?.length, dark);
});

/* ------------------------------------------------------------- store ---- */

async function withStore(
  body: (store: PairingStore, path: string, clock: {now: number}) => void | Promise<void>,
): Promise<void> {
  const directory = await mkdtemp(join(tmpdir(), "3gent-pairing-test-"));
  const path = join(directory, "devices.json");
  const clock = {now: 1_700_000_000_000};
  try {
    await body(new PairingStore({path, now: () => clock.now}), path, clock);
  } finally {
    await rm(directory, {recursive: true, force: true});
  }
}

test("pairing codes are single use and expire", async () => {
  await withStore((store, _path, clock) => {
    const offer = store.createOffer(ENDPOINT, 60_000);
    assert.equal(offer.url, buildPairingUrl(ENDPOINT, offer.code));
    assert.equal(offer.manual, buildManualCode(ENDPOINT, offer.code));

    assert.throws(() => store.redeem("NOTTHECODE", "3DS"), /not valid/);
    const first = store.redeem(formatPairingCode(offer.code), "Old 3DS");
    assert.match(first.device.deviceId, /^dev_[0-9a-f]{16}$/);
    assert.equal(first.device.name, "Old 3DS");
    assert.equal(store.deviceCount, 1);

    /* Replaying the same code must not mint a second credential. */
    assert.throws(() => store.redeem(offer.code, "3DS"), /not offering pairing/);

    store.createOffer(ENDPOINT, 60_000);
    clock.now += 60_001;
    assert.equal(store.activeOffer(), null);
    assert.throws(() => store.redeem(offer.code, "3DS"), /not offering pairing/);
  });
});

test("device tokens verify, persist and revoke", async () => {
  await withStore((store, path, clock) => {
    const offer = store.createOffer(ENDPOINT, 60_000);
    const {device, deviceToken} = store.redeem(offer.code, "New 3DS XL");
    assert.ok(deviceToken.length >= 40);

    assert.equal(store.verifyToken(deviceToken)?.deviceId, device.deviceId);
    assert.equal(store.verifyToken(`${deviceToken}x`), null);
    assert.equal(store.verifyToken(""), null);

    /* The clear token is never written to disk. */
    const reopened = new PairingStore({path, now: () => clock.now});
    assert.equal(reopened.deviceCount, 1);
    assert.equal(reopened.verifyToken(deviceToken)?.name, "New 3DS XL");
    assert.equal(reopened.list()[0]?.tokenHash.includes(deviceToken), false);

    /* Activity reaches disk, so another process can report it. */
    clock.now += 120_000;
    reopened.verifyToken(deviceToken);
    const seenBy = new PairingStore({path, now: () => clock.now});
    assert.equal(seenBy.list()[0]?.lastSeenAt, new Date(clock.now).toISOString());

    assert.equal(reopened.revoke("dev_absent"), false);
    assert.equal(reopened.revoke(device.deviceId), true);
    assert.equal(reopened.verifyToken(deviceToken), null);
    assert.equal(new PairingStore({path, now: () => clock.now}).deviceCount, 0);
  });
});

test("revoking from another process reaches a store that is already open", async () => {
  await withStore((serving, path, clock) => {
    const offer = serving.createOffer(ENDPOINT, 60_000);
    const {device, deviceToken} = serving.redeem(offer.code, "Old 3DS");
    assert.equal(serving.verifyToken(deviceToken)?.deviceId, device.deviceId);

    /* Stands in for `npm start -- --revoke-device` against a running bridge. */
    clock.now += 5_000;
    const cli = new PairingStore({path, now: () => clock.now});
    assert.equal(cli.revoke(device.deviceId), true);

    assert.equal(serving.verifyToken(deviceToken), null);
    assert.equal(serving.deviceCount, 0);
  });
});

test("pairing codes normalise and bridge names stay bounded", () => {
  assert.equal(normalizePairingCode("k7m2-qx4t-9bwf"), "K7M2QX4T9BWF");
  assert.equal(normalizePairingCode(" K7M2 QX4T 9BWF "), "K7M2QX4T9BWF");
  assert.equal(sanitizeBridgeName("Jm's MacBook Pro (work)"), "Jm-s-MacBook-Pro-wor");
  assert.equal(sanitizeBridgeName("!!!"), "bridge");
  assert.equal(sanitizeBridgeName(""), "bridge");
});

/* -------------------------------------------------------------- HTTP ---- */

interface TestBridge {
  baseUrl: URL;
  pushPort: number;
  store: PairingStore;
  close: () => Promise<void>;
}

async function startBridge(requirePairing: boolean): Promise<TestBridge> {
  const directory = await mkdtemp(join(tmpdir(), "3gent-pair-http-"));
  const store = new PairingStore({path: join(directory, "devices.json")});
  const {application, server} = createBridgeServer({
    capturePath: join(directory, "latest.wav"),
    photoPath: join(directory, "latest.bmp"),
    fakeDeltaIntervalMs: 5,
    logger: () => undefined,
    pairing: store,
    requirePairing,
  });
  const pushServer = createPushControlServer(application, {logger: () => undefined});
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  await new Promise<void>((resolve, reject) => {
    pushServer.once("error", reject);
    pushServer.listen(0, "127.0.0.1", resolve);
  });
  return {
    baseUrl: new URL(`http://127.0.0.1:${(server.address() as AddressInfo).port}`),
    pushPort: (pushServer.address() as AddressInfo).port,
    store,
    close: async () => {
      await application.shutdown();
      await new Promise<void>((resolve) => server.close(() => resolve()));
      await new Promise<void>((resolve) => pushServer.close(() => resolve()));
      await rm(directory, {recursive: true, force: true});
    },
  };
}

async function call(
  bridge: TestBridge,
  method: string,
  path: string,
  body: string,
  headers: Record<string, string> = {},
): Promise<{status: number; body: Record<string, unknown>}> {
  return await new Promise((resolve, reject) => {
    const outbound = httpRequest(
      new URL(path, bridge.baseUrl),
      {
        method,
        headers: {
          [PROTOCOL_HEADER]: String(PROTOCOL_VERSION),
          "content-type": "application/json",
          "content-length": Buffer.byteLength(body),
          ...headers,
        },
      },
      (response) => {
        const chunks: Buffer[] = [];
        response.on("data", (chunk: Buffer) => chunks.push(chunk));
        response.on("end", () => {
          const text = Buffer.concat(chunks).toString("utf8");
          resolve({
            status: response.statusCode ?? 0,
            body: text.length > 0
              ? JSON.parse(text) as Record<string, unknown>
              : {},
          });
        });
      },
    );
    outbound.on("error", reject);
    outbound.end(body);
  });
}

test("POST /v1/pair exchanges a code for a device token", async () => {
  const bridge = await startBridge(false);
  try {
    const offer = bridge.store.createOffer(ENDPOINT, 60_000);
    const rejected = await call(
      bridge,
      "POST",
      "/v1/pair",
      JSON.stringify({code: "WRONGCODE123", deviceName: "3DS"}),
    );
    assert.equal(rejected.status, 403);

    const accepted = await call(
      bridge,
      "POST",
      "/v1/pair",
      JSON.stringify({code: formatPairingCode(offer.code), deviceName: "Old 3DS"}),
    );
    assert.equal(accepted.status, 201);
    assert.equal(accepted.body.bridgeName, ENDPOINT.bridgeName);
    assert.deepEqual(accepted.body.endpoint, {
      host: ENDPOINT.host,
      httpPort: ENDPOINT.httpPort,
      pushPort: ENDPOINT.pushPort,
    });
    assert.equal(
      bridge.store.verifyToken(String(accepted.body.deviceToken))?.deviceId,
      accepted.body.deviceId,
    );

    const replayed = await call(
      bridge,
      "POST",
      "/v1/pair",
      JSON.stringify({code: offer.code, deviceName: "3DS"}),
    );
    assert.equal(replayed.status, 409);
  } finally {
    await bridge.close();
  }
});

test("--require-pairing gates every route except pairing itself", async () => {
  const bridge = await startBridge(true);
  try {
    const unauthorised = await call(bridge, "GET", "/v1/sessions?limit=1", "");
    assert.equal(unauthorised.status, 401);

    const offer = bridge.store.createOffer(ENDPOINT, 60_000);
    const paired = await call(
      bridge,
      "POST",
      "/v1/pair",
      JSON.stringify({code: offer.code, deviceName: "Old 3DS"}),
    );
    assert.equal(paired.status, 201);
    const token = String(paired.body.deviceToken);

    const badToken = await call(
      bridge,
      "GET",
      "/v1/sessions?limit=1",
      "",
      {authorization: "Bearer not-a-real-token"},
    );
    assert.equal(badToken.status, 401);

    const authorised = await call(
      bridge,
      "GET",
      "/v1/sessions?limit=1",
      "",
      {authorization: `Bearer ${token}`},
    );
    assert.equal(authorised.status, 200);
  } finally {
    await bridge.close();
  }
});

test("the pushed control link enforces the same device token", async () => {
  const bridge = await startBridge(true);
  const frames: Record<string, unknown>[] = [];
  const openHello = async (deviceToken?: string): Promise<Socket> => {
    const socket = connect(bridge.pushPort, "127.0.0.1");
    await new Promise<void>((resolve, reject) => {
      socket.once("connect", resolve);
      socket.once("error", reject);
    });
    socket.on("data", (chunk: Buffer) => {
      for (const line of chunk.toString("utf8").split("\n")) {
        if (line.length > 0) {
          frames.push(JSON.parse(line) as Record<string, unknown>);
        }
      }
    });
    socket.write(`${JSON.stringify({
      protocolVersion: PROTOCOL_VERSION,
      type: "connection.hello",
      sessionId: FAKE_SESSION_ID,
      after: 0,
      ...(deviceToken === undefined ? {} : {deviceToken}),
    })}\n`);
    return socket;
  };

  try {
    const anonymous = await openHello();
    await new Promise<void>((resolve) => anonymous.once("close", () => resolve()));
    assert.equal(frames.at(-1)?.type, "error");
    assert.equal(
      (frames.at(-1)?.error as Record<string, unknown>).code,
      "PAIRING_REQUIRED",
    );

    const offer = bridge.store.createOffer(ENDPOINT, 60_000);
    const paired = await call(
      bridge,
      "POST",
      "/v1/pair",
      JSON.stringify({code: offer.code, deviceName: "Old 3DS"}),
    );
    frames.length = 0;
    const authorised = await openHello(String(paired.body.deviceToken));
    await new Promise<void>((resolve) => setTimeout(resolve, 60));
    assert.equal(frames[0]?.type, "connection.ready");
    authorised.destroy();
  } finally {
    await bridge.close();
  }
});
