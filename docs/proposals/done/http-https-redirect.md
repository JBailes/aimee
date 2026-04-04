# Proposal: HTTP to HTTPS Redirect for Webchat

## Problem

The webchat server uses TLS with a self-signed certificate. When users navigate to `http://host:8080` instead of `https://host:8080`, the TLS handshake fails and the connection is silently dropped. The browser shows "ERR_CONNECTION_RESET" with no indication that HTTPS is required.

## Goals

- Plain HTTP requests to the webchat port automatically redirect to the HTTPS URL.
- TLS connections continue to work normally.
- No second listening port required.

## Approach

### 1. Pre-handshake byte inspection

Before passing a new connection to OpenSSL, peek at the first byte using `recv(fd, &first, 1, MSG_PEEK)`. TLS ClientHello records start with byte `0x16`. Any other value (ASCII letters for HTTP methods) indicates a plain HTTP connection.

### 2. HTTP redirect response

If the first byte is not `0x16`:
- Read the full HTTP request to extract the `Host` header.
- Send a plain HTTP `301 Moved Permanently` response with `Location: https://<host>:<port>/`.
- Close the connection.

If the first byte is `0x16`, proceed with the normal TLS handshake.

### 3. Port tracking

A static `g_webchat_port` variable stores the listening port (set in `webchat_serve`) so the redirect URL includes the correct port number.

## Files changed

- `src/webchat.c` — added `g_webchat_port`, `http_redirect()` helper, modified `connection_thread()` to peek first byte before TLS handshake
