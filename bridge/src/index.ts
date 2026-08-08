import { resolve } from "node:path";

import { createPushControlServer } from "./push-server.js";
import { createBridgeServer } from "./server.js";

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
  if (!Number.isInteger(options.fakeDeltaIntervalMs)
    || options.fakeDeltaIntervalMs < 1
    || options.fakeDeltaIntervalMs > 10_000) {
    throw new Error("--fake-delta-ms must be an integer from 1 to 10000");
  }
  return options;
}

const options = parseArguments(process.argv.slice(2));
const {application, server} = createBridgeServer({
  capturePath: options.capturePath,
  fakeDeltaIntervalMs: options.fakeDeltaIntervalMs,
  verbose: options.verbose,
  verbosePolls: options.verbosePolls,
});
const pushServer = createPushControlServer(application, {
  blackholeAfterReady: options.pushTestBlackhole,
  dropNextAcknowledgementAfterExecution: options.pushTestDropNextAck,
  verbose: options.verbose,
  verboseHeartbeats: options.verbosePolls,
});

server.listen(options.port, options.host, () => {
  pushServer.listen(options.pushPort, options.host);
});

pushServer.on("listening", () => {
  console.log(
    `3gent Stage 1.5 bridge listening on http://${options.host}:${options.port}\n`
    + `Pushed control: tcp://${options.host}:${options.pushPort}\n`
    + "Adapter: deterministic fake agent\n"
    + `Audio capture: ${options.capturePath}\n`
    + `Logging: ${options.verbose
      ? `VERBOSE (${options.verbosePolls ? "including empty polls" : "empty polls hidden"})`
      : "summary"}\n`
    + (options.verbose
      ? "WARNING: verbose logs may contain sensitive prompt and agent content.\n"
      : "")
    + (options.pushTestBlackhole || options.pushTestDropNextAck
      ? "WARNING: PUSH FAULT INJECTION IS ENABLED FOR HARDWARE TESTING.\n"
      : "")
    + "LOCAL DEVELOPMENT ONLY: pairing and authentication are not implemented.",
  );
});

function shutdown(): void {
  application.shutdown();
  let remaining = 2;
  const closed = () => {
    remaining -= 1;
    if (remaining === 0) {
      process.exit(0);
    }
  };
  server.close(closed);
  pushServer.close(closed);
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
