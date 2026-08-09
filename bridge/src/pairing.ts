/*
 * Pairing bootstrap and device credentials (D-010, ADR-0001, D-022).
 *
 * The QR code carries no credential. It carries an endpoint plus a short-lived,
 * single-use pairing code. The 3DS exchanges that code for a device token over
 * `POST /v1/pair`; the bridge keeps only a SHA-256 hash of the token, so a
 * stolen `devices.json` does not yield a usable credential.
 *
 * Token *enforcement* is opt-in (`--require-pairing`) while the transport is
 * still plaintext. See `docs/SECURITY.md` §4: a bearer token on an
 * unauthenticated plaintext link is an identity, not a security boundary, and
 * pretending otherwise by defaulting it on would be worse than saying so.
 */
import { createHash, randomBytes, randomInt, timingSafeEqual } from "node:crypto";
import { mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";

import { ProtocolError } from "./protocol.js";

export const PAIRING_URL_SCHEME = "3gent://pair";
export const PAIRING_PAYLOAD_VERSION = 1;
export const DEFAULT_PAIRING_TTL_MS = 180_000;
export const PAIRING_CODE_LENGTH = 12;
export const MAX_DEVICE_NAME_BYTES = 48;
export const MAX_BRIDGE_NAME_LENGTH = 20;

/* Digits and letters that survive a small, low-contrast handheld font. */
const CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTVWXYZ";

export interface PairedDevice {
  deviceId: string;
  name: string;
  tokenHash: string;
  pairedAt: string;
  lastSeenAt: string | null;
}

export interface PairingEndpoint {
  host: string;
  httpPort: number;
  pushPort: number;
  bridgeName: string;
}

export interface PairingOffer {
  code: string;
  expiresAt: number;
  /** Exact bytes encoded into the QR code. */
  url: string;
  /** Typed fallback, printed beside the QR (`docs/PROTOCOL.md`). */
  manual: string;
  endpoint: PairingEndpoint;
}

export interface RedeemedPairing {
  device: PairedDevice;
  /** Returned to the handheld once and never stored in the clear. */
  deviceToken: string;
  /** Echoed back so the handheld can correct a mistyped manual endpoint. */
  endpoint: PairingEndpoint;
}

interface PersistedState {
  version: 1;
  devices: PairedDevice[];
}

export function hashToken(token: string): string {
  return createHash("sha256").update(token, "utf8").digest("hex");
}

export function formatPairingCode(code: string): string {
  return (code.match(/.{1,4}/g) ?? [code]).join("-");
}

/** Uppercases and drops separators, so a typed code may be grouped or not. */
export function normalizePairingCode(raw: string): string {
  return raw.toUpperCase().replace(/[^0-9A-Z]/g, "");
}

function generatePairingCode(): string {
  let code = "";
  for (let index = 0; index < PAIRING_CODE_LENGTH; index += 1) {
    code += CODE_ALPHABET[randomInt(CODE_ALPHABET.length)] ?? "2";
  }
  return code;
}

/** Bounded to keep the QR small enough for a 400x240 sensor to resolve. */
export function sanitizeBridgeName(raw: string): string {
  const cleaned = raw
    .replace(/[^0-9A-Za-z._-]/g, "-")
    .replace(/-+/g, "-")
    .slice(0, MAX_BRIDGE_NAME_LENGTH)
    .replace(/^-+|-+$/g, "");
  return cleaned.length > 0 ? cleaned : "bridge";
}

export function buildPairingUrl(endpoint: PairingEndpoint, code: string): string {
  return `${PAIRING_URL_SCHEME}?v=${PAIRING_PAYLOAD_VERSION}`
    + `&h=${endpoint.host}`
    + `&p=${endpoint.httpPort}`
    + `&q=${endpoint.pushPort}`
    + `&c=${code}`
    + `&n=${sanitizeBridgeName(endpoint.bridgeName)}`;
}

export function buildManualCode(endpoint: PairingEndpoint, code: string): string {
  return `${endpoint.host} ${endpoint.httpPort} ${endpoint.pushPort} `
    + formatPairingCode(code);
}

export class PairingStore {
  readonly #path: string;
  readonly #now: () => number;
  #devices = new Map<string, PairedDevice>();
  #offer: PairingOffer | null = null;
  #loadedStamp = "";

  public constructor(options: {path: string; now?: () => number}) {
    this.#path = resolve(options.path);
    this.#now = options.now ?? Date.now;
    this.#load();
  }

  public get path(): string {
    return this.#path;
  }

  public get deviceCount(): number {
    return this.#devices.size;
  }

  public list(): PairedDevice[] {
    return [...this.#devices.values()].sort(
      (left, right) => left.pairedAt.localeCompare(right.pairedAt),
    );
  }

  /*
   * `--revoke-device` runs in a second process against a bridge that is already
   * serving. Without this, a revoked key keeps working until the bridge is
   * restarted, which is not what "revocable" should mean. One stat per
   * verification is cheap enough to pay for that being true.
   *
   * Modification time plus size is the usual cheap staleness check. It could in
   * principle miss a same-millisecond, same-size rewrite; for an operator
   * running a CLI against their own bridge that is not a real case, and the
   * fallback is the restart that was previously required anyway.
   */
  #reloadIfChanged(): void {
    if (this.#fileStamp() === this.#loadedStamp) {
      return;
    }
    this.#devices = new Map();
    this.#load();
  }

  #fileStamp(): string {
    try {
      const stats = statSync(this.#path);
      return `${stats.mtimeMs}:${stats.size}`;
    } catch {
      return "";
    }
  }

  /** Replaces any previous offer: at most one code is ever redeemable. */
  public createOffer(
    endpoint: PairingEndpoint,
    ttlMs = DEFAULT_PAIRING_TTL_MS,
  ): PairingOffer {
    const code = generatePairingCode();
    this.#offer = {
      code,
      expiresAt: this.#now() + ttlMs,
      url: buildPairingUrl(endpoint, code),
      manual: buildManualCode(endpoint, code),
      endpoint: {...endpoint, bridgeName: sanitizeBridgeName(endpoint.bridgeName)},
    };
    return this.#offer;
  }

  public activeOffer(): PairingOffer | null {
    if (this.#offer !== null && this.#offer.expiresAt <= this.#now()) {
      this.#offer = null;
    }
    return this.#offer;
  }

  public cancelOffer(): void {
    this.#offer = null;
  }

  /**
   * Exchanges a pairing code for a device token. The code is consumed whether
   * or not the caller keeps the token, so a replayed request cannot mint a
   * second credential.
   */
  public redeem(rawCode: string, rawDeviceName: string): RedeemedPairing {
    const offer = this.activeOffer();
    if (offer === null) {
      throw new ProtocolError(
        409,
        "PAIRING_NOT_OPEN",
        "the bridge is not offering pairing right now",
      );
    }
    const candidate = Buffer.from(normalizePairingCode(rawCode), "utf8");
    const expected = Buffer.from(offer.code, "utf8");
    const matches = candidate.length === expected.length
      && timingSafeEqual(candidate, expected);
    if (!matches) {
      throw new ProtocolError(403, "PAIRING_CODE_REJECTED", "pairing code is not valid");
    }

    const endpoint = offer.endpoint;
    this.#offer = null;
    const name = sanitizeDeviceName(rawDeviceName);
    const deviceToken = randomBytes(32).toString("base64url");
    const device: PairedDevice = {
      deviceId: `dev_${randomBytes(8).toString("hex")}`,
      name,
      tokenHash: hashToken(deviceToken),
      pairedAt: new Date(this.#now()).toISOString(),
      lastSeenAt: null,
    };
    this.#devices.set(device.deviceId, device);
    this.#persist();
    return {device, deviceToken, endpoint};
  }

  /** Returns the matching device, or null. Comparison is constant time. */
  public verifyToken(token: string): PairedDevice | null {
    if (token.length === 0) {
      return null;
    }
    this.#reloadIfChanged();
    const candidate = Buffer.from(hashToken(token), "hex");
    for (const device of this.#devices.values()) {
      const stored = Buffer.from(device.tokenHash, "hex");
      if (stored.length === candidate.length && timingSafeEqual(stored, candidate)) {
        this.#touch(device);
        return device;
      }
    }
    return null;
  }

  /*
   * Records activity, but writes at most once a minute per device. Persisting
   * on every request would put a disk write in the path of every event poll;
   * never persisting would make `--list-devices` report "never" from a separate
   * process, which is worse than not having the field.
   */
  #touch(device: PairedDevice): void {
    const now = this.#now();
    const previous = device.lastSeenAt === null
      ? 0
      : Date.parse(device.lastSeenAt);
    device.lastSeenAt = new Date(now).toISOString();
    if (!Number.isFinite(previous) || now - previous >= 60_000) {
      this.#persist();
    }
  }

  public revoke(deviceId: string): boolean {
    if (!this.#devices.delete(deviceId)) {
      return false;
    }
    this.#persist();
    return true;
  }

  #load(): void {
    let raw: string;
    try {
      raw = readFileSync(this.#path, "utf8");
      this.#loadedStamp = this.#fileStamp();
    } catch {
      return;
    }
    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch {
      throw new Error(`${this.#path} is not valid JSON; move it aside to re-pair`);
    }
    if (typeof parsed !== "object" || parsed === null) {
      return;
    }
    const devices = (parsed as PersistedState).devices;
    if (!Array.isArray(devices)) {
      return;
    }
    for (const device of devices) {
      if (isPairedDevice(device)) {
        this.#devices.set(device.deviceId, device);
      }
    }
  }

  #persist(): void {
    const state: PersistedState = {version: 1, devices: this.list()};
    mkdirSync(dirname(this.#path), {recursive: true});
    writeFileSync(this.#path, `${JSON.stringify(state, null, 2)}\n`, {mode: 0o600});
    /* Our own write must not look like someone else's change. */
    this.#loadedStamp = this.#fileStamp();
  }
}

function sanitizeDeviceName(raw: string): string {
  const trimmed = raw.trim().replace(/[\p{C}]/gu, "");
  const bounded = Buffer.from(trimmed, "utf8")
    .subarray(0, MAX_DEVICE_NAME_BYTES)
    .toString("utf8")
    .replace(/�+$/u, "");
  return bounded.length > 0 ? bounded : "Nintendo 3DS";
}

function isPairedDevice(value: unknown): value is PairedDevice {
  if (typeof value !== "object" || value === null) {
    return false;
  }
  const record = value as Record<string, unknown>;
  return typeof record.deviceId === "string"
    && typeof record.name === "string"
    && typeof record.tokenHash === "string"
    && typeof record.pairedAt === "string";
}
