import { timingSafeEqual } from "node:crypto";
import { createServer, type AddressInfo, type Server, type Socket } from "node:net";

export type RelayChannel = "http" | "push";

export interface RelayOptions {
  host: string;
  publicHttpPort: number;
  publicPushPort: number;
  uplinkHttpPort: number;
  uplinkPushPort: number;
  token: string;
  maximumWaitingPerChannel?: number;
  waitingTimeoutMs?: number;
  logger?: (message: string) => void;
}

interface WaitingSocket {
  socket: Socket;
  timer: NodeJS.Timeout;
}

export class RelayService {
  readonly #options: RelayOptions;
  readonly #servers: Server[] = [];
  readonly #uplinks = new Map<RelayChannel, WaitingSocket[]>([["http", []], ["push", []]]);
  readonly #clients = new Map<RelayChannel, WaitingSocket[]>([["http", []], ["push", []]]);
  readonly #logger: (message: string) => void;
  readonly #activeSockets = new Set<Socket>();
  #closed = false;

  public constructor(options: RelayOptions) {
    if (options.token.length < 16 || Buffer.byteLength(options.token) > 256) {
      throw new RangeError("relay token must contain 16 to 256 bytes");
    }
    this.#options = options;
    this.#logger = options.logger ?? console.log;
  }

  public async listen(): Promise<void> {
    if (this.#servers.length > 0) {
      throw new Error("relay is already listening");
    }
    const definitions: Array<{port: number; role: "public" | "uplink"; channel: RelayChannel}> = [
      {port: this.#options.publicHttpPort, role: "public", channel: "http"},
      {port: this.#options.publicPushPort, role: "public", channel: "push"},
      {port: this.#options.uplinkHttpPort, role: "uplink", channel: "http"},
      {port: this.#options.uplinkPushPort, role: "uplink", channel: "push"},
    ];
    for (const definition of definitions) {
      const server = createServer((socket) => {
        socket.setNoDelay(true);
        socket.setKeepAlive(true, 5_000);
        if (definition.role === "uplink") {
          this.#acceptUplink(definition.channel, socket);
        } else {
          this.#queue(definition.channel, socket, this.#clients, "public client");
        }
      });
      this.#servers.push(server);
      await new Promise<void>((resolve, reject) => {
        server.once("error", reject);
        server.listen(definition.port, this.#options.host, resolve);
      });
    }
  }

  public async close(): Promise<void> {
    if (this.#closed) {
      return;
    }
    this.#closed = true;
    for (const queues of [this.#uplinks, this.#clients]) {
      for (const waiting of queues.values()) {
        for (const entry of waiting) {
          clearTimeout(entry.timer);
          entry.socket.destroy();
        }
        waiting.length = 0;
      }
    }
    for (const socket of this.#activeSockets) {
      socket.destroy();
    }
    this.#activeSockets.clear();
    await Promise.all(this.#servers.map(async (server) => await new Promise<void>((resolve) => {
      server.close(() => resolve());
    })));
    this.#servers.length = 0;
  }

  public ports(): {
    publicHttp: number;
    publicPush: number;
    uplinkHttp: number;
    uplinkPush: number;
  } {
    if (this.#servers.length !== 4) {
      throw new Error("relay is not listening");
    }
    const values = this.#servers.map((server) => (server.address() as AddressInfo).port);
    return {
      publicHttp: values[0]!,
      publicPush: values[1]!,
      uplinkHttp: values[2]!,
      uplinkPush: values[3]!,
    };
  }

  #acceptUplink(channel: RelayChannel, socket: Socket): void {
    let input = Buffer.alloc(0);
    const timer = setTimeout(() => socket.destroy(), 5_000);
    const onData = (chunk: Buffer) => {
      input = Buffer.concat([input, chunk]);
      if (input.byteLength > 512) {
        socket.destroy();
        return;
      }
      const newline = input.indexOf(0x0a);
      if (newline < 0) {
        return;
      }
      socket.off("data", onData);
      clearTimeout(timer);
      const line = input.subarray(0, newline).toString("utf8");
      const remainder = input.subarray(newline + 1);
      let parsed: unknown;
      try {
        parsed = JSON.parse(line);
      } catch {
        socket.destroy();
        return;
      }
      if (!isRecord(parsed) || parsed.role !== "bridge" || parsed.channel !== channel
        || typeof parsed.token !== "string" || !safeEqual(parsed.token, this.#options.token)
        || remainder.length > 0) {
        socket.destroy();
        return;
      }
      this.#queue(channel, socket, this.#uplinks, "bridge uplink");
    };
    socket.on("data", onData);
    socket.once("close", () => clearTimeout(timer));
  }

  #queue(
    channel: RelayChannel,
    socket: Socket,
    queues: Map<RelayChannel, WaitingSocket[]>,
    kind: string,
  ): void {
    if (this.#closed) {
      socket.destroy();
      return;
    }
    const queue = queues.get(channel)!;
    const maximum = this.#options.maximumWaitingPerChannel ?? 8;
    if (queue.length >= maximum) {
      this.#logger(`relay rejected excess ${kind}: channel=${channel}`);
      socket.destroy();
      return;
    }
    const entry: WaitingSocket = {
      socket,
      timer: setTimeout(() => {
        removeEntry(queue, entry);
        socket.destroy();
      }, this.#options.waitingTimeoutMs ?? 15_000),
    };
    entry.timer.unref();
    queue.push(entry);
    socket.once("close", () => {
      clearTimeout(entry.timer);
      removeEntry(queue, entry);
    });
    this.#tryPair(channel);
  }

  #tryPair(channel: RelayChannel): void {
    const uplink = this.#takeLive(this.#uplinks.get(channel)!);
    const client = this.#takeLive(this.#clients.get(channel)!);
    if (uplink === null || client === null) {
      if (uplink !== null) {
        this.#uplinks.get(channel)!.unshift(uplink);
      }
      if (client !== null) {
        this.#clients.get(channel)!.unshift(client);
      }
      return;
    }
    clearTimeout(uplink.timer);
    clearTimeout(client.timer);
    this.#logger(`relay pairing: channel=${channel}`);
    uplink.socket.write("ready\n");
    this.#waitForConnected(channel, uplink.socket, client.socket);
  }

  #waitForConnected(channel: RelayChannel, uplink: Socket, client: Socket): void {
    let input = Buffer.alloc(0);
    const timer = setTimeout(() => {
      uplink.destroy();
      client.destroy();
    }, 5_000);
    const onData = (chunk: Buffer) => {
      input = Buffer.concat([input, chunk]);
      if (input.byteLength > 64) {
        uplink.destroy();
        client.destroy();
        return;
      }
      const newline = input.indexOf(0x0a);
      if (newline < 0) {
        return;
      }
      uplink.off("data", onData);
      clearTimeout(timer);
      if (input.subarray(0, newline).toString("utf8") !== "connected") {
        uplink.destroy();
        client.destroy();
        return;
      }
      const remainder = input.subarray(newline + 1);
      if (remainder.length > 0) {
        client.write(remainder);
      }
      uplink.pipe(client);
      client.pipe(uplink);
      this.#activeSockets.add(uplink);
      this.#activeSockets.add(client);
      const closeBoth = () => {
        this.#activeSockets.delete(uplink);
        this.#activeSockets.delete(client);
        uplink.destroy();
        client.destroy();
      };
      uplink.once("error", closeBoth);
      client.once("error", closeBoth);
      uplink.once("close", closeBoth);
      client.once("close", closeBoth);
      this.#logger(`relay active: channel=${channel}`);
    };
    uplink.on("data", onData);
  }

  #takeLive(queue: WaitingSocket[]): WaitingSocket | null {
    for (;;) {
      const entry = queue.shift();
      if (entry === undefined) {
        return null;
      }
      if (!entry.socket.destroyed) {
        return entry;
      }
      clearTimeout(entry.timer);
    }
  }
}

function removeEntry(queue: WaitingSocket[], entry: WaitingSocket): void {
  const index = queue.indexOf(entry);
  if (index >= 0) {
    queue.splice(index, 1);
  }
}

function safeEqual(left: string, right: string): boolean {
  const a = Buffer.from(left);
  const b = Buffer.from(right);
  return a.length === b.length && timingSafeEqual(a, b);
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
