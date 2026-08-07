# 3gent protocol

This directory contains machine-readable artifacts for the agent-agnostic 3gent
protocol. `docs/PROTOCOL.md` remains the human-readable system of record.

Stage 1 implements protocol version 1 over local persistent HTTP/1.1. Commands
use JSON or bounded media bodies and event batches use newline-delimited JSON.
Each v1 event line is capped at 640 UTF-8 bytes. The transport remains
replaceable for the authenticated remote path.

The schema validates the common event envelope. Event-specific payload schemas
will be split out when the fake-agent event set stabilizes through hardware use.
