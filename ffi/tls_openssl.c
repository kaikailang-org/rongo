/*
 * OpenSSL memory-BIO shim for rongo's Tls lane (TLS-over-NetTcp).
 *
 * OpenSSL is driven over a pair of memory BIOs, never a socket: it only
 * runs the TLS state machine (handshake, record layer, crypto) into and
 * out of in-memory buffers. kaikai's reactor-aware NetTcp moves the
 * ciphertext bytes. This is the Rustls / Go crypto.tls / Erlang ssl
 * split — TLS state machine here, transport in kaikai — which gives
 * non-blocking I/O (the fiber-yielding is NetTcp's) and keep-alive (the
 * reusable connection is a NetTcp Conn) for free.
 *
 * Ciphertext crosses the FFI boundary as hex (the boundary truncates a
 * String at the first NUL; hex never contains one). Plaintext IN uses
 * String+len (the raw pointer is NUL-safe for its length); plaintext OUT
 * also comes back hex. All buffers are per-connection — no static state,
 * so a fiber yielding mid-pump (NetTcp parks it during recv/send) cannot
 * corrupt another connection's data.
 *
 * The pump lives here, in C: the kaikai driver never sees a TLS record,
 * a WANT_READ, or the SSL_get_error switch — it only shuttles ciphertext.
 */

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define KAI_TLS_MAX_CONNS 128

/* Hostname verification: OpenSSL 4.0 renamed SSL_set1_host to
 * SSL_set1_dnsname (deprecating the old name); 3.x and LibreSSL only
 * have SSL_set1_host. Bind whichever the toolchain ships. */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x40000000L
#define KAI_TLS_SET_HOST(ssl, host) SSL_set1_dnsname((ssl), (host))
#else
#define KAI_TLS_SET_HOST(ssl, host) SSL_set1_host((ssl), (host))
#endif

/* tls_step / write / read status codes returned to the kaikai driver.
 * The driver loops on WANT_*: drain outgoing ciphertext to NetTcp, or
 * fetch incoming ciphertext from NetTcp, then call again. */
#define KAI_TLS_OK          0   /* op complete (handshake done, or write accepted) */
#define KAI_TLS_WANT_READ   1   /* feed ciphertext via put_incoming, retry */
#define KAI_TLS_WANT_WRITE  2   /* drain ciphertext via take_outgoing, retry */
#define KAI_TLS_ERR_SSL   (-1)  /* fatal TLS error */
#define KAI_TLS_ERR_SLOT  (-2)  /* bad slot */
#define KAI_TLS_ERR_VERIFY (-3) /* handshake failed on cert verification */

typedef struct {
  SSL  *ssl;
  BIO  *rbio;        /* ciphertext IN  — we BIO_write, OpenSSL reads  */
  BIO  *wbio;        /* ciphertext OUT — OpenSSL writes, we BIO_read  */
  int   in_use;
  /* Scratch for hex conversion, sized for one drain/read. Per-connection
   * so a fiber yield mid-pump cannot let another conn overwrite it. */
  char *hexbuf;
  int   hexcap;
} kai_tls_slot;

static kai_tls_slot slots[KAI_TLS_MAX_CONNS];

static SSL_CTX *client_ctx = NULL;

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

/* ---- server listeners --------------------------------------------
 *
 * A listener owns a server SSL_CTX with the cert + key loaded ONCE, and
 * every accepted connection makes a fresh SSL over that shared ctx. The
 * ctx is shared across all accepted fibers without a lock: the scheduler
 * is a single OS thread (fibers, not threads), so two fibers are never
 * inside OpenSSL at the same instant. */

#define KAI_TLS_MAX_LISTENERS 16

/* Listener errors, distinct from connection errors. */
#define KAI_TLS_ERR_LISTENER_FULL (-10)
#define KAI_TLS_ERR_CTX           (-11)
#define KAI_TLS_ERR_CERT          (-12)
#define KAI_TLS_ERR_KEY           (-13)

typedef struct {
  SSL_CTX *ctx;
  int      in_use;
} kai_tls_listener;

static kai_tls_listener listeners[KAI_TLS_MAX_LISTENERS];

static int listener_alloc(void) {
  for (int i = 0; i < KAI_TLS_MAX_LISTENERS; i++) {
    if (!listeners[i].in_use) { return i; }
  }
  return -1;
}

static int listener_ok(int64_t s) {
  int i = (int)s;
  return i >= 0 && i < KAI_TLS_MAX_LISTENERS && listeners[i].in_use;
}

/* Build a server ctx with cert + key loaded. When `ca_file` is non-NULL,
 * require and verify a client certificate (mTLS). Returns a listener
 * slot, or a negative KAI_TLS_ERR_* code. */
static int64_t listener_new(const char *cert_file, const char *key_file,
                            const char *ca_file) {
  int idx = listener_alloc();
  if (idx < 0) { return KAI_TLS_ERR_LISTENER_FULL; }

  const SSL_METHOD *method = TLS_server_method();
  if (method == NULL) { return KAI_TLS_ERR_CTX; }
  SSL_CTX *ctx = SSL_CTX_new(method);
  if (ctx == NULL) { return KAI_TLS_ERR_CTX; }
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1) {
    SSL_CTX_free(ctx); return KAI_TLS_ERR_CERT;
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1) {
    SSL_CTX_free(ctx); return KAI_TLS_ERR_KEY;
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    SSL_CTX_free(ctx); return KAI_TLS_ERR_KEY;
  }

  if (ca_file != NULL) {
    /* mTLS: load the trusted CA and REQUIRE a valid client cert.
     * VERIFY_PEER alone would accept a client that sends none —
     * FAIL_IF_NO_PEER_CERT closes that hole. The callback MUST stay NULL:
     * OpenSSL's default aborts the handshake on any verification failure
     * (bad chain, expired, untrusted CA). A custom callback that returns
     * 1 would silently admit invalid certs — the rejection depends on
     * this default. */
    if (SSL_CTX_load_verify_file(ctx, ca_file) != 1) {
      SSL_CTX_free(ctx); return KAI_TLS_ERR_CERT;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  }

  listeners[idx].ctx    = ctx;
  listeners[idx].in_use = 1;
  return (int64_t)idx;
}

/* Bind a plain-TLS server listener (no client-cert requirement). */
int64_t kai_tls_listener_new(const char *cert_file, const char *key_file) {
  return listener_new(cert_file, key_file, NULL);
}

/* Bind an mTLS listener that requires a client cert signed by `ca_file`. */
int64_t kai_tls_listener_new_mtls(const char *cert_file, const char *key_file,
                                  const char *ca_file) {
  return listener_new(cert_file, key_file, ca_file);
}

/* Free a listener's ctx and slot. Accepted connections are independent
 * (their own SSL objects) and outlive this. */
void kai_tls_listener_free(int64_t listener) {
  if (!listener_ok(listener)) { return; }
  int i = (int)listener;
  SSL_CTX_free(listeners[i].ctx);
  listeners[i].ctx    = NULL;
  listeners[i].in_use = 0;
}

static int slot_alloc(void) {
  for (int i = 0; i < KAI_TLS_MAX_CONNS; i++) {
    if (!slots[i].in_use) { return i; }
  }
  return -1;
}

static int slot_ok(int64_t s) {
  int i = (int)s;
  return i >= 0 && i < KAI_TLS_MAX_CONNS && slots[i].in_use;
}

/* Grow a slot's hex scratch to hold `need` chars (+status+NUL). */
static char *hex_ensure(kai_tls_slot *sl, int need) {
  if (sl->hexcap >= need) { return sl->hexbuf; }
  int cap = need < 4096 ? 4096 : need;
  char *nb = (char *)realloc(sl->hexbuf, (size_t)cap);
  if (nb == NULL) { return NULL; }
  sl->hexbuf = nb;
  sl->hexcap = cap;
  return nb;
}

static void hex_encode(const unsigned char *in, int n, char *out) {
  static const char d[] = "0123456789abcdef";
  for (int i = 0; i < n; i++) {
    out[2 * i]     = d[(in[i] >> 4) & 0xF];
    out[2 * i + 1] = d[in[i] & 0xF];
  }
  out[2 * n] = '\0';
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  return -1;
}

/* ---- lifecycle ---------------------------------------------------- */

/* Wire `ssl` to a fresh pair of memory BIOs and park it in slot `idx`.
 * Returns 0 on success, KAI_TLS_ERR_SSL on allocation failure (and frees
 * `ssl` in that case). Shared by the client and server constructors. */
static int wire_slot(int idx, SSL *ssl) {
  BIO *rbio = BIO_new(BIO_s_mem());
  BIO *wbio = BIO_new(BIO_s_mem());
  if (rbio == NULL || wbio == NULL) {
    if (rbio) { BIO_free(rbio); }
    if (wbio) { BIO_free(wbio); }
    SSL_free(ssl);
    return KAI_TLS_ERR_SSL;
  }
  SSL_set_bio(ssl, rbio, wbio);   /* SSL_free will free both BIOs */
  slots[idx].ssl    = ssl;
  slots[idx].rbio   = rbio;
  slots[idx].wbio   = wbio;
  slots[idx].in_use = 1;
  slots[idx].hexbuf = NULL;
  slots[idx].hexcap = 0;
  return 0;
}

/* Create an SSL client object wired to a fresh pair of memory BIOs, set
 * up SNI + verification, and start the handshake state machine. Returns
 * a slot, or a negative error. `host` drives SNI and hostname
 * verification; `insecure != 0` skips verification (per-connection). */
int64_t kai_tls_new(const char *host, int64_t insecure) {
  int idx = slot_alloc();
  if (idx < 0) { return KAI_TLS_ERR_SLOT; }

  SSL_CTX *ctx = kai_tls_ctx();
  if (ctx == NULL) { return KAI_TLS_ERR_SSL; }

  SSL *ssl = SSL_new(ctx);
  if (ssl == NULL) { return KAI_TLS_ERR_SSL; }

  if (!insecure) {
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (KAI_TLS_SET_HOST(ssl, host) != 1) { SSL_free(ssl); return KAI_TLS_ERR_SSL; }
    /* Callback MUST stay NULL: OpenSSL's default fails the handshake on a
     * bad chain / hostname / expiry. A callback returning 1 would defeat
     * verification silently — the security of connect() rests on it. */
    SSL_set_verify(ssl, SSL_VERIFY_PEER, NULL);
  }
  SSL_set_tlsext_host_name(ssl, host);   /* SNI */

  int rc = wire_slot(idx, ssl);
  if (rc != 0) { return rc; }
  SSL_set_connect_state(slots[idx].ssl);   /* we are the client */
  return (int64_t)idx;
}

/* Accept a new server-side connection over `listener`'s ctx: a fresh SSL
 * on the shared server ctx, wired to memory BIOs, in accept state. The
 * driver then pumps SSL_accept via tls_step. Verification (mTLS) is baked
 * into the ctx, so nothing per-connection is needed here. Returns a
 * connection slot, or a negative error. */
int64_t kai_tls_accept(int64_t listener) {
  if (!listener_ok(listener)) { return KAI_TLS_ERR_SLOT; }
  int idx = slot_alloc();
  if (idx < 0) { return KAI_TLS_ERR_SLOT; }

  SSL *ssl = SSL_new(listeners[(int)listener].ctx);
  if (ssl == NULL) { return KAI_TLS_ERR_SSL; }

  int rc = wire_slot(idx, ssl);
  if (rc != 0) { return rc; }
  SSL_set_accept_state(slots[idx].ssl);    /* we are the server */
  return (int64_t)idx;
}

/* Free the SSL object (which frees both BIOs) and the slot. The caller
 * must have already sent any close_notify via take_outgoing after a
 * tls_shutdown. */
void kai_tls_free(int64_t slot) {
  if (!slot_ok(slot)) { return; }
  int i = (int)slot;
  SSL_free(slots[i].ssl);       /* frees rbio + wbio too */
  free(slots[i].hexbuf);
  ERR_clear_error();
  slots[i].ssl    = NULL;
  slots[i].rbio   = NULL;
  slots[i].wbio   = NULL;
  slots[i].hexbuf = NULL;
  slots[i].hexcap = 0;
  slots[i].in_use = 0;
}

/* ---- ciphertext transport (driver <-> BIOs) ----------------------- */

/* Pull all pending outgoing ciphertext from the write-BIO, hex-encoded.
 * Returns "" when nothing is pending. The driver sends the decoded bytes
 * over NetTcp. MUST be called after every step/write/shutdown until it
 * returns empty — OpenSSL can emit outgoing bytes on any op, and leaving
 * them in the BIO hangs the peer. */
const char *kai_tls_take_outgoing(int64_t slot) {
  if (!slot_ok(slot)) { return ""; }
  kai_tls_slot *sl = &slots[(int)slot];
  size_t pending = BIO_ctrl_pending(sl->wbio);
  if (pending == 0) { return ""; }

  char *hb = hex_ensure(sl, (int)pending * 2 + 1);
  if (hb == NULL) { return ""; }

  /* A separate raw buffer: encoding in place into `hb` would overwrite
   * the ciphertext (hex is 2x, so the front half clobbers the source). */
  unsigned char *raw = (unsigned char *)malloc(pending);
  if (raw == NULL) { return ""; }
  int n = BIO_read(sl->wbio, raw, (int)pending);
  if (n <= 0) { free(raw); return ""; }
  hex_encode(raw, n, hb);
  free(raw);
  return hb;
}

/* Feed incoming ciphertext (hex) received from NetTcp into the read-BIO
 * so OpenSSL can consume it on the next step/read. */
void kai_tls_put_incoming(int64_t slot, const char *hex) {
  if (!slot_ok(slot)) { return; }
  kai_tls_slot *sl = &slots[(int)slot];
  size_t hlen = strlen(hex);
  size_t blen = hlen / 2;
  if (blen == 0) { return; }

  /* Decode hex straight into a temp on the C heap; BIO_write copies it. */
  unsigned char *buf = (unsigned char *)malloc(blen);
  if (buf == NULL) { return; }
  for (size_t i = 0; i < blen; i++) {
    int hi = hex_val(hex[2 * i]);
    int lo = hex_val(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) { free(buf); return; }
    buf[i] = (unsigned char)((hi << 4) | lo);
  }
  BIO_write(sl->rbio, buf, (int)blen);
  free(buf);
}

/* ---- the pump ----------------------------------------------------- */

/* Map an SSL return code to a driver status. Shared by handshake, read,
 * and write so the WANT_* logic lives in ONE place. `ret` is the raw
 * return of the SSL op; `produced_out` says whether the op may have put
 * bytes in the write-BIO (always drain after). */
static int64_t pump_status(kai_tls_slot *sl, int ret) {
  if (ret > 0) { return KAI_TLS_OK; }
  int err = SSL_get_error(sl->ssl, ret);
  switch (err) {
    case SSL_ERROR_NONE:        return KAI_TLS_OK;
    case SSL_ERROR_WANT_READ:   return KAI_TLS_WANT_READ;
    case SSL_ERROR_WANT_WRITE:  return KAI_TLS_WANT_WRITE;
    case SSL_ERROR_ZERO_RETURN: return KAI_TLS_OK;   /* clean close */
    default: {
      /* A verification failure aborts the handshake here; split it out so
       * the driver reports a rejected cert distinctly from a transport
       * error. */
      long vr = SSL_get_verify_result(sl->ssl);
      return vr != X509_V_OK ? KAI_TLS_ERR_VERIFY : KAI_TLS_ERR_SSL;
    }
  }
}

/* Advance the handshake one step. Returns OK when the handshake is
 * complete, or WANT_READ / WANT_WRITE telling the driver to move
 * ciphertext and call again. The driver drains take_outgoing after every
 * call regardless of the status. */
int64_t kai_tls_step(int64_t slot) {
  if (!slot_ok(slot)) { return KAI_TLS_ERR_SLOT; }
  kai_tls_slot *sl = &slots[(int)slot];
  ERR_clear_error();
  int ret = SSL_do_handshake(sl->ssl);
  return pump_status(sl, ret);
}

/* Encrypt `len` plaintext bytes (raw NUL-safe pointer). The ciphertext
 * lands in the write-BIO; the driver drains it with take_outgoing.
 * Returns OK, or WANT_READ (a write can need a read during renegotiation
 * — the driver must feed ciphertext then retry the SAME write). */
int64_t kai_tls_write(int64_t slot, const char *data, int64_t len) {
  if (!slot_ok(slot)) { return KAI_TLS_ERR_SLOT; }
  if (len <= 0) { return KAI_TLS_OK; }
  kai_tls_slot *sl = &slots[(int)slot];
  ERR_clear_error();
  int ret = SSL_write(sl->ssl, data, (int)len);
  return pump_status(sl, ret);
}

/* Decrypt up to `max` plaintext bytes, returned with a one-char status
 * prefix + hex (same framing as 0.1.0's recv):
 *   "d<hex>"  decrypted bytes (>= 1)
 *   "e"       clean end of stream (peer close_notify)
 *   "r"       WANT_READ — driver must feed more ciphertext and retry
 *   "x"       error
 * Hex is NUL-free so the strlen-based String repack is lossless. */
const char *kai_tls_read(int64_t slot, int64_t max) {
  if (!slot_ok(slot)) { return "x"; }
  kai_tls_slot *sl = &slots[(int)slot];
  int want = max <= 0 ? 0 : (max > 65536 ? 65536 : (int)max);
  if (want == 0) { return "x"; }

  char *hb = hex_ensure(sl, want * 2 + 2);   /* status + hex + NUL */
  if (hb == NULL) { return "x"; }
  /* Separate raw buffer: encoding into hb+1 would overwrite the plaintext
   * once the read fills the buffer (hex is 2x the bytes). */
  unsigned char *raw = (unsigned char *)malloc((size_t)want);
  if (raw == NULL) { return "x"; }

  ERR_clear_error();
  int n = SSL_read(sl->ssl, raw, want);
  if (n > 0) {
    hb[0] = 'd';
    hex_encode(raw, n, hb + 1);
    free(raw);
    return hb;
  }
  int err = SSL_get_error(sl->ssl, n);
  free(raw);
  switch (err) {
    case SSL_ERROR_ZERO_RETURN: return "e";
    case SSL_ERROR_WANT_READ:   return "r";
    default:                    return "x";
  }
}

/* Start a clean TLS shutdown: produces a close_notify alert into the
 * write-BIO for the driver to send. One-shot (we do not wait for the
 * peer's close_notify). */
void kai_tls_shutdown(int64_t slot) {
  if (!slot_ok(slot)) { return; }
  ERR_clear_error();
  SSL_shutdown(slots[(int)slot].ssl);
}
