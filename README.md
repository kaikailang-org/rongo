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

Not started. Design for the first lane (TLS) is preserved in the closed
kaikai issue #351.

## Install

```
kai add kaikailang-org/rongo@<version>
```

Independent semver; declares its supported kaikai range in its manifest.
