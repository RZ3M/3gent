import { RelayService } from "./relay.js";

interface Options {
  host: string;
  publicHttpPort: number;
  publicPushPort: number;
  uplinkHttpPort: number;
  uplinkPushPort: number;
  unsafePublic: boolean;
}

const options: Options = {
  host: "127.0.0.1",
  publicHttpPort: 9080,
  publicPushPort: 9081,
  uplinkHttpPort: 9180,
  uplinkPushPort: 9181,
  unsafePublic: false,
};
const arguments_ = process.argv.slice(2);
for (let index = 0; index < arguments_.length; index += 1) {
  const argument = arguments_[index];
  const value = arguments_[index + 1];
  if (argument === "--unsafe-public") {
    options.unsafePublic = true;
  } else if (argument === "--host" && value !== undefined) {
    options.host = value;
    index += 1;
  } else if (argument === "--public-http-port" && value !== undefined) {
    options.publicHttpPort = Number(value);
    index += 1;
  } else if (argument === "--public-push-port" && value !== undefined) {
    options.publicPushPort = Number(value);
    index += 1;
  } else if (argument === "--uplink-http-port" && value !== undefined) {
    options.uplinkHttpPort = Number(value);
    index += 1;
  } else if (argument === "--uplink-push-port" && value !== undefined) {
    options.uplinkPushPort = Number(value);
    index += 1;
  } else {
    throw new Error(`unknown or incomplete relay argument: ${argument ?? ""}`);
  }
}
for (const [name, value] of Object.entries(options).filter(([, value]) => typeof value === "number")) {
  if (!Number.isInteger(value) || value < 1 || value > 65_535) {
    throw new Error(`${name} must be an integer from 1 to 65535`);
  }
}
if (new Set([
  options.publicHttpPort,
  options.publicPushPort,
  options.uplinkHttpPort,
  options.uplinkPushPort,
]).size !== 4) {
  throw new Error("all four relay ports must differ");
}
if (!options.unsafePublic) {
  throw new Error("the current relay is plaintext; pass --unsafe-public only for deliberate testing");
}
const token = process.env.THREEGENT_RELAY_TOKEN;
if (token === undefined) {
  throw new Error("THREEGENT_RELAY_TOKEN is required");
}

const relay = new RelayService({...options, token});
await relay.listen();
console.log(
  `3gent self-host relay listening on ${options.host}\n`
  + `3DS HTTP/media: ${options.publicHttpPort}\n`
  + `3DS pushed control: ${options.publicPushPort}\n`
  + `Bridge HTTP uplinks: ${options.uplinkHttpPort}\n`
  + `Bridge push uplink: ${options.uplinkPushPort}\n`
  + "WARNING: 3DS-facing links are unauthenticated plaintext. Do not expose this beyond a deliberate test environment.",
);

let closing = false;
const shutdown = async () => {
  if (closing) {
    return;
  }
  closing = true;
  await relay.close();
  process.exit(0);
};
process.on("SIGINT", () => void shutdown());
process.on("SIGTERM", () => void shutdown());
