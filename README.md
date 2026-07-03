# rongo

Advanced networking for [kaikai](https://github.com/kaikailang-org/kaikai).

**Rapa Nui:** *rongo* — errand, order, message, notice; the transmission of
word. The root of *rongorongo*, Rapa Nui's own script. A network carries
messages between nodes; `rongo` is that carrier.

## Scope

`rongo` hosts the networking that does **not** belong in core stdlib —
anything that carries a heavy system-library dependency the bundled kaikai
install must not drag into every binary:

- **TLS** — the `Tls` effect + `tls_wrap`, enabling `https://`. OpenSSL /
  LibreSSL via FFI.
- Advanced protocols and transport as the ecosystem grows (HTTP over TLS,
  and later protocols that need a system stack).

## What stays in core stdlib (not here)

Basic, dependency-free networking ships with kaikai itself:

- `net/http` — plaintext HTTP/1.1 client.
- `net/tcp` — TCP sockets.
- (planned) `net/udp`, `net/dns`.

`net/http` rejects `https://` at parse time by design — HTTPS is enabled by
pulling in `rongo`, which brings its own OpenSSL FFI. This is the boundary
codified in kaikai's `docs/ecosystem.md`: basic networking in stdlib,
advanced networking (system-dependency-carrying) here.

## Status

The TLS lane works: `https://` end to end, over OpenSSL, with mandatory
certificate verification. What comes after `0.1.0` — keep-alive, non-blocking
I/O, TLS server side, HTTP/2 — is in [ROADMAP.md](ROADMAP.md).

The module layout:

- **`rongo.ffi.tls`** — the flat TLS engine over OpenSSL: `connect`,
  `send`, `recv`, `close`, returning `Result[String, _]`, plus the
  `TlsConn` handle. `connect` runs the whole handshake — DNS, TCP, TLS
  ≥ 1.2, SNI, chain + hostname verification — and discriminates the
  failure (DNS / connect / handshake / verify) in the `Err`.
- **`rongo.tls`** — the `Tls` effect + `tls_wrap`. A thin handler layer
  over the engine so a program can be tested against a mock handler with
  no network and no OpenSSL. It has no default handler by design (a user
  package cannot register one), so a `Tls` op is only reachable inside a
  wrap.
- **`rongo.https`** — HTTP/1.1 over TLS: `get` / `post` / `put` /
  `delete` / `request`. This is the `https://` that `net.http` rejects.
  It reuses net.http's transport-agnostic parser, header lookup, and
  chunked decoding, and supplies the two pieces net.http couples to
  plaintext `NetTcp`: an `https://` URL parser and a driver over `Tls`.

Verification is always on. A dev-only escape hatch,
`tls.connect_insecure`, skips it — the deliberately ugly name is the
point; never reach for it in production.

### Example

```kaikai
import rongo.https

fn main() : Unit / Console + Ffi = {
  let resp = https.get("https://example.com/")
  print("status: #{int_to_string(resp.status)}")
}
```

See `examples/https_get/` for a runnable version.

## Build

The kaikai bindings link against the `ffi/tls_openssl.c` slot-table shim,
which needs OpenSSL's headers and libs. Both are fed through `CFLAGS`,
resolved by `pkg-config`:

```sh
make            # build the demo (a live https.get)
make example    # build examples/https_get      (live GET)
make example-post  # build examples/https_post   (live POST, large body)
make test       # run the tests (pure parsers + effect mock, no network)
```

`kai test` covers the pure URL parser and the `Tls` effect against a
mock handler — the runner installs no `Ffi` capability, so a test block
cannot drive the real OpenSSL path. That path is exercised by the
runnable examples: `https_get` (GET) and `https_post`, whose >16 KB body
spans several TLS records and proves `send_all`'s partial-write loop
sends the whole request.

Requires OpenSSL 3.x / 4.x or LibreSSL (the shim uses only the stable
high-level API; hostname verification binds `SSL_set1_dnsname` on 4.0,
`SSL_set1_host` before). On macOS with Homebrew's keg-only `openssl@4`,
the Makefile points `pkg-config` at its directory; on Linux the system
OpenSSL is found without help.

## Install

```
kai add kaikailang-org/rongo@<version>
```

Independent semver; declares its supported kaikai range in its manifest.
