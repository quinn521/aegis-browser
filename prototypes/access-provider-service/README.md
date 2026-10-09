# ACCESS provider B1 — local synthetic implementation candidate

This independent private package implements the accepted B1 source slice:
installation challenge/register, independent ordinary Profile credentials,
one installation guest pool, persisted exact retries, and parent revocation.
It uses the unchanged Stage A package through `file:../access-provider-contract`.
Node **22.23.1**, pnpm **9.15.0**, jose **6.2.12**, pg **8.23.1** and the resolved
transitive lock are pinned. Installation must ignore lifecycle scripts and use
the task-owned cache; it must not modify the repository workspace or toolchain.

## Status and authority boundary

- Lightweight Node/HTTP validation uses explicitly labeled synthetic fixtures.
  Driver-protocol checks use scripted clients. Neither is PostgreSQL evidence.
- SQL schema, migration, integration tests and process barriers are **SOURCE
  ONLY / NOT_RUN**. No database, daemon, container or production service was
  connected or started during implementation. SQL syntax, role grants,
  PostgreSQL concurrency, durability and recovery remain unverified.
- `mode: 'fixture'` is mandatory. All other modes fail with
  `PRODUCTION_BINDINGS_UNAVAILABLE`; there is no production promotion flag.
  Production signing/trust rotation, trustworthy time, native Profile ownership
  verification and physical-installation uniqueness are unimplemented.
- B2 directory/node ACK/endpoint leases and D budgets/writers are unimplemented.
  Frozen Profile scopes do not create handlers for them. B1 issues no usable
  endpoint lease. `guard=false`, `realLoop=0`.
- I owns the required fresh built-in review of the exact final 15 files. This
  package is a review candidate, not accepted delivery or release evidence.

## Explicit construction and lifecycle

`createApp({mode,db,trust,signer,clock,ownership,administration,limits})` requires
every dependency. Imports and construction do not listen, connect or run DDL.
The app is closed to requests until `start({app,host:'127.0.0.1',port:0})` binds a
numeric loopback origin; explicit ports 1–65535 are also accepted. Each app can
start once. The returned owned server has an idempotent `close()` that closes
connections and the supplied owned store. No CLI, environment parsing or
automatic production defaults exist.

Trust contains issuer, audience, realm, serviceEnvironment, trustEpoch,
algorithm, keyId and publicKey. This executable fixture supports ES256 only.
The signer must match that algorithm/key ID; every issued credential is also
verified with Stage A before persistence. Clock observations are explicit
`{now,uncertaintySeconds}` safe integer seconds. Ownership and administration
are trusted injected adapters; request booleans or frontend labels never become
these adapters. Synthetic ownership grants are installation/recipient-bound
fixture capabilities, with no native-authentication claim.

Limits require all fields: installations ≤128, profiles ≤8 per installation,
challenges ≤4096, issuance operations ≤4096, reserved revocations ≤128,
challengeSeconds ≤60, installationSeconds ≤600, profileSeconds ≤300,
concurrentRequests ≤8 and requestMilliseconds ≤5000. All must be positive.
Expired identities and operation fingerprints remain tombstones; there is no
deletion, renewal or key-rotation API.

## HTTP and PoP

Only three exact POST targets exist:

| Target | Purpose |
| --- | --- |
| `/bootstrap/challenge` | Register or ordinary-Profile challenge |
| `/bootstrap/register` | Installation credential and its single guest pool |
| `/profile-credentials` | Independent recipient-key Profile credential |

Bodies require `Content-Type: application/json`, explicit Content-Length ≤64KiB
and one `Idempotency-Key` matching the body. Duplicate/unknown fields and
malformed UTF-8 are rejected through the frozen strict parser. Authentication
and PoP headers must be single-valued; proofs are ≤16KiB. Query strings,
alternate targets, chunked/encoded bodies and forwarded-origin authority are
not supported. Active bodies, adapters, crypto and transactions share one
monotonic request deadline. A deadline can close the socket before an error
body is written; callers recover ambiguous responses through idempotency.

Challenge body:

```json
{"version":1,"operation":"register","idempotencyKey":"opaque-id","arguments":{"channel":"fixture","installationJwk":{"kty":"EC","crv":"P-256","x":"canonical-public-x","y":"canonical-public-y"}}}
```

The Profile arguments are exactly `{profileKind:'normal',recipientJwk,
ownershipRequest}` and also require `Authorization: Bearer <installation JWS>`.
OTR and Guest return unavailable; System is unsupported. The adapter resolves
the opaque ownershipRequest to a stable normal-Profile subject bound to that
installation and recipient. Distinct Profiles require distinct public keys;
the installation key cannot be a Profile recipient.

Issuance body is exactly `{version:1,challengeId,nonce,idempotencyKey,arguments}`.
`X-Aegis-PoP` is a Compact JWS under the installation key; Profiles additionally
require `X-Aegis-Recipient-PoP` under their independent recipient key. Protected
headers are exactly `{alg:'ES256',kid:<public-key-derived ID>,typ:'aegis-b1-pop+jws'}`.
Public keys contain only EC/P-256 kty/crv/x/y, with canonical coordinates and
library-validated points. Remote keys, embedded keys and private members reject.

Signed PoP claims contain exactly version, method, target, operation, issuer,
audience, realm, serviceEnvironment, role, keyFingerprint, recipientFingerprint,
parentId, parentRevision, challengeId, nonce, requestDigest, actor,
idempotencyKey, issuedAt, notBefore and expiresAt. Registration parent/recipient
bindings are null. Target is the bound origin plus the exact issuance path.
The request digest hashes deterministic sorted-key UTF-8 JSON business
arguments, excluding challenge/proof transients. Key fingerprints hash the
same canonical public-member JSON. Possession proves key control only.

Actual signatures are checked against a non-authorizing challenge snapshot
before write locks, then rechecked against the transaction-locked rows. Missing
or substituted context fails closed. Responses expose only fixed public error
codes; service code logs no payload, token, proof, secret or SQL parameter.

## PostgreSQL source contract

`createOwnedPool` accepts explicit numeric loopback connection parameters only:
database b1, role b1_app/b1_migrator/b1_bootstrap and an explicit synthetic
password. It uses no PG environment/default connection string, with max two
connections and one-second connection/statement/idle-transaction bounds.
This constructor does not itself establish admission or connect.

`PostgresStore` uses parameterized values and fixed SQL identifiers. Each
borrower reserves both connections before BEGIN, after an abortable per-pool
gate. Its SERIALIZABLE transaction uses one client, locks domain → installation
→ pool → root → Profile → child → challenge → operation, commits or rolls back,
then releases/discards the pair. The second client commits clock state. Only
positively aborted business 40001/40P01 transactions are retried, at most twice
inside the original deadline; ambiguous COMMIT is never blindly reissued.
Bigints are checked before safe Number use. A transaction object cannot be
used after completion or rollback.

All schema identities and composite FKs include configured D=(issuer,realm,
serviceEnvironment). Deferred bindings and immutable/tombstone triggers guard
parent kind, pool/Profile/key/claim relationships, deadline, consumed-operation
context, stable results and monotonic state. Hard capacity triggers serialize
on D. The app role has SELECT/INSERT and only narrow row-lock (`UPDATE(id)`), terminal
revocation and consumption UPDATE grants; no DDL, deletion or budget writer.

Trusted lower time controls not-before; expiry uses the maximum DB clock,
trusted upper bound and independently persisted high-water. The logged
`domain_clocks` row is authoritative; `domains.time_highwater` is a frozen
initialization value. A migration trigger seeds the clock row. Narrow SECURITY
DEFINER functions have fixed `search_path=pg_catalog`, qualified objects and no
PUBLIC execute rights. The app has no direct clock DML grants.

Every runtime observation first commits a pending UUID fence, then samples the
injected clock, locks only its independent clock row, samples DB time after
that lock, and commits the nondecreasing floor and cleared fence. The business
transaction never reads that clock row through its SERIALIZABLE snapshot.
A trusted upper below the prior floor rejects after its observation commit;
rejected issuance cannot roll the floor back. Uncertain sampling/storage/clock
COMMIT leaves admission blocked by an unresolved fence across restart. No
memory fallback, automatic fence clearing or reconciliation adapter exists.

Both verified proof deadlines remain required for new and cached results.
After hierarchy locks and signature verification, admission obtains fresh
combined time and synchronously rechecks parent/result, challenge and each
proof expiry (equality rejects). The check repeats after consumption SQL and in
the separate locked post-commit response admission. A clock failure blocks
response admission even when issuance already committed. Stable retries retain
exact saved status, headers and JWS bytes, with nonce consumption atomic in the
business transaction. Another operation key resolves the same original
identity; changed arguments conflict. Date is disabled.

Owned pools install an error sink before checkout, and persistent Client guards
survive release/termination. A checked-out Client error aborts cooperative
adapters and races queries/work against failure, cancellation and the original
deadline. Losing promises are consumed; finished transaction objects cannot
issue late SQL. Healthy idle clients get a bounded ROLLBACK (at most 250ms);
stuck queries, failed clients or expired requests destroy the pinned pg stream
and discard exactly once. Close aborts work and bounds pool drainage at 1s.
Harness wrappers forward event/disposal capabilities and cache Client wrappers;
crash hooks select business COMMIT, excluding clock/preverification commits.

Server-only `app.revokeInstallation({installationId,capability,signal})` requires
the injected administrative capability. It marks the root terminal, increments
revocation sequence once and persists a non-authorizing receipt. Exact repeats
return the saved receipt without requiring a live revoked parent. There is no
HTTP administrative route.

`migrate({pool,trust,owner,verifyOwner})` requires a separately admitted owned
bootstrap client. It refuses an existing provider_b1 schema, switches to the
pre-provisioned migration role and executes the fixed schema plus configured
domain atomically. Role provisioning and isolated database ownership belong to
the later admitted fixture owner; start never runs migration.

## Validation and future SQL execution

Run the focused lightweight suite with the pinned Node binary:

```text
node --test test/validation.test.mjs
```

Tests cover actual JOSE signatures/context substitutions, strict parser/header
boundaries, independent Profile keys/shared pool, stable retries, revocation,
time/capacity/concurrency bounds, failed or stalled signing and owned cleanup.
Synthetic state and scripted driver checks are explicitly named in the suite.
They do not run accepted Stage A's test suite again.

`test/postgres.test.mjs` exports `registerPostgresTests({harness,bindings})`.
It registers nothing on import, and direct execution without admission fails
with SQL_NOT_ADMITTED; a skipped/zero-test run is not a SQL pass. Future bindings
must use `mutableClock(now,true)` and share their exact trust object with the
admitted harness. Every launched SQL test app receives a real PostgresStore.

`test/pg-harness.mjs` requires a verified owner receipt binding run UUID, UID,
exact container/network IDs, owned artifact path, pinned arm64 image digest,
numeric loopback endpoint and explicit per-role credentials. Receipt checks
require the accepted resource/quota/durability/role/secret-file bounds. Every
refreshed gate requires a finite nonnegative safe integer storageFreeBytes of
at least 6 GiB, without coercion, before connection or lifecycle work. The
external verifier and restart lifecycle must be supplied after separate I
admission. The harness does not provision/start Docker or search for a DB.
Owned child workers use IPC for private fixture signing/connection material;
none is placed in argv or logs. COMMIT barriers permit explicit before/after
process kill. Restart callbacks retain the same externally owned data receipt.
Pool/worker cleanup is local; the external owner alone removes exact owned
container/network/data resources.

| PostgreSQL source case | Current status |
| --- | --- |
| Fresh schema, isolated grants and durability settings | NOT_RUN |
| Independent clients/pools, one installation/pool, exact retry | NOT_RUN |
| Independent Profile keys and foreign-domain/pool FKs | NOT_RUN |
| Process crash before/after COMMIT, DB clean/crash restart | NOT_RUN |
| Lost response, concurrent retry, nonce substitution/expiry | NOT_RUN |
| Both concurrent revocation/issuance lock orders and replay | NOT_RUN |
| Expiry during lock/sign/COMMIT and persisted time rollback | NOT_RUN |
| Least-privilege row locks, immutable writes and clock function owners/search path | NOT_RUN |
| Independent clock progress, rejected-floor restart and unresolved pending fence | NOT_RUN |
| Final hierarchy wait with fixed trusted time and advancing DB time | NOT_RUN |
| Checked-out/idle Pool events and both clock COMMIT acknowledgement losses | NOT_RUN |
| Genuine 40001 conflict and exhausted retry budget | NOT_RUN |
| Genuine 40P01 deadlock fixture | NOT_IMPLEMENTED; scripted driver only |

SQL engine acceptance and all real observations remain required. The supplied
external restart/owner-verification lifecycle is also an unimplemented local
adapter until the separate resource admission is granted. No native, hosted
CI, production-authentication, deployment or release claim follows from this
candidate.

## Review-repair evidence boundary

This repair is restricted to the nine assigned files. The frozen attempt01
packet and old-code counterexamples remain unchanged. The one design supplement
ran as Astra/max; the Sol6.1/xhigh CLI implementation route was unavailable.
Continuation was authorized in the original app session with Sol6.1/xhigh
requested; actual app model and effort are UNKNOWN. Raw runs, exact source
hashes and preservation checks are retained in the new repair artifact packet.
I alone owns the pending fresh built-in review. Local synthetic/driver results
cannot clear PostgreSQL, production bindings or release gates.

## R2 lifecycle ownership and proof windows

Each clock row has an immutable positive identity lock_id. The two-integer
advisory namespace1110520113 is reserved in owned database b1. IDs never wrap,
recycle, concatenate or hash D. A session lock on the reserved clock client
spans marker commit, sample, observation commit and acknowledged unlock. Busy
observers poll real ownership under their original deadline, without inspecting
pending state. After ownership is acquired, a remaining pending UUID is an
unresolved fence and rejects without automatic clearing. Clock mutation
functions assert the same session's ownership before taking row locks.

Uncertain COMMIT/unlock/acquisition, client end/error, cancellation or pending
queries destroy/discard the clock connection. Healthy release requires an idle
pinned pg _txStatus and acknowledged absence of lifecycle ownership; no
unlock_all fallback exists. Only query-origin DatabaseError40001/40P01 may
retry an aborted business transaction; adapter-thrown SQLSTATE text cannot.

Migration uses the shared lifetime checkout/abort/disposal adapter and consumes
its dedicated bootstrap pool. Its total budget is5s: admission/read/acquisition/
DDL≤4s, closure≤1s. Optional abort stops late work; COMMIT uncertainty rejects
without blind initialization retry. Callers must not end this pool a second
time. Import/construction still performs no migration.

Authority snapshots both proof strings, verifies the exact signature/context,
and only then strictly decodes that same token's notBefore/expiresAt. Frozen
proof windows retain both lower and upper bounds on new/cached/postcommit
admission. Increased uncertainty can invalidate a proof even while the durable
upper floor advances and older challenge/result remain live.

The genuine DB-only expiry oracle aligns outside a request, then uses sequential
same-client keepalives with≤100ms gaps and a measured≤200ms final host delay.
It asserts the actual250ms lock/1s statement/idle settings and a live backend;
client termination or missed timing is a fixture failure. It remains source
only, alongside cross-pool ownership and unlock/abandonment SQL cases. Broader
per-case pool and identity isolation repairs belong to the later owner stage.
R1's97-test receipts, frozen inputs and the PGowner design remain unchanged.
I owns the unique fresh review of R2; genuine PG is still NOT_RUN.


### PG owner 纯测试与真实运行边界

`pnpm run test:owner` 执行随源码交付的 `test/owner/owner.test.mjs`，覆盖协议解析、资源预算、冻结 harness 的生命周期接口、私有 fixture 身份、取消和有界清理。测试使用假传输或资源；不会启动 listener、PostgreSQL、VM、Docker 或 guest。

`pnpm test` 同时执行原 `test/validation.test.mjs` 和 owner 测试；`pnpm run test:validation` 保留原 135 项 validation 用例。validation 包含本机 HTTP listener，须在允许该执行范围时运行。过去 R3 的 135 项结果是历史证据，不能替代修改后的最终候选运行。

真实 PG suite 仍通过 owner 运行准入和 serial fixture 注册。资源真实性、bootstrap、guest 权限与 ENOSPC、40P01、wire COMMIT ACK 丢失及重启持久化均需要独立的真实运行证据；纯测试通过不代表这些门已通过。失败清理会保留失败和不确定资源状态，绝不清除 durable marker fence。

运行 manifest 的 `tools.colima`、`tools.docker` 和 `tools.pnpm` 需要批准后的 `canonicalPath`、`bytes` 与 `sha256`。preflight 解析安装入口的 symlink，核对其 canonical target，并通过同一 target 执行；VM、OCI 和其他 cache 输入仍要求普通文件，拒绝 symlink。

运行 capability 必须提供 `reviewedSources: {path, sha256}`，指向格式为 `B1_REVIEWED_RUNTIME_SOURCES_V1` 的外部审查清单。清单的 `worktree` 是 canonical checkout 路径，`files` 必须恰好包含 `RUNTIME_SOURCE_FILES` 中的 service 21 项与 local contract 6 项，每项具有 `path`、`type: "file"`、`bytes`、`sha256`；`resolvedContract.entryPath` 绑定 Node 实际解析的 contract 模块在 worktree 下的 canonical 相对路径。Node 解析到的安装副本也逐文件核对已审查的 contract 字节，不能只核对旁边的本地 package。缺失、遗漏、重复、意外项、文件漂移或 dependency retarget 均拒绝准入。

`independentReview` 的固定哈希 JSON receipt 必须 `decision: "CLEAR"`，且 `reviewedIdentity.runtimeSourceManifestSha256` 等于上述清单的 `sha256`。每个获准 runtime action 前都会重新核对 receipt、清单及全部源码。owner 不生成或更新审查清单来接受当前字节；仅 owner5 的旧 capability 不再有效。新候选清单在 R/I 完成审查及运行授权前仍是未准入证据。

fixture 的 wire 协议要求不进行 TLS 协商。preflight、wire startup 和建池前要求 `PGSSLMODE` 未设置或为 `disable`，`PGSSLNEGOTIATION` 未设置或为 `postgres`；`require`、`direct` 等不兼容环境会拒绝。该检查保留进程环境和生产 store/TLS 配置。deadlock 的注册与 201 断言由 guaranteed application cleanup 包住；关闭失败或超时会同时保留注册失败及不确定清理状态。

harness 创建的 owned fixture pool 会固定 `ssl: false`、`sslnegotiation: "postgres"`。固定版本 pg-pool 在实际 checkout 时才构造 Client，首次、排队或空闲过期后的替换 Client 都继承这些选项；store 返回后的 PG 环境漂移不会改变 fixture 协议。创建前的不兼容环境仍拒绝。此固定仅用于 harness 及其 owned worker，不修改生产 store/TLS 或进程环境。纯回归使用真实 harness、PostgresStore、pg-pool/Client 构造与 idle-expiry，连接和 SQL 传输用 doubles，没有 socket 或 PG 运行。
