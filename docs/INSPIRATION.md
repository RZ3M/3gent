# 3gent — Related Product Patterns

These are architectural references, not dependencies.

## Moshi

Moshi is a mobile terminal for long-running coding agents and shell/tmux sessions on a user-controlled host.

Useful lessons for 3gent:
- remote coding-agent control is useful as a companion, not a replacement computer;
- reconnectability matters;
- voice, approvals, and diffs are valuable mobile surfaces;
- QR/easy pairing can reduce setup pain.

Difference:
- Moshi is fundamentally a real mobile terminal.
- 3gent should be **agent-first and structured-first** because a 3DS is a much more constrained display/input device.

3gent should not clone a full mobile terminal merely because Moshi can.

## Herdr

Herdr is an agent-aware terminal multiplexer.

Useful lessons:
- persistent sessions matter;
- semantic states such as working/blocked/idle are much more useful than staring at terminal pixels;
- an agent-control API can coexist with real PTYs;
- one runtime can manage multiple coding agents.

Potential 3gent integration:
- Herdr adapter lists agents/workspaces;
- 3gent prompts an agent;
- Herdr reports lifecycle state;
- 3gent reads relevant recent output;
- advanced terminal details stay on the host.

3gent should not reimplement Herdr.

## Codex app-server

Codex's app-server is the closest architectural match for a rich 3gent adapter because it already separates a client UI from the local agent runtime.

Useful concepts:
- threads;
- turns;
- items;
- streaming notifications;
- explicit approval requests;
- diff updates.

3gent should normalize these concepts rather than leak them unchanged into the 3DS protocol.
