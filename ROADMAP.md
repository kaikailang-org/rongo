# rongo roadmap

A priority-ordered list, not a version-locked plan. `0.1.0` ships the
TLS client lane: `https://` over OpenSSL with mandatory certificate
verification. What comes next, most-wanted first.

## 0.2.0 — TLS over NetTcp: non-blocking I/O + keep-alive

The one big rearchitecture, and the thing that unblocks everything after
it. Today the shim owns its own blocking socket (`SSL_set_fd`), so
`SSL_read`/`SSL_write` block the whole OS thread — one in-flight TLS
request stalls every other fiber. kaikai HAS a reactor that drives its
own `NetTcp` fds non-blocking (Phase R2), but its API is `static` to the
runtime: a user FFI shim cannot register its own fd. So "just set the fd
O_NONBLOCK" is not open to us.

The fix is the architecture Rustls, Go `crypto/tls`, and Erlang `ssl`
all use: **separate the TLS state machine from the transport.** OpenSSL
runs over memory BIOs (`BIO_s_mem`) — it only encrypts/decrypts into
buffers and never touches a socket. kaikai's `NetTcp` (already
reactor-aware) moves the ciphertext bytes. The mem-BIO is the dumb joint
between them: OpenSSL doesn't know NetTcp is below, NetTcp doesn't know
TLS is above.

This lands both wanted features at once:
- **Non-blocking I/O** comes free — the fiber-yielding is NetTcp's, so a
  TLS request in flight no longer stalls other fibers. `Cancel` can
  actually interrupt.
- **Keep-alive** falls out naturally — the reusable connection IS a
  NetTcp `Conn`, kept open across requests, paired with a persistent SSL
  slot (the TLS session survives, no re-handshake).

Design decisions (from architecture review):
- **One pump in C, parameterised by op** (handshake/read/write/shutdown),
  not four pumps. The single real trap: drain the write-BIO after EVERY
  SSL op unconditionally (OpenSSL emits outgoing bytes even on WANT_READ),
  not just on WANT_WRITE. Nail it once.
- **The pump lives in C; the kaikai driver only shuttles ciphertext.**
  The shim exposes a state machine ("give me ciphertext" / "take this
  ciphertext to send"); the driver never sees a TLS record or a
  WANT_READ. Keeps the `SSL_get_error` switch off the FFI boundary.
- **`TlsConn = { conn: NetTcp.Conn, slot: Int }`, single kaikai owner,
  atomic ordered close**: SSL_shutdown → drain close_notify → NetTcp.close
  → free slot. A recv-EOF between requests marks the pair dead so the
  pool won't reuse it.
- The shim shrinks: it drops DNS/connect/socket (NetTcp owns those) and
  becomes just the crypto engine (SSL + mem-BIOs + the pump).
- Retires two 0.1.0 invariants (recv static buffer, insecure_mode global)
  — both move to per-connection state, since I/O now yields mid-call.

Gate: `https_get` completes the handshake and returns a body
byte-identical to the 0.1.0 blocking lane, plus a second concurrent
request proving one no longer stalls the other.

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
