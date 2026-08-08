# 3gent protocol

This directory contains machine-readable artifacts for the agent-agnostic 3gent
protocol. `docs/PROTOCOL.md` remains the human-readable system of record.

The `0.6.0-hwtest` build implements protocol version 1 over a development-only persistent raw
TCP pushed-control link while retaining HTTP/1.1 for bounded media upload and
fixtures. Both pushed frames and event batches use newline-delimited JSON. Each
v1 event is capped at 640 UTF-8 bytes and each control frame at 4 KiB. The local
raw framing remains replaceable for the authenticated remote path.

Codex-native objects are translated into the same bounded session/event model;
they are never forwarded through this protocol. Event-specific payload schemas
will be split out after the real-adapter hardware pass.
