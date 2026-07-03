# rongo roadmap

A priority-ordered list, not a version-locked plan. What comes next,
most-wanted first.

## Shipped

- **0.1.0** — the TLS client lane: `https://` over OpenSSL with mandatory
  certificate verification.
- **0.2.0** — TLS over NetTcp: non-blocking I/O + keep-alive. OpenSSL now
  runs its state machine over memory BIOs (the Rustls / Go `crypto/tls`
  split) while kaikai's reactor-aware `NetTcp` moves the ciphertext, so a
  request in flight yields the fiber instead of the thread (two requests
  overlap), and a `Session` reuses one connection across requests
  (Content-Length / chunked framing, no re-handshake).
- **0.3.0** — TLS server side: `listen` / `accept` serve TLS with a
  certificate, reusing the same memory-BIO pump (only the handshake
  direction differs). `listen_mtls` requires and verifies a client
  certificate against a CA, rejecting a client that presents none. Server
  and client share one `TlsConn` type, two constructors. The accept loop
  runs on the reactor, so it can serve concurrent connections under
  `Spawn`.

## 1. Client certificates + the HTTP server framework

Two loose ends from 0.3.0. The **client** can't yet present a certificate,
so it can't be the client half of an mTLS connection to a server that
requires one — add a cert/key to `connect`. And the server side today is
raw TLS (accept + read/write plaintext); an **HTTP server** on top —
`serve(listener, handler)` reusing net.http's `parse_request` /
`serialize_response`, with a serve loop and server-side keep-alive — is
its own release-worth of design.

## 2. Connection pooling + dead-connection detection

`Session` reuses one connection today, but the caller manages its
lifetime by hand: a status-0 (transport failure) means "close and reopen"
with no automatic detection. Add a pool (idle eviction, max-per-host) and
mark a connection dead on a mid-stream error so the pool won't hand it
back out — the piece `Session` deliberately left to the caller in 0.2.0.

## 3. HTTP/2 over TLS (ALPN)

Negotiate `h2` via ALPN in the handshake (`SSL_CTX_set_alpn_protos`) and
implement HTTP/2 framing + stream multiplexing. The largest item — a real
protocol, not a wrapper — but it is the "advanced protocols as the
ecosystem grows" the package README scopes rongo for. Builds on the
non-blocking transport (0.2.0) and the pool (2).

## Standing debt (not a feature, but tracked)

- **No automated test of the real OpenSSL path.** kaikai's test runner
  installs no `Ffi` capability, so `kai test` can only cover the pure
  parser and the effect mock. The live path (handshake, verify, the pump,
  keep-alive framing) is covered only by the runnable examples, gated in
  CI. If a way to drive FFI under `kai test` appears upstream, move that
  coverage into real tests.
- **Chunked trailers are framed but not exposed.** `chunked_complete`
  waits for the final CRLF after the terminating chunk (so trailers don't
  corrupt the next keep-alive request), but the trailer headers
  themselves are discarded, not surfaced on the `Response`.
