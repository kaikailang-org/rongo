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

rongo speaks HTTPS on both sides: an `https://` client and an HTTP server,
over OpenSSL, non-blocking, with keep-alive and mutual TLS. TLS runs over
kaikai's reactor-aware `NetTcp`, so a request in flight yields the fiber
instead of freezing the thread; two requests overlap, a `Session` reuses
one connection across requests, and the server's accept loop serves
concurrent connections each on its own fiber. What comes after — a read
timeout, request routing, HTTP/2 — is in [ROADMAP.md](ROADMAP.md).

The architecture is the Rustls / Go `crypto/tls` split: OpenSSL runs the
TLS state machine over memory BIOs and never touches a socket; `NetTcp`
moves the ciphertext. The module layout:

- **`rongo.ffi.tls`** — the flat TLS engine. Client: `connect`, `send`,
  `recv`, `close` over the `TlsConn` handle, with the whole handshake
  (DNS, TCP, TLS ≥ 1.2, SNI, chain + hostname verification — including IP
  SANs when the host is an address). `connect_with_cert` /
  `connect_with_cert_ca` present a client certificate for mutual TLS.
  Server: `listen` / `listen_mtls` bind a port with a certificate,
  `accept` returns a `TlsConn` — same type, same pump, mirrored handshake.
- **`rongo.tls`** — the `Tls` effect + `tls_wrap`. The effect ops carry
  pure data (`write` / `read` / `close`, no handle); the connection is
  the handler. A program can be tested against a mock handler with no
  network and no OpenSSL.
- **`rongo.https`** — HTTP/1.1 client over TLS: `get` / `post` / `put` /
  `delete` / `request` (one connection per request), plus `open_session`
  / `session_get` / `close_session` for keep-alive (many requests, one
  connection). Reuses net.http's parser; frames responses by
  Content-Length / chunked so keep-alive does not depend on the peer
  closing.
- **`rongo.http_server`** — HTTP/1.1 server over TLS: `serve(listener,
  handler)` where `handler : (Request) -> Response / e` runs in whatever
  effect row it needs. Concurrent (each connection on its own fiber),
  keep-alive, graceful shutdown on cancel. No read timeout yet — run it
  behind a trusted network or a proxy (see ROADMAP).

Verification is always on. A dev-only escape hatch,
`tls.connect_insecure`, skips it — the deliberately ugly name is the
point; never reach for it in production.

### Example

```kaikai
import rongo.https

fn main() : Unit / Console + NetTcp + Ffi = {
  let resp = https.get("https://example.com/")
  print("status: #{int_to_string(resp.status)}")
}
```

Keep-alive over one connection:

```kaikai
match https.open_session("example.com", 443) {
  Ok(s) -> {
    https.session_get(s, "https://example.com/a")
    https.session_get(s, "https://example.com/b")   # same connection, no re-handshake
    https.close_session(s)
  }
  Err(e) -> print(e)
}
```

An HTTPS server — a handler over a TLS listener:

```kaikai
fn handler(req: http.Request) : http.Response = http.Response {
  status: 200, headers: [], body: "hello from #{req.url}"
}

match tls.listen("127.0.0.1", 8443, "server.crt", "server.key") {
  Ok(l)  -> http_server.serve(l, handler)   # concurrent, keep-alive; cancel to stop
  Err(e) -> print(e)
}
```

The handler may carry effects (`(Request) -> Response / e`) — log, read
files, query a DB — and `serve` threads that row through. For raw TLS
(not HTTP), `tls.accept` returns a `TlsConn` to `recv` / `send` directly.

See `examples/` for runnable versions (`https_get`, `https_post`,
`https_concurrent`, `https_keepalive`, `tls_echo`, `mtls`, `http_server`).

## Build

The kaikai bindings link against the `ffi/tls_openssl.c` OpenSSL shim,
which needs OpenSSL's headers and libs. Both are fed through `CFLAGS`,
resolved by `pkg-config`:

```sh
make                    # build the demo (a live https.get)
make example            # examples/https_get         (live GET)
make example-post       # examples/https_post        (live POST, large body)
make example-concurrent # examples/https_concurrent  (two overlapping requests)
make example-keepalive  # examples/https_keepalive   (3 requests, 1 connection)
make certs              # dev CA + server/client certs for the TLS examples
make example-server     # examples/tls_echo          (TLS server + client, one process)
make example-mtls       # examples/mtls              (mutual TLS, both halves rongo)
make example-http-server # examples/http_server      (HTTPS server + client, keep-alive)
make test               # tests (pure parsers + effect mock, no network)
```

`kai test` covers the pure URL parser and the `Tls` effect against a
mock handler — the runner installs no `Ffi` capability, so a test block
cannot drive the real OpenSSL path. That path is exercised by the
runnable examples: `https_post`'s >16 KB body spans several TLS records,
`https_concurrent` proves one request does not block another,
`https_keepalive` proves a connection is reused across requests,
`tls_echo` runs rongo's own server and client against each other, and
`mtls` closes the mutual-TLS loop (client cert accepted, no-cert
rejected) — both halves rongo, no network.

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
