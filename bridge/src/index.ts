import { resolve } from "node:path";

import { createBridgeServer } from "./server.js";

interface Options {
  host: string;
  port: number;
  capturePath: string;
  verbose: boolean;
}

function parseArguments(arguments_: string[]): Options {
  const options: Options = {
    host: "127.0.0.1",
    port: 8080,
    capturePath: resolve("data", "latest.wav"),
    verbose: false,
  };
  for (let index = 0; index < arguments_.length; index += 1) {
    const argument = arguments_[index];
    const value = arguments_[index + 1];
    if (argument === "--verbose") {
      options.verbose = true;
    } else if (argument === "--host" && value !== undefined) {
      options.host = value;
      index += 1;
    } else if (argument === "--port" && value !== undefined) {
      options.port = Number(value);
      index += 1;
    } else if (argument === "--capture-path" && value !== undefined) {
      options.capturePath = resolve(value);
      index += 1;
    } else {
      throw new Error(`unknown or incomplete argument: ${argument ?? ""}`);
    }
  }
  if (!Number.isInteger(options.port) || options.port < 1 || options.port > 65_535) {
    throw new Error("--port must be an integer from 1 to 65535");
  }
  return options;
}

const options = parseArguments(process.argv.slice(2));
const {application, server} = createBridgeServer({
  capturePath: options.capturePath,
  verbose: options.verbose,
});

server.listen(options.port, options.host, () => {
  console.log(
    `3gent Stage 1 bridge listening on http://${options.host}:${options.port}\n`
    + "Adapter: deterministic fake agent\n"
    + `Audio capture: ${options.capturePath}\n`
    + `Logging: ${options.verbose ? "VERBOSE (full text and protocol payloads)" : "summary"}\n`
    + (options.verbose
      ? "WARNING: verbose logs may contain sensitive prompt and agent content.\n"
      : "")
    + "LOCAL DEVELOPMENT ONLY: pairing and authentication are not implemented.",
  );
});

function shutdown(): void {
  application.shutdown();
  server.close(() => process.exit(0));
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
