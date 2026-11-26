# macOS Access owned core

`owned-core-lock.json` pins the Xray-core tag, source commit and tree, compatibility
patch, Go module checksums, Go toolchain, build parameters, and the macOS arm64
binary digest. The initial pin matches the iOS Xray source and Go toolchain in
[embedded-core-lock.json](../../ios/EmbeddedCore/embedded-core-lock.json).
Xray-core uses MPL-2.0. The repository carries the
[license text](../../../scripts/licenses/access-core/xray-core-LICENSE.txt).

The patch adds the macOS parent-exit watcher, exclusive loopback listener, and
listener FD handoff used by Access. It enables those behaviors when the
`aegis_owned` Go build tag and the browser's launch contract are both present.
Owned startup reads one JSON config from the browser over stdin. It rejects
additional app services and keeps the upstream browser dialer disabled.

## Prepare a DEV test fixture

From the repository root, use Python 3.11 or later, Git, and Go with access to the
pinned toolchain and modules. Pass a new output directory.

```sh
python3 apps/browser/access-owned-core/prepare.py /absolute/path/to/new-output
```

Use `--source /absolute/path/to/xray-clone` to reuse a local copy of the tagged
source. Preparation checks the tag, source and patched trees, module files,
and binary digest. It writes `xray` and a schema v2 `core-manifest.json` for that
directory. Use the manifest with `--aegis-access-dev-core-manifest` for DEV
browser acceptance and pass the binary to native integration tests using
`--aegis-owned-core-test-binary`. Build output and private test inputs stay in
the acceptance directory.

Run the held-open-stdin startup regression with the prepared binary.

```sh
python3 apps/browser/access-owned-core/test_startup.py /absolute/path/to/new-output/xray
```

When upgrading Xray or Go, review the iOS pin, replay the compatibility patch on
the new tagged source, and update the lock and patch together. Rebuild in a
second clean checkout and compare digests before refreshing the expected binary
digest. Verify listener handoff, parent exit and retirement, and the supported
transports in the macOS browser before accepting the upgrade.
