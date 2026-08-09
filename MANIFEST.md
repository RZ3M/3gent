# 3gent Repository Manifest

Primary project boundaries:

- `.gitignore`
- `AGENTS.md`
- `CLAUDE.md` — pointer to `AGENTS.md`
- `README.md`
- `client-3ds/` — libctru thin client, build instructions, and hardware checks
- `client-3ds/third_party/quirc/` — vendored ISC QR decoder, unmodified (D-021)
- `bridge/` — TypeScript/Node bridge, fake adapter, and automated tests
- `protocol/` — machine-readable protocol-v1 schemas
- `tools/dev-server/` — retained Stage 0 fixtures and tests
- `tools/ui-preview/` — host SVG preview of the handheld interface
- `tools/qr-check/` — decodes the bridge's pairing QR with the vendored decoder
- `docs/ARCHITECTURE.md`
- `docs/DECISIONS.md`
- `docs/INSPIRATION.md`
- `docs/PRODUCT.md`
- `docs/PROTOCOL.md`
- `docs/RESEARCH.md`
- `docs/ROADMAP.md`
- `docs/SECURITY.md`
- `docs/SOURCES.md`
- `docs/UI_UX.md`
- `docs/adr/0001-qr-pairing.md`
- `docs/adr/0002-codex-app-server.md`
- `docs/adr/0003-pairing-implementation.md`
- `prompts/00_BOOTSTRAP_CODEX.md`
- `prompts/01_NEXT_AFTER_STAGE0.md`

Generated 3DS outputs, bridge dependencies/build output, WAV captures, rendered
interface previews, and Python caches are intentionally excluded by
`.gitignore`.
