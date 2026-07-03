# rongo roadmap

A priority-ordered list, not a version-locked plan. `0.1.0` ships the
TLS client lane: `https://` over OpenSSL with mandatory certificate
verification. What comes next, most-wanted first.

## 1. Connection reuse — keep-alive + pooling

Today every request is connect → send → drain → close: one TLS handshake
per request, the dominant cost of an HTTPS call. Add HTTP/1.1 keep-alive
(honour `Connection: keep-alive`, reuse the `TlsConn` across requests to
the same host:port) and a connection pool over the slot table.

Prerequisite work this unblocks a fix for: the slot table is a fixed 128
entries with no RAII — a leaked handle is never reclaimed. Pooling needs
real lifecycle management (idle eviction, max-per-host), which is the
natural place to add reclamation.

## 2. Reactor-aware, non-blocking TLS I/O

The shim's socket is plain-blocking and invisible to kaikai's reactor, so
`SSL_read`/`SSL_write` block the whole OS thread — one in-flight TLS
request stalls every other fiber. kaikai's reactor already drives its own
`NetTcp` fds non-blocking (Phase R2); rongo should do the same: open the
fd `O_NONBLOCK`, drive the handshake and I/O off `SSL_ERROR_WANT_READ`/
`WANT_WRITE`, and park the fiber on readiness instead of the thread.

This retires two documented invariants at once — the `recv` static buffer
and the `insecure_mode` global both become unsafe the moment I/O yields
mid-call, so both must move to per-connection state as part of this work.
A `Cancel` raised mid-request also becomes able to actually interrupt,
which the blocking path cannot do.

## 3. TLS server side + mutual TLS

Client-only today. Add `accept`/`listen` over TLS (serve `https://`) and
client-certificate authentication (mTLS) for both directions:
`SSL_CTX_use_certificate` / `SSL_CTX_use_PrivateKey` on the server, a
client cert on `connect` for services that require it. Pairs naturally
with net.http's existing server-side helpers (`parse_request`,
`serialize_response`), which rongo can reuse the way `https` reuses the
client parser.

## 4. HTTP/2 over TLS (ALPN)

Negotiate `h2` via ALPN in the handshake (`SSL_CTX_set_alpn_protos`) and
implement HTTP/2 framing + stream multiplexing. The largest item — a real
protocol, not a wrapper — but it is the "advanced protocols as the
ecosystem grows" the package README scopes rongo for. Depends on
connection reuse (1) and non-blocking I/O (2) being in place first, since
multiplexing without them buys little.

## Standing debt (not a feature, but tracked)

- **No automated test of the real OpenSSL path.** kaikai's test runner
  installs no `Ffi` capability, so `kai test` can only cover the pure
  parser and the effect mock. The live path (handshake, verify, EINTR,
  partial write) is covered only by the runnable examples, gated in CI.
  If a way to drive FFI under `kai test` appears upstream, move that
  coverage into real tests.
- **`send_all` is O(n²)** on repeated partial writes (each retry
  re-slices the buffer). A fast path skips the slice for the common
  single-write case; a multi-MB streaming sender would want the offset
  pushed into the shim.
