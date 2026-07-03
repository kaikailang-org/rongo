/*
 * OpenSSL slot-table shim for rongo's Tls lane.
 *
 * SSL_CTX and SSL are pointers into OpenSSL's internal state, so they
 * cannot cross the kaikai FFI by value. Following the raylib-binding
 * pattern (uira's Sound/Music), we park each live connection in a
 * static slot table and expose scalar-only entry points that the
 * kaikai bindings call. A connection is identified by its slot index.
 *
 * This file includes <openssl/ssl.h>; the out.c kaikai emits must NOT,
 * so the OpenSSL headers stay in this translation unit alone.
 *
 * Byte marshalling across the boundary. A kaikai `String` reaches C as
 * the raw `box->as.s.bytes` pointer (no length), and a returned
 * `String` is repacked with strlen-based `kai_str`. TLS payloads carry
 * embedded 0x00, so:
 *   - SEND takes (data: String, len: Int): the raw pointer is valid for
 *     `len` bytes across any NUL, so send reads exactly `len` — no
 *     encoding, no strlen.
 *   - RECV returns a HEX string: hex never contains NUL, so strlen-based
 *     repacking is lossless. The kaikai side decodes with encoding.hex.
 * The asymmetry pays the 2x only on the receive half.
 *
 * The whole handshake — getaddrinfo, TCP connect, SSL_connect, SNI,
 * hostname + chain verification — lives in ONE entry point
 * (kai_tls_connect). The kaikai side never sees an intermediate state:
 * a slot is either a fully-established, verified connection or a
 * negative error code. Error codes are discriminated so the kaikai
 * layer can tell DNS from connect from handshake from verify failure.
 */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define KAI_TLS_MAX_CONNS 128

/* Hostname verification: OpenSSL 4.0 renamed SSL_set1_host to
 * SSL_set1_dnsname (and deprecated the old name); 3.x and LibreSSL only
 * have SSL_set1_host. Bind to whichever the toolchain ships so rongo
 * builds against OpenSSL 3.x, 4.0, and LibreSSL alike. */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x40000000L
#define KAI_TLS_SET_HOST(ssl, host) SSL_set1_dnsname((ssl), (host))
#else
#define KAI_TLS_SET_HOST(ssl, host) SSL_set1_host((ssl), (host))
#endif

/* Cap on a single recv: the hex return buffer is 2*N+1, so this bounds
 * the static scratch. Callers that pass a larger `max` are clamped. */
#define KAI_TLS_RECV_CAP 65536

/* Negative return codes from kai_tls_connect. The kaikai layer maps
 * each to a distinct Err(String); collapsing verify into a generic
 * failure would be a security bug (a rejected cert must never read as
 * a transient network error). */
#define KAI_TLS_ERR_TABLE_FULL (-1)
#define KAI_TLS_ERR_DNS        (-2)
#define KAI_TLS_ERR_CONNECT    (-3)
#define KAI_TLS_ERR_SSL_NEW    (-4)
#define KAI_TLS_ERR_HANDSHAKE  (-5)
#define KAI_TLS_ERR_VERIFY     (-6)

typedef struct {
  SSL *ssl;
  int  fd;
  int  in_use;
} kai_tls_slot;

static kai_tls_slot slots[KAI_TLS_MAX_CONNS];

/* One process-wide client context. TLS_client_method negotiates the
 * highest protocol both ends support; default verify paths load the
 * system trust store. Built lazily on first connect so a program that
 * never opens a TLS connection pays nothing. */
static SSL_CTX *client_ctx = NULL;

/* Insecure mode is opt-in per process (rongo exposes it only through a
 * separate, ugly-named binding). When set, connect skips verification —
 * for talking to a dev box with a self-signed cert, never in prod.
 *
 * INVARIANT (same reason as recv's static buffer): this global is safe
 * because our fd is a plain blocking socket the reactor never sees, so
 * SSL_connect blocks the OS thread — no other fiber runs between
 * connect_insecure's set(1) and set(0), and no concurrent connect can
 * inherit the insecure mode. If this shim ever registers the fd with the
 * reactor for fiber-yielding I/O, a yield mid-handshake would let a
 * "verified" connect silently run unverified: a MITM hole. Replace this
 * global with a per-connection flag before that change. */
static int insecure_mode = 0;

void kai_tls_set_insecure(int64_t on) { insecure_mode = on != 0 ? 1 : 0; }

static SSL_CTX *kai_tls_ctx(void) {
  if (client_ctx != NULL) { return client_ctx; }
  const SSL_METHOD *method = TLS_client_method();
  if (method == NULL) { return NULL; }
  SSL_CTX *ctx = SSL_CTX_new(method);
  if (ctx == NULL) { return NULL; }
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_default_verify_paths(ctx);
  client_ctx = ctx;
  return ctx;
}

static int kai_tls_slot_alloc(void) {
  for (int i = 0; i < KAI_TLS_MAX_CONNS; i++) {
    if (!slots[i].in_use) { return i; }
  }
  return -1;
}

static int kai_tls_slot_ok(int64_t slot) {
  int i = (int)slot;
  return i >= 0 && i < KAI_TLS_MAX_CONNS && slots[i].in_use;
}

/* Open a blocking TCP socket to host:port via getaddrinfo, trying each
 * resolved address in turn. Returns the connected fd, -1 on DNS
 * failure, -2 when every address refused. */
static int kai_tls_tcp_connect(const char *host, int port) {
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;      /* IPv4 or IPv6 */
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { continue; }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) { break; }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd < 0 ? -2 : fd;
}

/* Full TLS handshake against host:port. Verification (chain + hostname)
 * is mandatory unless insecure_mode is set: SSL_set1_dnsname arms
 * hostname checking (OpenSSL 4.0's replacement for the deprecated
 * SSL_set1_host) and SSL_VERIFY_PEER makes a bad chain fail the
 * handshake. Returns a slot index, or a negative KAI_TLS_ERR_* code. */
int64_t kai_tls_connect(const char *host, int64_t port) {
  int idx = kai_tls_slot_alloc();
  if (idx < 0) { return KAI_TLS_ERR_TABLE_FULL; }

  int fd = kai_tls_tcp_connect(host, (int)port);
  if (fd == -1) { return KAI_TLS_ERR_DNS; }
  if (fd < 0)   { return KAI_TLS_ERR_CONNECT; }

  SSL_CTX *ctx = kai_tls_ctx();
  if (ctx == NULL) { close(fd); return KAI_TLS_ERR_SSL_NEW; }

  SSL *ssl = SSL_new(ctx);
  if (ssl == NULL) { close(fd); return KAI_TLS_ERR_SSL_NEW; }

  if (!insecure_mode) {
    /* Hostname verification against the leaf cert's SAN/CN. */
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (KAI_TLS_SET_HOST(ssl, host) != 1) {
      SSL_free(ssl); close(fd); return KAI_TLS_ERR_SSL_NEW;
    }
    /* Fail the handshake — not just the post-check — on a bad chain. */
    SSL_set_verify(ssl, SSL_VERIFY_PEER, NULL);
  }
  /* SNI: send the hostname so the server picks the right cert. Needed
   * even in insecure mode — many vhosts 404 without it. */
  SSL_set_tlsext_host_name(ssl, host);

  if (SSL_set_fd(ssl, fd) != 1) {
    SSL_free(ssl); close(fd); return KAI_TLS_ERR_SSL_NEW;
  }

  if (SSL_connect(ssl) != 1) {
    /* A verification failure surfaces here (VERIFY_PEER aborts the
     * handshake). Split it out so the caller can tell a rejected cert
     * from a transport-level handshake error. */
    long vr = SSL_get_verify_result(ssl);
    SSL_free(ssl); close(fd);
    return vr != X509_V_OK ? KAI_TLS_ERR_VERIFY : KAI_TLS_ERR_HANDSHAKE;
  }

  slots[idx].ssl    = ssl;
  slots[idx].fd     = fd;
  slots[idx].in_use = 1;
  return (int64_t)idx;
}

/* Write the first `len` bytes of `data`. `data` is the kaikai String's
 * raw byte pointer, valid for `len` bytes across any embedded NUL, so
 * we never call strlen. Returns bytes written (>0), 0 on clean peer
 * shutdown, -1 on error. We do not free `data` (kaikai owns it).
 *
 * A signal interrupting the blocking write (SSL_ERROR_SYSCALL + EINTR)
 * is retried, not reported as a broken connection — otherwise a stray
 * SIGCHLD/SIGWINCH turns a transient interrupt into a dead request. */
int64_t kai_tls_send(int64_t slot, const char *data, int64_t len) {
  if (!kai_tls_slot_ok(slot) || len <= 0) { return -1; }
  SSL *ssl = slots[(int)slot].ssl;
  for (;;) {
    ERR_clear_error();
    errno = 0;
    int n = SSL_write(ssl, data, (int)len);
    if (n > 0) { return (int64_t)n; }
    int err = SSL_get_error(ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) { return 0; }
    if (err == SSL_ERROR_SYSCALL && errno == EINTR) { continue; }
    return -1;
  }
}

/* Read up to `max` bytes and return them with a one-char status prefix
 * followed by lowercase hex (two chars per byte):
 *   "d<hex>"  data: `hex` decodes to the bytes read (always >= 1 byte)
 *   "e"       clean EOF — peer sent close_notify
 *   "x"       error
 * The prefix removes the ""=EOF/0-bytes ambiguity: a zero-length data
 * frame would be "d", distinct from "e". Hex is NUL-free, so the
 * strlen-based String repack on return is lossless.
 *
 * INVARIANT: the static scratch is safe because our fd is a plain
 * blocking socket (kai_tls_tcp_connect opens it without O_NONBLOCK) that
 * kaikai's reactor never sees — SSL_read's underlying read() blocks the
 * OS thread, so no other fiber runs between filling `out`/`buf` and the
 * caller-side kai_str copy. This does NOT depend on kaikai lacking a
 * reactor (it has one for its own NetTcp fds); it depends on OUR fd
 * staying blocking. If this shim is ever changed to register the fd with
 * the reactor for fiber-yielding I/O, two concurrent recvs would race on
 * this buffer — make it per-connection then. */
const char *kai_tls_recv(int64_t slot, int64_t max) {
  static const char hexd[] = "0123456789abcdef";
  static char out[KAI_TLS_RECV_CAP * 2 + 2];   /* status char + hex + NUL */
  static unsigned char buf[KAI_TLS_RECV_CAP];

  if (!kai_tls_slot_ok(slot) || max <= 0) { return "x"; }
  /* max is clamped to KAI_TLS_RECV_CAP; the kaikai binding documents the
   * cap so a caller passing a huge max is not surprised by a short read. */
  int want = max > KAI_TLS_RECV_CAP ? KAI_TLS_RECV_CAP : (int)max;

  SSL *ssl = slots[(int)slot].ssl;
  int n;
  for (;;) {
    ERR_clear_error();
    errno = 0;
    n = SSL_read(ssl, buf, want);
    if (n > 0) { break; }
    int err = SSL_get_error(ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) { return "e"; }   /* clean EOF */
    if (err == SSL_ERROR_SYSCALL && errno == EINTR) { continue; }
    return "x";
  }
  out[0] = 'd';
  for (int i = 0; i < n; i++) {
    out[1 + 2 * i]     = hexd[(buf[i] >> 4) & 0xF];
    out[1 + 2 * i + 1] = hexd[buf[i] & 0xF];
  }
  out[1 + 2 * n] = '\0';
  return out;
}

/* Close the TLS session and the underlying socket, freeing the slot.
 * A single unidirectional SSL_shutdown is enough for a client done
 * reading; we do not wait for the peer's close_notify. A peer that
 * dropped the TCP connection without close_notify leaves entries in
 * OpenSSL's error queue, so we clear it — otherwise a later
 * ERR_get_error() anywhere in the process would read stale garbage. */
void kai_tls_close(int64_t slot) {
  if (!kai_tls_slot_ok(slot)) { return; }
  int i = (int)slot;
  SSL_shutdown(slots[i].ssl);
  SSL_free(slots[i].ssl);
  close(slots[i].fd);
  ERR_clear_error();
  slots[i].ssl    = NULL;
  slots[i].fd     = -1;
  slots[i].in_use = 0;
}
