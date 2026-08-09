# 3gent — Security Model

**Status:** Draft / security-sensitive design must be reviewed before remote release

## 1. Threat model summary

3gent remotely controls software that may:
- read source code;
- edit files;
- execute commands;
- access network resources depending on the agent's policy.

Therefore a compromised remote client must not automatically become unrestricted shell access.

## 2. Trust boundaries

### Trusted authority

The desktop bridge.

It owns:
- local agent integration;
- policy;
- secrets;
- approval validation;
- repository access.

### Less-trusted client

The 3DS.

It can request actions but cannot override bridge policy.

### Minimally trusted relay

If used, the relay should route traffic with the smallest practical amount of retained data and privilege.

## 3. Secrets

Never store these on the 3DS:
- OpenAI API keys;
- Claude/Anthropic keys;
- long-lived provider credentials;
- SSH private keys unless a future separately reviewed design explicitly requires it.

Prefer device-specific, revocable 3gent credentials.

## 4. Pairing

QR is the default bootstrap.

The QR must contain only short-lived pairing material, for example:
- protocol version;
- endpoint;
- random nonce/challenge identifier;
- public-key fingerprint or equivalent identity binding;
- display name.

Do not put a permanent bearer token directly in the QR.

Requirements:
- expiration;
- one-time or replay-resistant use;
- visible device name;
- revocation from desktop;
- manual fallback.

### What version `0.8.0-pairing` implements

- one redeemable offer at a time, expiring in 180 seconds by default, consumed
  on redemption so a replay cannot mint a second credential;
- the QR and the typed fallback carry only endpoint plus code, never a token;
- `POST /v1/pair` returns a 32-byte base64url device token once; the bridge
  keeps only its SHA-256 hash, alongside a `dev_` identifier, device name and
  timestamps;
- constant-time comparison for both the pairing code and the token hash;
- revocation with `--revoke-device <id>`, listing with `--list-devices`; a
  running bridge picks up a revocation without being restarted;
- the token is redacted from verbose HTTP and pushed-control logging.

### What it does not implement, and why

Token checking is **opt-in** (`--require-pairing`, default off). The link is
still plaintext: anyone positioned to reach the port can also read the token in
transit, so requiring it would create the appearance of authentication without
the substance. Enforcement should become the default in the same change that
lands secure transport (§7, D-P11). Until then the credential is an identity for
revocation and audit, not an access control.

The handheld stores its token in cleartext at `sdmc:/3ds/3gent/pairing.cfg`.
The 3DS offers no key storage worth the name and the SD card is removable, so a
device key must be assumed readable by anyone holding the console. This is
precisely why credentials are per-device and revocable. Forgetting a pairing on
the handheld deletes the local file only; it does not revoke anything, and the
interface says so.

The QR payload reserves an `f` field for endpoint identity material. It is not
emitted, because nothing verifies it yet.

## 5. Remote approvals

Each approval response must be bound to:
- approval ID;
- session;
- current bridge state;
- expiration;
- allowed choice set.

Never accept:
```text
"approve whatever is pending"
```

as the production semantic.

## 6. Approval tiers

Proposed model:

### Tier 0 — read-only/status
May be automatic.

### Tier 1 — bounded ordinary action
May be remotely approvable once.

### Tier 2 — broad/high-impact action
May require stronger confirmation.

### Tier 3 — prohibited remotely
Must be confirmed at the development computer or forbidden.

Exact classification is pending.

## 7. Transport security

Remote communication must use a standard, reviewed encryption approach.

Important:
- the 3DS is old hardware;
- TLS/library compatibility must be tested;
- Old 3DS handshake time and memory must be measured before the transport is
  accepted;
- remote hostnames must resolve without blocking the interactive frame loop;
- certificate validation cannot silently assume the user-set 3DS clock is
  correct;
- if modern TLS is difficult, do not solve that by writing a home-made cipher;
- prefer an existing, maintained crypto/TLS implementation or a different architecture.

R-010 must evaluate a maintained TLS branch rather than accepting a stale
packaged dependency by convenience. Endpoint pinning may help with the 3DS clock
and CA-bundle constraints, but a production design must bind a stable endpoint
identity, define rotation/recovery, and avoid an unmaintainable permanent leaf
certificate pin. Disabling certificate or time validation is not an acceptable
remote solution.

## 8. Relay

Relay should not need:
- AI provider keys;
- repository filesystem access;
- shell access.

Consider end-to-end/application-layer protection only if it can be implemented with reviewed primitives and libraries.

## 9. Local development

It is acceptable for the Stage 0 fixtures and Stage 1/1.5 local fake-agent slice
to use plain local HTTP and raw TCP **only as clearly marked development
experiments on a trusted LAN with disposable input**. Neither development port
may be exposed to the internet.

It must not be presented as the remote production security model.

Version `0.6.0-hwtest` additionally contains an explicitly unsafe self-hosted
reverse relay because production security was excluded from the current test
scope. The relay refuses to start without `--unsafe-public`; bridge uplinks need
a token, but 3DS-facing HTTP and control sockets are plaintext and
unauthenticated. It is suitable only for a private/test server, restrictive
firewall, and disposable content. It does not satisfy any public-beta security
requirement and must never be enabled by default.

## 10. Logging

Logs must avoid:
- provider credentials;
- pairing secrets;
- full sensitive prompts by default;
- arbitrary repository file contents unless explicitly enabled for debugging.

The local bridge may expose exact prompts and protocol payloads only through an
explicit diagnostic flag such as `--verbose`. That mode must display a warning,
must remain off by default, and must summarize binary media rather than dumping
raw audio. Verbose terminal output should be treated as sensitive user data.

## 11. Security TODO before public remote beta

- make `--require-pairing` the default, together with secure transport;
- bind the pairing bootstrap to an endpoint identity (the reserved `f` field);
- decide how pairing works through the relay, which today it bypasses;
- document relay authentication;
- define approval tiers;
- threat-model replay and impersonation;
- test sleep/reconnect;
- test lost/stolen 3DS handling;
- review dependencies;
- add responsible disclosure instructions.
