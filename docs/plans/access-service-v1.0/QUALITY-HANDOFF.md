# QUALITY-HANDOFF：W0 固定 Chromium 验证

更新：2026-09-24。负责人 Q；本记录是当前执行入口，历史 PR 或构建报告各保留自己的身份。W0 未完成，G0 UNVERIFIED；本轮不新增 required gate。

## 冻结来源与所有权

- 初始盘点 D：`7f74e0a4e971f91d08d90536e429aba7b82cf37d`；原生执行前纳入仅 README 排版的 #176，当前 B=`42f48b535f4ff0c302b6c4e98762fcee86495e1c`。最终交付 H 由运行报告/Git PR 绑定；M、S 尚未生成，不能用 B 冒充它们。
- 产品工作树：`/Volumes/ExternalSSD/repositories/aegis-browser-worktrees/access-w0-quality-20260924`，分支 `codex/access-w0-quality-20260924`。
- Q 新候选：`/Volumes/ExternalSSD/repositories/aegis-chromium-w0-20260924/src`；从闲置 H13 副本进行 APFS clone，仅复用构建输入；复制已完成，前两候选已完成准入并保留 runtime/编译失败，第三修复候选待重验。计划保留相对输出 `out/Pr129Verification`，绝不把复制来的二进制当成新 H 证据。
- Chromium 固定 `151.0.7922.77` / `ff37cfca210138f2a40b843b4a8195ab7e4fc7ff`；新 patched tree、V8、参数/二进制哈希与 sourceStable 须由新候选实际生成。
- 首轮资源核验：外盘 APFS 挂载、可写，约 350 GiB 可用；重型构建前重查。
- 其他任务：PID 64342 / PPID 63161，`evidence/pr23-c9b07b7/native-two-targets.py`，产品 H=`c9b07b72d249b5e1ac1978e11f863b2d953e668a`，source=`aegis-chromium-phase3-20260922/pr23-088fc98/src`，out=`out/Pr129Verification`；其 `.aegis-pr23-native-slot` 与 `.aegis-ci-lock` 归原任务。Q 不 kill/reset/replay，不在它运行时启动第二个重型 Ninja。以上 PID 只是观测快照，启动前重新检查。

## 已有精确证据（历史，不转用）

GitHub 回读 #162：B=`131da2fec25b783e0cc42374728e1f5ffb01cf53`，最终 H=`4670c4dd5f24db61f38f102cc60475658e63554a`，S=`ca4e1b24c750746d6e20a83fa58f1d2500791d02`，2026-09-23 19:51:06 UTC MERGED。旧 H12=`6664528…`/M=`0723b8d…` 不能当作 H13 的 M。

历史 evidence 根目录：`/Volumes/ExternalSSD/repositories/aegis-chromium-phase3-20260922/evidence/pr162-h13-4670c4d/`。本地证据不属于 Git 提交，换机器须取得原始文件。

| 报告 | 结果及准确范围 | SHA-256 |
| --- | --- | --- |
| `native-attempt1/result.json` | `PARTIAL_PASS`，16+45+35=96 tests，sourceStable=true | `c8fd0baf2bbe835d96f055f8a2d588475a619ce5175217bc16cbe681b225976f` |
| `browser-attempt1/result.json` | 冲突导航、History、Settings 各枚举/运行 1 项，exit 0，sourceStable=true；History/Settings 内层 Mocha 已观测 | 14b10b65815874fd61722e99140a4880e5cea3c8821cf3e19182e0e9e46e9898 |

browser binary SHA-256=`603a83cb2ad79f798fff517105e2e54ad2d8898cd5d512fbf3c14488b61523ef`；旧 Chromium tree=`2b924501f5eb8aeac1ce9a911f710f773fe8690d`，旧 V8 tree=`5a6be89cfa0c35d8eb6ee81aec3cb8100780f7e9`。旧参数是显式本机 SDK26.5/bundled LLD 变体，不能称产品原参数通过。

## 本轮第一候选与失败归档

第一候选 H=`cf2329a47e3e35e5e4f89f76767750758ab703f5`，tree=`a41fc24b885741bdd42659c864b0654868d52b2c`；B=`42f48b535f4ff0c302b6c4e98762fcee86495e1c`。以下结果只属于该 H；修复后新 H 必须重验。

| 证据 | 第一候选结果 |
| --- | --- |
| 本地 full quality | PASS/sourceStable=true；产品树 `.artifacts/ci/local-cf2329a/report.json` |
| 托管 PR #177 | CI run `35968346198` attempt 1，quality/quality-gate SUCCESS；报告 tested M=`1921ad40521087696b391c19d1403ee3fa38fddc`，父提交恰为 B/H，tested tree 与 H 相同；不是新 H 的 CI |
| source admission | PASS/sourceStable=true，Chromium tree=`3084075654bfdc589978b00bc5d40c0f55ee1ad1`；Q evidence `source-attempt1/result.json` |
| native attempt 1 | **FAIL/sourceStable=true**；前 10 targets 共 81 tests PASS，第 11 adapter target 首项在 TestingProfile SetUp 因 `chrome::DIR_USER_DATA`（1001）未注册而崩溃，另外 4 项 SKIPPED，后 6 targets NOT_RUN；实际 out args 在首个 runtime 前及结束后哈希相同 |
| browser attempt 1 | **FAIL/sourceStable=true**；41 项实际枚举，NoPublishedPolicyPreservesNativePath PASS；第 2 MainNavigationWithoutPolicyPreservesNativePath 的 origin==1 等待超时，后 39 项 NOT_RUN |
| navigation diagnostic 1 | 同 H 精确用例重跑 exit 1；netlog 捕获 `/resource` 外的 `/favicon.ico` GET，后者收到 HTTP 200/origin；原 fixture 对两者均计数。证据支持修观测范围，不能改成 >=1 放宽断言 |
| 独立 Review | 产品代码 Astra/high CLEAR；外部 browser driver 的 actual out args 稳定性 P2 修后同 reviewer CLEAR。代码 CLEAR 不消除上述真实失败 |

Q evidence 根：`/Volumes/ExternalSSD/repositories/aegis-chromium-w0-20260924/evidence/`。`native-attempt1/`、`browser-attempt1/` 和 `navigation-diagnostic1/` 保存命令、退出码、二进制 hash、summary 与原始日志；不要覆盖 attempt 或删除失败记录。0171 修三个 Profile unit 的 Chrome suite/任务环境，0172 将自动 favicon 与业务请求计数分开；两项均属测试设施修复，修后运行仍待证明。

## 第二候选与编译失败归档

第二候选 H=`745bf3c1d2b3f80e406311b422d94f567fd8a7dc`，tree=`fc0398a30d91d8a56319c78152aea5c1e138f10a`，B 不变。local full PASS/sourceStable=true，source admission 2 PASS/sourceStable=true，Chromium tree=`93c55d2db0a3922495cf231d027638f818e1bd8c`。托管 CI run `35970171524` attempt 1 的 quality/quality-gate SUCCESS；M=`eab204f274f4f782cc9cfb21b16ab20c7531b4dd` 的父提交精确为 B/H、tree 与 H 相同。

- `native-attempt2/result.json`：**FAIL/sourceStable=true**，前 11 targets/86 tests PASS，包含 adapter 全部 5 项，证明 bootstrap 修复已实际运行；第 12 dispatch_state unit 因测试替身 `bool* terminated_` 违反 Chromium raw_ptr 检查而编译失败，后 5 targets NOT_RUN。实际 out args 前后 hash 相同。
- `browser-attempt2/result.json`：**FAIL/sourceStable=true**，新增 favicon helper 的 `GURL::path_piece()` 不在固定 151 API 中，编译失败，0 项 runtime；不能复用第一候选的枚举/运行结果。
- 第二候选的独立代码 Review 曾给 CLEAR，实际编译暴露上述 API 错误后，同 reviewer 已明确撤回 CLEAR，修正为未通过，后续修复必须由同 reviewer 复审并实际编译运行。
- 后续补丁修测试替身非空引用与其生命周期，以及固定版本路径 API；不关闭编译检查、不放宽计数断言。第三候选 17 unit / 41 browser 仍待重验，前两轮失败保留。

## 最小完成矩阵

17 个目标在本轮源代码 BUILD.gn 中均有声明。下列“历史”专指上述 H13 receipt，NOT_RUN 不断言从未在其他候选执行。第一候选失败详情见上节；下一修复候选每项目标必须实际枚举非零测试并执行，记录命令/退出码、summary、二进制哈希、sourceStable。

| unit target | H13 历史结果 | Q 下一修复候选 |
| --- | --- | --- |
| `access_identity_generation_state_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `access_proxy_selection_generation_state_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `access_base_proxy_config_generation_state_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `request_generation_tuple_builder_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `browser_request_metadata_seed_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `request_ownership_registry_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `policy_publication_ack_tracker_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `request_dispatch_gate_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `aegis_access_unittests` | PASS，45 tests | NOT_RUN；缺新候选执行证据 |
| `access_service_coordinator_unittests` | PASS，16 tests | NOT_RUN；缺新候选执行证据 |
| `access_browser_request_adapter_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `access_request_dispatch_state_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `access_identity_generation_source_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `access_proxy_selection_generation_source_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `access_network_context_transport_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |
| `access_rule_store_unittests` | PASS，35 tests | NOT_RUN；缺新候选执行证据 |
| `access_published_request_runtime_unittests` | NOT_RUN | NOT_RUN；缺新候选执行证据 |

Access browser suite `AccessProxyingURLLoaderFactoryBrowserTest` 保留 38 个源码定义，新增 `AccessLoadingPredictorPrefetchBrowserTest` 3 项，共 41 项。必须从新二进制重新枚举，不将源码计数视为实际匹配数。历史 clone 只含 3/17 个 unit 二进制，另 14 个需要构建。

| 必需入口 | 源码映射 / 范围 | 当前缺口 |
| --- | --- | --- |
| 导航 / redirect | `MainNavigation*`、`SubframeNavigation*`、`SameHostRedirectReevaluatesThroughProxy`、`RedirectToUnselectedHostFailsClosed` | 缺新候选执行证据 |
| PREPARED / Profile 隔离 | `MainNavigationConsumesPreparedSnapshotBeforeCommit`、`MainNavigationRoutingIsolatedAcrossProfiles` | 缺新候选执行证据，必须精确非零枚举 |
| Worker / SharedWorker / Service Worker | `WorkerMainResource*`、`WorkerSubresource*`、`SharedWorkerSubresource*`、`ServiceWorkerProcessScript*`、`ServiceWorkerSubresource*`；browser-process SW 脚本仅验证当前原生边界 | 缺新候选执行证据；不扩大为 SW update 全覆盖 |
| missing endpoint / BLOCK | `ProxyPolicyWithoutSelectedEndpointFailsClosed`、各 `WithoutEndpointFailsClosed`、`BlockBarrierTerminatesInFlightProxyRequest` | 缺新候选执行证据；不代表完整缓存/BFCache/prerender BLOCK |
| frame prefetch | `PrefetchWithoutPolicyPreservesNativePath`、`PrefetchUsesSelectedProxy`、`PrefetchWithoutEndpointFailsClosed`，由页面 `<link rel=prefetch>` 触发 | 缺新候选执行证据 |
| browser-process helper | `BrowserProcessPrefetch*` 5 项，直接调用包装 helper | 缺新候选执行证据；不替代真实 LoadingPredictor 入口 |
| LoadingPredictor 真实入口 | 0148 有 `PrefetchManagerTest.AegisProfileUsesGatedFactoryWhenNetworkContextPrefetchEnabled` 模式选择 unit；本轮新增真实 `PrefetchManager::Start` 派发 regression | 本轮新增 `AccessLoadingPredictorPrefetchBrowserTest` 三项源码（native/proxy/missing endpoint），实际通过 `PrefetchManager::Start`；**缺执行证据**，不覆盖预测生成或导航触发 |

资源占用独立于上述源码/执行缺口：主机重启后旧 Hc9 Ninja 已消失，旧报告 `TESTING`、sourceStable=null 与双锁保留为 `INTERRUPTED_UNKNOWN`。协调任务随后取得旧 owner 明确交接，下一重型构建时段归 Q；启动前仍须重查实际进程/容量，只管理 Q 的候选锁。

## 本轮已执行与下一动作

1. 刷新远端 B/D 与 main/upstream 身份，建立外盘隔离工作树；原始 inventory、PR 和进程快照在产品树 `.artifacts/w0-quality-20260924/`。
2. `mise exec -- python -B scripts/dev/chromium_access_gtests_test.py`：17 tests，exit 0；只证明 runner 回归，不是 Chromium unit/browser 结果。
3. LoadingPredictor regression 源码与健康观测检查就绪后冻结最终 H，做 ordered patches + exact overlay 准入；保存新 Chromium/V8 tree、参数、工具、源码输入摘要。
4. 资源释放后 Q 取得自己的 candidate lock；按 unit 小批次串行执行 coordinator/store/runtime/transport/dispatch/tracker 及其余已声明目标，再 build/list/run 真实 browser 入口。每次实际非零枚举，禁重试掩盖失败，首次失败按设施/产品分类并报告协调者。
5. 最终 H 运行 local full quality、独立 Astra/high Review、托管 PR B/H/M + attempt + check source；合并由协调者按实时授权/门槛决定。未闭环项明确保留，不升级 131 个 A/PF 主行。

主会话只能确认模型家族 GPT-6，精确后缀/effort 未由运行时暴露；有界测试实现阶段显式选 `gpt-6-sol` / `xhigh`。最终独立 Review 尚未执行。
