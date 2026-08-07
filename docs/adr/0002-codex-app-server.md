# ADR-0002 — Codex integration through app-server

**Status:** Accepted in principle; implementation details version-sensitive

## Context

3gent needs a rich way to drive Codex:
- prompts;
- streamed responses;
- session/thread state;
- approvals;
- diffs;
- interruption.

Terminal scraping would lose structure and create a large compatibility burden.

## Decision

The desktop bridge should prefer the official Codex `app-server` interface for the Codex adapter.

Initial transport:
```text
codex app-server --listen stdio://
```

The bridge translates Codex-native protocol objects into 3gent's internal model.

## Why

Codex app-server is explicitly intended to power rich interfaces and exposes:
- threads;
- turns;
- items;
- streaming notifications;
- approval requests;
- diff updates.

## Constraints

- App-server protocol can evolve.
- Generate/inspect schemas from the installed Codex version.
- Do not make the 3DS implement Codex's protocol.
- Do not rely on the direct WebSocket listener while it is documented as experimental/unsupported.
- Keep a narrow adapter so future Codex changes do not rewrite the 3DS client.

## Consequence

Codex can become the first rich adapter without making 3gent a Codex-only project.
