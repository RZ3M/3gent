import { once } from "node:events";
import { connect, createServer, type Server, type Socket } from "node:net";
import type { AddressInfo } from "node:net";
import assert from "node:assert/strict";
import test from "node:test";

import { RelayService } from "../src/relay.js";
import { ReverseTunnelClient } from "../src/reverse-tunnel.js";
import { createBridgeServer } from "../src/server.js";
import { createPushControlServer } from "../src/push-server.js";
import { FAKE_SESSION_ID } from "../src/fake-agent.js";

test("self-host relay carries repeated HTTP and bidirectional push sockets through outbound bridge pools", async () => {
  const localHttp = await echoServer();
  const localPush = await echoServer();
  const relay = new RelayService({
    host: "127.0.0.1",
    publicHttpPort: 0,
    publicPushPort: 0,
    uplinkHttpPort: 0,
    uplinkPushPort: 0,
    token: "test-token-at-least-16-bytes",
    logger: () => {},
  });
  await relay.listen();
  const ports = relay.ports();
  const tunnel = new ReverseTunnelClient({
    relayHost: "127.0.0.1",
    relayHttpUplinkPort: ports.uplinkHttp,
    relayPushUplinkPort: ports.uplinkPush,
    token: "test-token-at-least-16-bytes",
    localHttpPort: port(localHttp),
    localPushPort: port(localPush),
    httpPoolSize: 2,
    logger: () => {},
  });
  tunnel.start();
  try {
    assert.equal(await roundTrip(ports.publicHttp, "first-http"), "first-http");
    assert.equal(await roundTrip(ports.publicHttp, "second-http"), "second-http");
    assert.equal(await roundTrip(ports.publicPush, "push-both-ways"), "push-both-ways");
    await tunnel.close();
    tunnel.start();
    assert.equal(await roundTrip(ports.publicHttp, "after-restart"), "after-restart");
  } finally {
    await tunnel.close();
    await relay.close();
    await closeServer(localHttp);
    await closeServer(localPush);
  }
});

test("relays the real bridge health route and pushed session handshake", async () => {
  const {application, server: httpServer} = createBridgeServer({logger: () => {}});
  const pushServer = createPushControlServer(application, {logger: () => {}});
  await listen(httpServer);
  await listen(pushServer);
  const relay = new RelayService({
    host: "127.0.0.1",
    publicHttpPort: 0,
    publicPushPort: 0,
    uplinkHttpPort: 0,
    uplinkPushPort: 0,
    token: "integration-token-16-bytes",
    logger: () => {},
  });
  await relay.listen();
  const ports = relay.ports();
  const tunnel = new ReverseTunnelClient({
    relayHost: "127.0.0.1",
    relayHttpUplinkPort: ports.uplinkHttp,
    relayPushUplinkPort: ports.uplinkPush,
    token: "integration-token-16-bytes",
    localHttpPort: port(httpServer),
    localPushPort: port(pushServer),
    httpPoolSize: 2,
    logger: () => {},
  });
  tunnel.start();
  try {
    const health = await fetch(`http://127.0.0.1:${ports.publicHttp}/health`);
    assert.equal(health.status, 200);
    assert.equal((await health.json() as {status: string}).status, "ready");

    const socket = connect({host: "127.0.0.1", port: ports.publicPush});
    await once(socket, "connect");
    socket.write(`${JSON.stringify({
      protocolVersion: 1,
      type: "connection.hello",
      sessionId: FAKE_SESSION_ID,
      after: 0,
    })}\n`);
    const [chunk] = await once(socket, "data") as [Buffer];
    assert.match(chunk.toString("utf8"), /"type":"connection.ready"/);
    socket.destroy();
  } finally {
    await tunnel.close();
    await relay.close();
    await application.shutdown();
    await closeServer(httpServer);
    await closeServer(pushServer);
  }
});

async function echoServer(): Promise<Server> {
  const server = createServer((socket) => socket.pipe(socket));
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  return server;
}

async function listen(server: Server): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
}

async function roundTrip(targetPort: number, message: string): Promise<string> {
  const socket: Socket = connect({host: "127.0.0.1", port: targetPort});
  await once(socket, "connect");
  socket.write(message);
  const [chunk] = await once(socket, "data") as [Buffer];
  socket.destroy();
  return chunk.toString("utf8");
}

function port(server: Server): number {
  return (server.address() as AddressInfo).port;
}

async function closeServer(server: Server): Promise<void> {
  await new Promise<void>((resolve) => server.close(() => resolve()));
}
