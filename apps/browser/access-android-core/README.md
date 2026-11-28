# Android Access embedded runtime

This module exposes the source-pinned Access Go runtime used by iOS through a C ABI for the Android browser. Each native handle owns one core and its streams. Destroying a handle revokes it before draining its core. The ABI accepts a single outbound from the browser's trusted configuration generator and reports status codes without configuration or credential details.

The browser integration must check the trusted channel and Profile before creating a core. The HTTP CONNECT and SOCKS5 listeners, JNI integration and Network Service registration are tracked in [#259](https://github.com/gcsagroup/aegis-browser/issues/259) and [#261](https://github.com/gcsagroup/aegis-browser/issues/261).

Run source tests from this directory with Go 1.27.3.

```sh
go test -race -mod=readonly ./...
```

For the native library, point `CC` at the pinned Android NDK clang wrapper for the product's minimum API level.

```sh
GOOS=android GOARCH=arm64 CGO_ENABLED=1 \
  go build -mod=readonly -buildmode=c-shared -o libaegis_android_access_core.so .
```

The build generates a C header alongside the shared library. Use its status enum to distinguish invalid inputs, retired handles and core errors. The native caller owns its buffers and must run blocking calls off the UI thread.

The Android module uses `anet` v0.0.2 to keep interface enumeration independent of Go's private IPv6 zone caches. At startup it selects address-based enumeration on Android 11 and later, where Android restricts app access to interface link information. The Access ABI handles TCP destinations without IPv6 interface zones.
