import { connect, type Socket } from "node:net";

import type { RelayChannel } from "./relay.js";

export interface ReverseTunnelOptions {
  relayHost: string;
  relayHttpUplinkPort: number;
  relayPushUplinkPort: number;
  token: string;
  localHost?: string;
  localHttpPort: number;
  localPushPort: number;
  httpPoolSize?: number;
  logger?: (message: string) => void;
}

interface TunnelSlot {
  channel: RelayChannel;
  stopped: boolean;
  relay: Socket | null;
  local: Socket | null;
  retryMs: number;
  timer: NodeJS.Timeout | null;
}

export class ReverseTunnelClient {
  readonly #options: ReverseTunnelOptions;
  readonly #slots: TunnelSlot[] = [];
  readonly #logger: (message: string) => void;
  #started = false;

  public constructor(options: ReverseTunnelOptions) {
    if (options.token.length < 16 || Buffer.byteLength(options.token) > 256) {
      throw new RangeError("relay token must contain 16 to 256 bytes");
    }
    this.#options = options;
    this.#logger = options.logger ?? console.log;
  }

  public start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    const httpPoolSize = this.#options.httpPoolSize ?? 4;
    for (let index = 0; index < httpPoolSize; index += 1) {
      this.#startSlot("http");
    }
    this.#startSlot("push");
  }

  public async close(): Promise<void> {
    for (const slot of this.#slots) {
      slot.stopped = true;
      if (slot.timer !== null) {
        clearTimeout(slot.timer);
        slot.timer = null;
      }
      slot.relay?.destroy();
      slot.local?.destroy();
    }
    this.#slots.length = 0;
    this.#started = false;
  }

  #startSlot(channel: RelayChannel): void {
    const slot: TunnelSlot = {
      channel,
      stopped: false,
      relay: null,
      local: null,
      retryMs: 250,
      timer: null,
    };
    this.#slots.push(slot);
    this.#connectRelay(slot);
  }

  #connectRelay(slot: TunnelSlot): void {
    if (slot.stopped) {
      return;
    }
    const port = slot.channel === "http"
      ? this.#options.relayHttpUplinkPort
      : this.#options.relayPushUplinkPort;
    const relay = connect({host: this.#options.relayHost, port});
    slot.relay = relay;
    relay.setNoDelay(true);
    relay.setKeepAlive(true, 5_000);
    let input = Buffer.alloc(0);
    let activated = false;
    const onData = (chunk: Buffer) => {
      input = Buffer.concat([input, chunk]);
      if (input.byteLength > 8 * 1024) {
        relay.destroy();
        return;
      }
      const newline = input.indexOf(0x0a);
      if (newline < 0) {
        return;
      }
      relay.off("data", onData);
      if (input.subarray(0, newline).toString("utf8") !== "ready") {
        relay.destroy();
        return;
      }
      const remainder = input.subarray(newline + 1);
      activated = true;
      this.#connectLocal(slot, relay, remainder);
    };
    relay.on("data", onData);
    relay.once("connect", () => {
      relay.write(`${JSON.stringify({
        role: "bridge",
        channel: slot.channel,
        token: this.#options.token,
      })}\n`);
      slot.retryMs = 250;
    });
    relay.once("error", (error) => {
      this.#logger(`relay ${slot.channel} uplink error: ${error.message}`);
    });
    relay.once("close", () => {
      if (!activated) {
        this.#scheduleReconnect(slot);
      }
    });
  }

  #connectLocal(slot: TunnelSlot, relay: Socket, initial: Buffer): void {
    const port = slot.channel === "http"
      ? this.#options.localHttpPort
      : this.#options.localPushPort;
    const local = connect({host: this.#options.localHost ?? "127.0.0.1", port});
    slot.local = local;
    local.setNoDelay(true);
    const closeBoth = () => {
      relay.destroy();
      local.destroy();
    };
    local.once("connect", () => {
      relay.write("connected\n");
      if (initial.length > 0) {
        local.write(initial);
      }
      relay.pipe(local);
      local.pipe(relay);
      this.#logger(`relay ${slot.channel} tunnel active`);
    });
    local.once("error", (error) => {
      this.#logger(`relay local ${slot.channel} error: ${error.message}`);
      closeBoth();
    });
    local.once("close", () => {
      closeBoth();
      this.#scheduleReconnect(slot);
    });
    relay.once("close", () => {
      closeBoth();
      this.#scheduleReconnect(slot);
    });
  }

  #scheduleReconnect(slot: TunnelSlot): void {
    if (slot.stopped || slot.timer !== null) {
      return;
    }
    slot.relay = null;
    slot.local = null;
    const delay = slot.retryMs;
    slot.retryMs = Math.min(slot.retryMs * 2, 10_000);
    slot.timer = setTimeout(() => {
      slot.timer = null;
      this.#connectRelay(slot);
    }, delay);
    slot.timer.unref();
  }
}
