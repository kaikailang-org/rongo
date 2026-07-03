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
- **0.4.0** — client certificates, closing the mutual-TLS loop:
  `connect_with_cert` presents a client cert (verifying the server against
  the system trust store), `connect_with_cert_ca` also swaps the trust
  root for a private CA — both loaded per-connection on the SSL object via
  an `X509_STORE`, no shared-context mutation. rongo's own client and
  server now speak mTLS to each other end to end. Also fixed: connecting
  to a bare IP literal now verifies against the cert's IP SANs (was
  checking the IP as a DNS name and failing).
- **0.5.0** — the HTTP server framework: `serve(listener, handler)` where
  `handler : (Request) -> Response / e` runs in whatever effect row it
  needs (`serve` is polymorphic in `e`). Reuses net.http's `parse_request`
  / `serialize_response`, supplies a request read-loop over the TLS
  connection, and runs an internal nursery so connections are concurrent
  and a cancel unwinds the server cleanly. Server-side keep-alive: a
  connection serves many requests until `Connection: close` or the peer
  closes. `max_request_bytes` caps one request's buffer.

## 1. Server read timeout

The 0.5.0 server has no read timeout: a slow/idle client holds its fiber
open (a slow-loris DoS on an untrusted network — documented, run behind a
proxy for now). The natural build (race recv against a Clock timer via
`Spawn.select`) does not work — `Spawn.select` is a Phase-2 stub that parks
on the array head instead of returning the first to finish, so there is no
"first-of-two", and `await`-on-one can't provide it. The clean fix is
upstream: a `NetTcp.recv_timeout(c, max, ns)` that exposes the
dual-park-with-deadline the runtime already has for `Actor.receive_timeout`
— small, sound, and it turns the timeout into a single cancellable op with
no fiber race. (Cancellation itself works: the runtime does unpark a fiber
parked on recv or a timer — that was never the problem.)

## 1b. Handler trap isolation (upstream)

`serve` catches a handler's `Cancel.raise` and turns it into a 500, but a
runtime trap in a handler — array out-of-bounds, division by zero, a
non-exhaustive match — calls `exit(1)` at the runtime level, killing the
whole server process, past both the guard and the nursery. There is no
signal→fiber bridge to catch it from a user package. The upstream fix is to
route traps to a fiber-level abort (longjmp to a pad, as the runtime
already does for `Cancel`) instead of `exit(1)`; then `serve`'s existing
guard would contain them with a minimal change. Until then, handlers must
not trap — this is the Erlang-style fault isolation kaikai does not yet
give a server.

## 2. Request routing + body limits

0.5.0's server hands the whole `Request` to one handler. A router
(method + path patterns → handlers), separate header/body size limits, and
finer error responses (413, 400) are the next layer on top of `serve`.

## 3. HTTP server keep-alive drain + connection pooling (client)

The server-side graceful drain (finish in-flight requests within a
deadline on shutdown, not just cancel) pairs with the client-side
connection pool: `Session` reuses one connection today, but the caller
manages its lifetime by hand — a status-0 means "close and reopen" with no
automatic detection. A pool (idle eviction, max-per-host) and dead-conn
marking is the piece `Session` deliberately left to the caller in 0.2.0.

## 4. HTTP/2 over TLS (ALPN)

Negotiate `h2` via ALPN in the handshake (`SSL_CTX_set_alpn_protos`) and
implement HTTP/2 framing + stream multiplexing. The largest item — a real
protocol, not a wrapper — but it is the "advanced protocols as the
ecosystem grows" the package README scopes rongo for. Builds on the
non-blocking transport (0.2.0) and the pool (3).

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
  themselves are discarded, not surfaced on the `Response`. The server does
  not decode chunked request bodies at all — it answers 411.
- **O(n²) read loops.** Both the client's `recv_response_loop` and the
  server's `read_request_loop` rebuild the whole accumulated string each
  iteration to test for completeness. Bounded by the size caps, but a
  streaming reader that scans only the new bytes would drop the quadratic
  factor — fix both together.
