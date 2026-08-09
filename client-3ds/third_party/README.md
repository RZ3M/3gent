# Vendored third-party sources

## quirc

- Upstream: <https://github.com/dlbeer/quirc>
- Version: 1.2 (release tarball `v1.2`)
- License: ISC — see `quirc/LICENSE`
- Files taken: `lib/quirc.c`, `lib/decode.c`, `lib/identify.c`,
  `lib/version_db.c`, `lib/quirc.h`, `lib/quirc_internal.h`
- Local modifications: **none**

quirc decodes the pairing QR code from a camera frame (D-021, R-006). It is
vendored rather than packaged because devkitPro's `portlibs` does not ship it,
and it is used unmodified so it can be re-imported from upstream by copying the
same six files.

It does not compile under this project's `-Wall -Wextra -Werror`, so
`client-3ds/Makefile` builds these objects with a relaxed warning set. Project
code stays strict.

`tools/qr-check/` compiles this same copy on the host and decodes the bridge's
generated pairing matrix, which is how the encoder and decoder are proved
compatible without a hardware run.
