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

.PHONY: all demo test example clean check-openssl

all: build/rongo

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
LIB_SRCS := https.kai tls.kai ffi/tls.kai $(SHIM)

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
