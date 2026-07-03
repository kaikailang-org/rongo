# rongo — TLS lane over OpenSSL. The kaikai bindings link against the
# `ffi/tls_openssl.c` slot-table shim, which needs OpenSSL's headers and
# libs; both are fed through CFLAGS, exactly as uira feeds raylib.
#
# The emitted out.c must NOT see <openssl/ssl.h> — kaikai has no OpenSSL
# typedefs to collide, but the invariant is the same as uira's: the shim
# is a separate translation unit that owns the OpenSSL include.
#
# OpenSSL is discovered via pkg-config. On macOS with Homebrew's keg-only
# openssl@4, point PKG_CONFIG_PATH at its pkgconfig dir (the recipe does
# this if the keg is present); on Linux the system openssl pkg-config is
# found without help. Any OpenSSL 3.x or 4.x — or LibreSSL — works: the
# shim uses only the stable high-level API.

KAI_BIN ?= kai

# Prefer Homebrew's keg-only openssl@4 if present; otherwise trust the
# environment's pkg-config to resolve `openssl`. PKG_CONFIG_PATH must be
# set INSIDE each $(shell ...) — make's `export` does not reach the
# subshell that $(shell) spawns at parse time, so a bare export would
# silently fall through to a system openssl@3 that lacks OpenSSL 4 API.
OPENSSL4_PC := /opt/homebrew/opt/openssl@4/lib/pkgconfig
ifneq ($(wildcard $(OPENSSL4_PC)),)
  PKG_ENV := PKG_CONFIG_PATH=$(OPENSSL4_PC):$$PKG_CONFIG_PATH
else
  PKG_ENV :=
endif

OPENSSL_CFLAGS := $(shell $(PKG_ENV) pkg-config --cflags openssl)
OPENSSL_LIBS   := $(shell $(PKG_ENV) pkg-config --libs openssl)

SHIM := ffi/tls_openssl.c

KAI_CFLAGS := -std=c99 -O2 -Wall $(OPENSSL_CFLAGS) $(SHIM) $(OPENSSL_LIBS)

.PHONY: all demo test example example-post example-concurrent example-keepalive \
        example-pool example-server example-mtls example-http-server certs clean check-openssl

# The `openssl` CLI from the same install pkg-config resolves.
OPENSSL_BIN := $(shell $(PKG_ENV) pkg-config --variable=prefix openssl)/bin/openssl

all: build/rongo

# Dev certs for the TLS examples. Dev only — never real keys. Gitignored.
# One dev CA signs both the server cert (CN=localhost + SAN) and a client
# cert, so the mTLS example anchors both ends to the same authority. The
# plain server example (tls_echo) uses connect_insecure and ignores the CA.
certs: test-certs/ca.crt test-certs/server.crt test-certs/client.crt

# The dev CA that signs the server and client certs.
test-certs/ca.crt:
	mkdir -p test-certs
	$(OPENSSL_BIN) req -x509 -newkey rsa:2048 -nodes \
	  -keyout test-certs/ca.key -out test-certs/ca.crt \
	  -days 3650 -subj "/CN=rongo-test-ca"

# Server cert signed by the dev CA, with a localhost SAN.
test-certs/server.crt: test-certs/ca.crt
	$(OPENSSL_BIN) req -newkey rsa:2048 -nodes \
	  -keyout test-certs/server.key -out test-certs/server.csr \
	  -subj "/CN=localhost"
	printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > test-certs/san.ext
	$(OPENSSL_BIN) x509 -req -in test-certs/server.csr \
	  -CA test-certs/ca.crt -CAkey test-certs/ca.key -CAcreateserial \
	  -out test-certs/server.crt -days 3650 -extfile test-certs/san.ext
	rm -f test-certs/server.csr test-certs/san.ext test-certs/ca.srl

# Client cert signed by the dev CA, for the mTLS example.
test-certs/client.crt: test-certs/ca.crt
	$(OPENSSL_BIN) req -newkey rsa:2048 -nodes \
	  -keyout test-certs/client.key -out test-certs/client.csr \
	  -subj "/CN=rongo-client"
	$(OPENSSL_BIN) x509 -req -in test-certs/client.csr \
	  -CA test-certs/ca.crt -CAkey test-certs/ca.key -CAcreateserial \
	  -out test-certs/client.crt -days 3650
	rm -f test-certs/client.csr test-certs/ca.srl

# The entry-point demo (rongo.kai) — a live https.get.
demo: build/rongo

build/rongo: rongo.kai https.kai tls.kai ffi/tls.kai $(SHIM) | build
	CFLAGS="$(KAI_CFLAGS)" $(KAI_BIN) build . -o $@

# Examples live under examples/ but build as part of the rongo package so
# their `import rongo.*` resolves. An example with its own kai.toml
# depending on `../..` hits kaikai 0.98's path-dep resolver on a package
# that contains its own consumer, which fails; the `entry` field in
# kai.toml is the only override kai build accepts, so build_example swaps
# the manifest for the build and restores it after.
#
# build_example: $1 = entry .kai path, $2 = output binary.
LIB_SRCS := https.kai http_server.kai pool.kai tls.kai ffi/tls.kai $(SHIM)

define build_example
	cp kai.toml kai.toml.bak
	sed 's#^entry = .*#entry = "$(1)"#' kai.toml.bak > kai.toml
	CFLAGS="$(KAI_CFLAGS)" $(KAI_BIN) build . -o $(2) ; \
	  status=$$? ; mv kai.toml.bak kai.toml ; exit $$status
endef

example: build/https_get
build/https_get: examples/https_get/main.kai $(LIB_SRCS) | build
	$(call build_example,examples/https_get/main.kai,$@)

# Live integration check for the send path over a >16 KB POST body.
example-post: build/https_post
build/https_post: examples/https_post/main.kai $(LIB_SRCS) | build
	$(call build_example,examples/https_post/main.kai,$@)

# The 0.2.0 gate: two concurrent HTTPS requests, one must not block the other.
example-concurrent: build/https_concurrent
build/https_concurrent: examples/https_concurrent/main.kai $(LIB_SRCS) | build
	$(call build_example,examples/https_concurrent/main.kai,$@)

# Keep-alive: three requests over one persistent connection.
example-keepalive: build/https_keepalive
build/https_keepalive: examples/https_keepalive/main.kai $(LIB_SRCS) | build
	$(call build_example,examples/https_keepalive/main.kai,$@)

# Connection pool: many requests, connections reused and reaped automatically.
example-pool: build/https_pool
build/https_pool: examples/https_pool/main.kai $(LIB_SRCS) | build
	$(call build_example,examples/https_pool/main.kai,$@)

# The 0.3.0 gate: a TLS server and rongo's own client, echo end to end.
# Needs the dev cert (`make certs`).
example-server: build/tls_echo
build/tls_echo: examples/tls_echo/main.kai $(LIB_SRCS) | build
	$(call build_example,examples/tls_echo/main.kai,$@)

# The 0.4.0 gate: mutual TLS, both halves rongo's own — a client with a
# cert is served, a client with none is rejected. Needs `make certs`.
example-mtls: build/mtls
build/mtls: examples/mtls/main.kai $(LIB_SRCS) | build
	$(call build_example,examples/mtls/main.kai,$@)

# The 0.5.0 gate: an HTTPS server (serve + handler) and rongo's own client
# over one keep-alive connection, both in one process. Needs `make certs`.
example-http-server: build/http_server
build/http_server: examples/http_server/main.kai $(LIB_SRCS) http_server.kai | build
	$(call build_example,examples/http_server/main.kai,$@)

# Package tests: pure parsers + the effect mock (no network) run under
# `kai test`; the shim is linked so the FFI externs resolve.
test: $(SHIM)
	CFLAGS="$(KAI_CFLAGS)" $(KAI_BIN) test .

build:
	mkdir -p build

# Sanity: is OpenSSL discoverable at all?
check-openssl:
	@$(PKG_ENV) pkg-config --modversion openssl >/dev/null 2>&1 \
		&& echo "openssl $$($(PKG_ENV) pkg-config --modversion openssl) via $$($(PKG_ENV) pkg-config --variable=prefix openssl)" \
		|| { echo "openssl not found by pkg-config — set PKG_CONFIG_PATH"; exit 1; }

clean:
	rm -rf build
