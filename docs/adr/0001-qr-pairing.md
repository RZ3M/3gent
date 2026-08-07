# ADR-0001 — QR pairing as default

**Status:** Accepted

## Context

3gent needs a pairing flow between a 3DS and a desktop bridge/relay.

Typing long endpoints, fingerprints, or tokens on a 3DS is unpleasant. The device already has cameras.

## Decision

QR is the default pairing experience.

The desktop displays a QR code containing only short-lived bootstrap data.

A manual-code flow remains available as a fallback.

## Security constraints

The QR must not be a permanent bearer credential.

It should contain or reference:
- protocol version;
- endpoint;
- short-lived nonce/challenge;
- identity fingerprint/material as the final security design requires;
- optional display name.

The bridge issues revocable device credentials only after the pairing exchange succeeds.

## Consequences

Positive:
- low-friction setup;
- visually appropriate for the device;
- easy to explain.

Negative:
- camera quality may make scanning unreliable;
- QR decoding adds a dependency;
- monitor brightness/size matters.

## Required feasibility work

Test scanning on actual 3DS hardware before relying on QR as the only path.

Manual pairing remains mandatory as a fallback.
