import { hostname, networkInterfaces } from "node:os";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import type { Server } from "node:net";
import type { Socket } from "node:net";

import type { AgentAdapter } from "./agent-adapter.js";
import { CodexAgentAdapter } from "./codex-agent-adapter.js";
import { EventStore } from "./event-store.js";
import { FakeAgentAdapter } from "./fake-agent-adapter.js";
import {
  DEFAULT_PAIRING_TTL_MS,
  formatPairingCode,
  PairingStore,
  sanitizeBridgeName,
  type PairingOffer,
} from "./pairing.js";
import { createPushControlServer } from "./push-server.js";
import { encodeQr } from "./qr-encode.js";
import { renderQrToSvg, renderQrToTerminal } from "./qr-render.js";
import { createBridgeServer } from "./server.js";
import { ReverseTunnelClient } from "./reverse-tunnel.js";
import {
  CommandTranscriber,
  OpenAiTranscriber,
  StaticTranscriber,
  type Transcriber,
} from "./transcriber.js";

interface Options {
  host: string;
  port: number;
  pushPort: number;
  capturePath: string;
  fakeDeltaIntervalMs: number;
  pushTestBlackhole: boolean;
  pushTestDropNextAck: boolean;
  verbose: boolean;
  verbosePolls: boolean;
  adapter: "fake" | "codex";
  codexExecutable: string;
  workspace: string;
  transcriber: "auto" | "none" | "openai" | "command";
  transcriptionCommand: string | null;
  transcriptionArguments: string[];
  transcriptionModel: string;
  relayHost: string | null;
  relayHttpUplinkPort: number;
  relayPushUplinkPort: number;
  pair: boolean;
  pairingTtlSeconds: number;
  requirePairing: boolean;
  devicesPath: string;
  pairingSvgPath: string;
  bridgeName: string;
  advertiseHost: string | null;
  listDevices: boolean;
  revokeDeviceId: string | null;
}

/*
 * The bind address is usually 0.0.0.0 or 127.0.0.1, neither of which a 3DS can
 * dial. Advertise the first non-internal IPv4 address instead, and let
 * --advertise-host override it for relay or multi-homed setups.
 */
function detectAdvertiseHost(bindHost: string): string {
  if (bindHost !== "0.0.0.0" && bindHost !== "::" && bindHost !== "127.0.0.1"
    && bindHost !== "localhost" && bindHost !== "::1") {
    return bindHost;
  }
  for (const addresses of Object.values(networkInterfaces())) {
    for (const address of addresses ?? []) {
      if (address.family === "IPv4" && !address.internal) {
        return address.address;
      }
    }
  }
  return "127.0.0.1";
}

function parseArguments(arguments_: string[]): Options {
  const options: Options = {
    host: "127.0.0.1",
    port: 8080,
    pushPort: 8081,
    capturePath: resolve("data", "latest.wav"),
    fakeDeltaIntervalMs: 80,
    pushTestBlackhole: false,
    pushTestDropNextAck: false,
    verbose: false,
    verbosePolls: false,
    adapter: "fake",
    codexExecutable: "codex",
    workspace: process.cwd(),
    transcriber: "auto",
    transcriptionCommand: null,
    transcriptionArguments: [],
    transcriptionModel: "gpt-4o-mini-transcribe",
    relayHost: null,
    relayHttpUplinkPort: 9180,
    relayPushUplinkPort: 9181,
    pair: false,
    pairingTtlSeconds: DEFAULT_PAIRING_TTL_MS / 1000,
    requirePairing: false,
    devicesPath: resolve("data", "devices.json"),
    pairingSvgPath: resolve("data", "pairing.svg"),
    bridgeName: sanitizeBridgeName(hostname().split(".")[0] ?? "bridge"),
    advertiseHost: null,
    listDevices: false,
    revokeDeviceId: null,
  };
  for (let index = 0; index < arguments_.length; index += 1) {
    const argument = arguments_[index];
    const value = arguments_[index + 1];
    if (argument === "--verbose") {
      options.verbose = true;
    } else if (argument === "--verbose-polls") {
      options.verbose = true;
      options.verbosePolls = true;
    } else if (argument === "--host" && value !== undefined) {
      options.host = value;
      index += 1;
    } else if (argument === "--port" && value !== undefined) {
      options.port = Number(value);
      index += 1;
    } else if (argument === "--push-port" && value !== undefined) {
      options.pushPort = Number(value);
      index += 1;
    } else if (argument === "--capture-path" && value !== undefined) {
      options.capturePath = resolve(value);
      index += 1;
    } else if (argument === "--fake-delta-ms" && value !== undefined) {
      options.fakeDeltaIntervalMs = Number(value);
      index += 1;
    } else if (argument === "--push-test-blackhole") {
      options.pushTestBlackhole = true;
    } else if (argument === "--push-test-drop-next-ack") {
      options.pushTestDropNextAck = true;
    } else if (argument === "--adapter" && (value === "fake" || value === "codex")) {
      options.adapter = value;
      index += 1;
    } else if (argument === "--codex-executable" && value !== undefined) {
      options.codexExecutable = value;
      index += 1;
    } else if (argument === "--workspace" && value !== undefined) {
      options.workspace = resolve(value);
      index += 1;
    } else if (argument === "--transcriber"
      && (value === "auto" || value === "none" || value === "openai" || value === "command")) {
      options.transcriber = value;
      index += 1;
    } else if (argument === "--transcription-command" && value !== undefined) {
      options.transcriptionCommand = value;
      index += 1;
    } else if (argument === "--transcription-arg" && value !== undefined) {
      options.transcriptionArguments.push(value);
      index += 1;
    } else if (argument === "--transcription-model" && value !== undefined) {
      options.transcriptionModel = value;
      index += 1;
    } else if (argument === "--relay-host" && value !== undefined) {
      options.relayHost = value;
      index += 1;
    } else if (argument === "--relay-http-uplink-port" && value !== undefined) {
      options.relayHttpUplinkPort = Number(value);
      index += 1;
    } else if (argument === "--relay-push-uplink-port" && value !== undefined) {
      options.relayPushUplinkPort = Number(value);
      index += 1;
    } else if (argument === "--pair") {
      options.pair = true;
    } else if (argument === "--pairing-ttl-seconds" && value !== undefined) {
      options.pairingTtlSeconds = Number(value);
      index += 1;
    } else if (argument === "--require-pairing") {
      options.requirePairing = true;
    } else if (argument === "--devices-path" && value !== undefined) {
      options.devicesPath = resolve(value);
      index += 1;
    } else if (argument === "--pairing-svg-path" && value !== undefined) {
      options.pairingSvgPath = resolve(value);
      index += 1;
    } else if (argument === "--bridge-name" && value !== undefined) {
      options.bridgeName = sanitizeBridgeName(value);
      index += 1;
    } else if (argument === "--advertise-host" && value !== undefined) {
      options.advertiseHost = value;
      index += 1;
    } else if (argument === "--list-devices") {
      options.listDevices = true;
    } else if (argument === "--revoke-device" && value !== undefined) {
      options.revokeDeviceId = value;
      index += 1;
    } else {
      throw new Error(`unknown or incomplete argument: ${argument ?? ""}`);
    }
  }
  if (!Number.isInteger(options.port) || options.port < 1 || options.port > 65_535) {
    throw new Error("--port must be an integer from 1 to 65535");
  }
  if (!Number.isInteger(options.pushPort) || options.pushPort < 1 || options.pushPort > 65_535) {
    throw new Error("--push-port must be an integer from 1 to 65535");
  }
  if (options.pushPort === options.port) {
    throw new Error("--push-port must differ from --port");
  }
  for (const [name, port] of [
    ["--relay-http-uplink-port", options.relayHttpUplinkPort],
    ["--relay-push-uplink-port", options.relayPushUplinkPort],
  ] as const) {
    if (!Number.isInteger(port) || port < 1 || port > 65_535) {
      throw new Error(`${name} must be an integer from 1 to 65535`);
    }
  }
  if (!Number.isInteger(options.fakeDeltaIntervalMs)
    || options.fakeDeltaIntervalMs < 1
    || options.fakeDeltaIntervalMs > 10_000) {
    throw new Error("--fake-delta-ms must be an integer from 1 to 10000");
  }
  if (!Number.isInteger(options.pairingTtlSeconds)
    || options.pairingTtlSeconds < 30
    || options.pairingTtlSeconds > 3_600) {
    throw new Error("--pairing-ttl-seconds must be an integer from 30 to 3600");
  }
  return options;
}

const options = parseArguments(process.argv.slice(2));
const pairing = new PairingStore({path: options.devicesPath});

if (options.listDevices || options.revokeDeviceId !== null) {
  if (options.revokeDeviceId !== null) {
    const revoked = pairing.revoke(options.revokeDeviceId);
    console.log(revoked
      ? `Revoked ${options.revokeDeviceId}.`
      : `No paired device with ID ${options.revokeDeviceId}.`);
    if (!revoked) {
      process.exit(1);
    }
  }
  const devices = pairing.list();
  if (devices.length === 0) {
    console.log(`No paired devices in ${pairing.path}.`);
  } else {
    console.log(`Paired devices in ${pairing.path}:`);
    for (const device of devices) {
      console.log(
        `  ${device.deviceId}  ${device.name}  paired ${device.pairedAt}`
        + `  last seen ${device.lastSeenAt ?? "never"}`,
      );
    }
  }
  process.exit(0);
}

/*
 * Show a pairing code when asked, and on a first run with nothing paired yet:
 * a bridge no handheld knows about has nothing else useful to offer.
 */
function openPairingOffer(): PairingOffer {
  const offer = pairing.createOffer(
    {
      host: options.advertiseHost ?? detectAdvertiseHost(options.host),
      httpPort: options.port,
      pushPort: options.pushPort,
      bridgeName: options.bridgeName,
    },
    options.pairingTtlSeconds * 1_000,
  );
  const matrix = encodeQr(offer.url);
  let svgNote = "";
  try {
    mkdirSync(dirname(options.pairingSvgPath), {recursive: true});
    writeFileSync(options.pairingSvgPath, renderQrToSvg(matrix), "utf8");
    svgNote = `\nEasier to scan full screen: open ${options.pairingSvgPath}`;
  } catch (error) {
    svgNote = `\nCould not write ${options.pairingSvgPath}: `
      + `${error instanceof Error ? error.message : "unknown error"}`;
  }
  console.log(
    `\nPair a 3DS with this bridge (valid for ${options.pairingTtlSeconds}s):\n\n`
    + `${renderQrToTerminal(matrix)}\n\n`
    + `Manual entry:  ${offer.endpoint.host} ${offer.endpoint.httpPort} `
    + `${offer.endpoint.pushPort} ${formatPairingCode(offer.code)}\n`
    + `QR payload:    ${offer.url} (v${matrix.version}, ${matrix.size}x${matrix.size})`
    + `${svgNote}\n`,
  );
  return offer;
}

const events = new EventStore();
let adapter: AgentAdapter;
if (options.adapter === "codex") {
  console.log("Starting local Codex app-server adapter...");
  adapter = await CodexAgentAdapter.create({
    events,
    executable: options.codexExecutable,
    logger: options.verbose ? console.log : () => {},
  });
} else {
  adapter = new FakeAgentAdapter(events, options.fakeDeltaIntervalMs);
}
let transcriber: Transcriber | undefined;
const transcriberChoice = options.transcriber === "auto"
  ? options.adapter === "fake" ? "mock" : "none"
  : options.transcriber;
if (transcriberChoice === "mock") {
  transcriber = new StaticTranscriber();
} else if (transcriberChoice === "openai") {
  const apiKey = process.env.OPENAI_API_KEY;
  if (apiKey === undefined || apiKey.length === 0) {
    throw new Error("OPENAI_API_KEY is required for --transcriber openai");
  }
  transcriber = new OpenAiTranscriber({
    apiKey,
    model: options.transcriptionModel,
  });
} else if (transcriberChoice === "command") {
  if (options.transcriptionCommand === null) {
    throw new Error("--transcription-command is required for --transcriber command");
  }
  transcriber = new CommandTranscriber({
    executable: options.transcriptionCommand,
    arguments: options.transcriptionArguments,
  });
}
const {application, server} = createBridgeServer({
  adapter,
  events,
  capturePath: options.capturePath,
  defaultCwd: options.workspace,
  ...(transcriber === undefined ? {} : {transcriber}),
  verbose: options.verbose,
  verbosePolls: options.verbosePolls,
  pairing,
  requirePairing: options.requirePairing,
});
const pushServer = createPushControlServer(application, {
  blackholeAfterReady: options.pushTestBlackhole,
  dropNextAcknowledgementAfterExecution: options.pushTestDropNextAck,
  verbose: options.verbose,
  verboseHeartbeats: options.verbosePolls,
});
const activeSockets = new Set<Socket>();
for (const listener of [server, pushServer]) {
  listener.on("connection", (socket: Socket) => {
    activeSockets.add(socket);
    socket.once("close", () => activeSockets.delete(socket));
  });
}
let reverseTunnel: ReverseTunnelClient | null = null;
if (options.relayHost !== null) {
  const token = process.env.THREEGENT_RELAY_TOKEN;
  if (token === undefined) {
    throw new Error("THREEGENT_RELAY_TOKEN is required with --relay-host");
  }
  reverseTunnel = new ReverseTunnelClient({
    relayHost: options.relayHost,
    relayHttpUplinkPort: options.relayHttpUplinkPort,
    relayPushUplinkPort: options.relayPushUplinkPort,
    token,
    localHttpPort: options.port,
    localPushPort: options.pushPort,
    logger: options.verbose ? console.log : () => {},
  });
}

server.listen(options.port, options.host, () => {
  pushServer.listen(options.pushPort, options.host);
});

pushServer.on("listening", () => {
  reverseTunnel?.start();
  console.log(
    `3gent 0.8 pairing bridge listening on http://${options.host}:${options.port}\n`
    + `Pushed control: tcp://${options.host}:${options.pushPort}\n`
    + `Bridge name: ${options.bridgeName}\n`
    + `Adapter: ${options.adapter === "codex" ? "Codex app-server" : "deterministic fake agent"}\n`
    + `Transcriber: ${transcriber?.id ?? "not configured"}\n`
    + `Relay: ${options.relayHost ?? "disabled"}\n`
    + `Audio capture: ${options.capturePath}\n`
    + `Paired devices: ${pairing.deviceCount} in ${pairing.path}\n`
    + `Device tokens: ${options.requirePairing ? "REQUIRED" : "accepted but not required"}\n`
    + `Logging: ${options.verbose
      ? `VERBOSE (${options.verbosePolls ? "including empty polls" : "empty polls hidden"})`
      : "summary"}\n`
    + (options.verbose
      ? "WARNING: verbose logs may contain sensitive prompt and agent content.\n"
      : "")
    + (options.pushTestBlackhole || options.pushTestDropNextAck
      ? "WARNING: PUSH FAULT INJECTION IS ENABLED FOR HARDWARE TESTING.\n"
      : "")
    + "LOCAL DEVELOPMENT ONLY: this link is still plaintext and unencrypted.",
  );
  if (options.pair || pairing.deviceCount === 0) {
    openPairingOffer();
  } else {
    console.log("Run with --pair to show a pairing QR code for another handheld.");
  }
});

let shuttingDown = false;
async function shutdown(): Promise<void> {
  if (shuttingDown) {
    return;
  }
  shuttingDown = true;
  const listenersClosed = Promise.all([
    closeListener(server),
    closeListener(pushServer),
  ]);
  for (const socket of activeSockets) {
    socket.destroy();
  }
  activeSockets.clear();
  await reverseTunnel?.close();
  await application.shutdown();
  await listenersClosed;
  process.exit(0);
}

async function closeListener(listener: Server): Promise<void> {
  await new Promise<void>((resolveClose) => listener.close(() => resolveClose()));
}

process.on("SIGINT", () => void shutdown());
process.on("SIGTERM", () => void shutdown());
