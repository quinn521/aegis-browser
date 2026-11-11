# Embedded Access core (K0 / K1)

Pinned upstream sources and a verified local xcframework build for the in-process
thin bridge (`StartInstance` / `Dial`). Stock LibXray `Invoke` / `RunXray` is
**not** the product ABI.

- Xray-core: `v26.11.9` / `676eaf66f8f05d4d558ee7a74b4736623d3feeb9`
- libXray: `v26.11.9` / `5a0f7d8d1c28b34afb5727b04b26a10f6dbac7e3` (license / source reference only)
- Bridge: `apps/ios/EmbeddedCore/bridge` → `AegisEmbeddedCore.xcframework`

`embedded-core-lock.json` status is `verified_build` with both iOS slice digests.
`AccessGoEmbeddedCore` wraps the linked module. The product publisher
(`AccessEmbeddedProxyEntryFactory`) stays fail-closed until the lock may link,
the framework is linked, **and** a supported Reality Vision config is present. Dev builds read a local path from
`AEGIS_ACCESS_LOCAL_V2RAY_CONFIG` in the app process. Internal Alpha and Beta
builds can use the app resource described below.

## Build / package

```bash
scripts/release/build-ios-access-core.sh \
  --lock apps/ios/EmbeddedCore/embedded-core-lock.json \
  --output apps/ios/EmbeddedCore/Products
```

Then recompute digests (do not invent values):

```bash
scripts/release/record-ios-access-core-digests.sh \
  --xcframework apps/ios/EmbeddedCore/Products/AegisEmbeddedCore.xcframework
```

Freeze matching digests into `embedded-core-lock.json` only after accepting the
build. `Products/` is a local artifact (gitignored), linked by the Xcode project.

## Internal Alpha and Beta test node

The private signing repository owns the GitHub Environment secret
`AEGIS_ACCESS_TEST_NODE_URI` (one complete VLESS Reality Vision share URI).
Expose it only in the private internal archive step. With the locked framework
prepared, call the wrapper from that step, selecting `alpha` or `beta`:

```bash
bash scripts/release/ios-private-test-node-archive.sh beta \
  MARKETING_VERSION=0.8.0 CURRENT_PROJECT_VERSION=1
```

The wrapper accepts additional Xcode settings such as the private job's signing
team. It writes the URI to a temporary file outside the checkout, removes the URI
from Xcode's environment, and prints the private archive path. The input file
and DerivedData are removed on exit. Anyone with the resulting app can extract
the URI. Keep the archive, exported apps, and access to them private.

The shared node's 1 GB daily allowance covers upload and download combined.
It is separate from commercial account entitlements. Public PR, signing, and
release workflows do not need this secret. Ordinary builds omit the node.
Dev, release, and unrecognized channels remove the resource on a reused build
directory and ignore it at runtime.

## Toolchain (verified local attempt)

- Go: `1.27.3`
- gomobile / gobind: `golang.org/x/mobile@v0.0.0-20261011034528-197770444867`
- Artifact: `apps/ios/EmbeddedCore/Products/AegisEmbeddedCore.xcframework`

Slice digests (thin bridge):

- ios-arm64: `4d23ef190459a63963a1df0beb2296cc3c9cd6614e701529c2105f624621e6d4`
- ios-arm64-simulator: `9264f94db8724dab5988d216497f695d0a45f78103f6418ee84a4b86a4207c93`

License texts for these upstream projects are listed in iOS
「开源许可与鸣谢」 via `scripts/licenses/access-core/`.
