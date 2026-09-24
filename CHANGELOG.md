## v0.8.0 (2026-09-24)

### Fix

- **ffi**: make the TLS handle tables thread-safe

## v0.7.2 (2026-09-24)

### Fix

- drop the string-length workaround, fixed upstream in 0.124.0

## v0.7.1 (2026-09-23)

### Fix

- declare the hanga-roa edition, the only one the compiler provides

## v0.7.0 (2026-09-23)

### Fix

- **examples**: drop the line continuation inside the https_get doc string
- read string lengths before forwarding them, for HTTP framing
- **http_server**: count live connections by mailbox, not a shared Ref
- **rongo**: propagate OpenSSL native dependencies (#4)

### Refactor

- migrate the sources to the orongo edition (kaikai 0.104)

## v0.6.0 (2026-07-03)

### Feat

- **pool**: client connection pool over the https lane

## v0.5.0 (2026-07-03)

### Feat

- **http**: add the HTTP server framework — serve(listener, handler)

## v0.4.0 (2026-07-02)

### Feat

- **tls**: present client certificates — close the mutual-TLS loop

## v0.3.0 (2026-07-02)

### Feat

- **tls**: serve TLS — listen/accept + mutual TLS

## v0.2.0 (2026-07-02)

### Feat

- **tls**: run TLS over NetTcp for non-blocking I/O and keep-alive

## v0.1.0 (2026-07-02)

### Feat

- **tls**: implement TLS lane over OpenSSL, enabling https://
