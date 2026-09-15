# DEV CI 与上游推进操作指南

本指南对应个人 Fork `quinn521/aegis-browser` 的 DEV `main`。它建立基础质量门和可核验的合并证据，不替代 Chromium 完整构建、真实网络、签名、设备或发布验收。

## 固定工具链与本地完整门

仓库在 `.mise.toml` 固定 Node `22.23.1`、pnpm `9.15.0` 与 Python `3.11.9`。使用 mise 时先安装并进入固定环境：

```bash
mise install
mise exec -- node scripts/ci/run-quality.mjs \
  --scope full \
  --base "$(git merge-base HEAD origin/main)" \
  --report-dir ".artifacts/ci/local-$(git rev-parse --short HEAD)"
```

入口依次执行工具链预检、`pnpm install --frozen-lockfile`、`pnpm run quality:fast`、分语言覆盖率生成与校验、许可证元数据记录、修订 4 冻结文件校验、临时副本中的 13 项离线预览检查、CI 门禁回归、工作流验证与源码前后摘要比对。任一阶段非零即整体失败；报告写入被忽略的 `.artifacts/ci/`，不得提交该目录。

## Lint、单元测试与覆盖率口径

根 `pnpm run lint` 使用 ESLint 9 flat config，实际分析 `packages/core/src/**/*.ts` 与 `scripts/ci/**/*.mjs`。它采用 JavaScript/TypeScript recommended 的正确性规则；`tsc --noEmit` 仍由独立 `typecheck` 执行。为了接纳现有代码，例外限制在具体范围：测试文件允许现有 `@ts-nocheck`、精度边界常量与字符串 escape；`privacy/pii.ts` 保留现有正则 escape；`structure-signature.ts` 保留两个空扩展 interface。新增 CI fixture 会证明合法代码退出 0、未定义标识符退出非 0。这里没有把 ESLint 描述成格式化器或完整风格门。

基础 `quality` job 复用一次现有套件，不为覆盖率重跑整套测试。其报告目录包含 `coverage-manifest.json`，各语言分别记录 scope、报告路径、摘要与哈希，`aggregateCoverage` 固定为 `null`：

| 语言 | 本 job 的覆盖范围 | 报告与边界 |
| --- | --- | --- |
| TypeScript | `packages/core/src` 的 28 个生产 `.ts`；同次 Vitest 170 项单测 | LCOV、JSON summary、text；所有未执行生产文件仍进分母。Access 的 TS vectors 只校验共享结构，不等价于 C++ 行为覆盖。 |
| JavaScript | `scripts/ci`、core 工具与 `apps/browser/scripts` 中受控的 Node `.js/.mjs/.cjs` | c8 从同次 Node 子进程的 V8 数据生成报告，并用 `--all` 纳入未执行生产脚本；renderer/WebUI JavaScript 未测。 |
| C++ | `aegis_access` standalone 的 `access_route_planner.cc` 与 `site_proxy_rule_group.cc` | 同次 487-check native binary 使用匹配 clang/llvm profile 生成 LCOV；完整 Chromium、GURL/SQLite、GN/GTest 和浏览器集成未测。 |
| Python | `local-pypi-proxy.py` 与两个 Access vector generators | 固定 coverage.py 隔离 venv；复用 proxy 4 项测试和 native 生成器调用，另有两个拒绝 fixture，combine 后生成 XML/JSON/LCOV/text；两个 prototype worker 未测。 |
| Swift | `apps/ios` 的 25 个产品 Swift 文件 | 独立 `iOS Coverage` workflow 在双 Simulator 上生成 xccov JSON，状态为 `MANUAL_REPORTING_WORKFLOW`；它不属于当前 macOS 必需门，也不是实机、签名或发布证据，Codacy 转换仍待办。 |
| Bash / PowerShell | 手动 Linux kcov 与 Windows Pester coverage jobs | 只代表列明脚本的行为/行覆盖；macOS 专属 shell 分支和真实 Windows UI 仍按报告列为未测。 |
| Java | `Driver.java` Android instrumentation 验收工具 | 独立 `Android Java Coverage` workflow 在专用 API 36 emulator 上执行六项 fixture 并生成 JaCoCo exec/XML，状态为 `MANUAL_REPORTING_WORKFLOW`；它不属于 macOS `quality-gate`，也不代表浏览器 Java runtime、真机或发布验收。 |

当前只关注 macOS；Linux、Windows、iOS 和 Android 验证后置，仅保留手动入口。共享合同、静态产品拓扑与本机 fixture 继续验证。

HTML、CSS、JSON/data 与 GN/GNI 按静态/数据输入单列，不伪造行覆盖率。native 分类仍可对未知产品路径给出 `REVIEW_REQUIRED`；本质量工作不会把它改成 `NOT_APPLICABLE`。

`quality:fast` 保留 core、Mac 脚本、Agent UI、本地模型验证和共享合同。Android 工具/打包 fixture 及 Windows WinRM 自测移至显式 `pnpm run quality:other-platforms`；该入口不会被自动质量门调用。JavaScript c8 `--all` 仍保留完整生产脚本清单，未执行的平台脚本如实进入分母，不通过裁剪统计范围提高覆盖率。

`.github/workflows/other-platform-coverage.yml` 仅支持 `workflow_dispatch`，保留原 Linux Bash 与 Windows PowerShell job 的固定工具、SHA 绑定、失败传播和 artifact。它们不作为自动 CI 的 skipped checks 出现。

## Codacy Production 准备状态

根 `.codacy.yml` 已按官方 Java glob 语义精确忽略依赖、构建/生成输出与补丁运输文件，未整体排除 `third_party`，因此 `apps/browser/overlay/third_party/aegis*` 中的本仓集成仍可分析。配置文件不能启用 Codacy 工具；工具选择与仓库授权必须在 Codacy UI 完成。

当前状态是：`CONFIG_PREPARED`、`APP_AUTH_PENDING`、`FIRST_ANALYSIS_PENDING`、`COVERAGE_UPLOAD_PENDING`。`APP_AUTH_PENDING` 表示当前 GitHub token 无法核实 Codacy App 安装/授权状态，并不证明已经安装或尚未安装。管理员后续只授权目标 Aegis 仓库，核对 Codacy 项目确实对应 `quinn521/aegis-browser` 的 DEV `main`，再以首次分析显示的精确 SHA 验证身份。

本阶段只生成并上传 CI artifact，不加入 token、coverage upload job 或 Codacy/coverage 徽章。未来上传必须使用短期仓库级 secret，禁止给 fork PR 暴露 secret，并把覆盖报告绑定到实际被测提交：PR `pull_request` CI 测试的是 GitHub 合并候选 M，不是产品分支 H；main push 则绑定 S。本地账号、密码、API token 与仓库 token 均不得进入仓库、日志或公开文档。

待提交工作树可以先运行一次，但提交后必须对最终干净 HEAD 重跑。只有报告的 `inputSource` 与 `finalSource` 相同、`result=PASS`，且最终 HEAD/树与报告绑定时，才是有效的本地证据。修改、rebase、冲突修复或重新生成锁文件后旧证据失效。

## PR、main 与证据身份

`.github/workflows/quality.yml` 对所有目标为 `main` 的 PR（含 Draft）和 `main` push 运行 macOS 优先基础门；普通开发分支不重复执行 push 全量门。固定汇总检查名是 `quality-gate`。它仅在唯一必需执行 job `quality` 明确为 `success` 时成功，结果映射必须恰好包含 `quality`；failure、cancelled、skipped、timeout 或缺失均不得放行。

`.github/workflows/ios-coverage.yml` 是独立报告 workflow。仅通过 `workflow_dispatch` 手动生成报告，PR/main push 不自动运行。它继续严格绑定当次 GitHub tested SHA，测试失败仍为非零；当前分支保护只要求 `quality-gate`，因此 iOS 报告不改变 macOS 优先的合并门。

`.github/workflows/android-java-coverage.yml` 也是独立报告 workflow，仅通过 `workflow_dispatch` 手动运行，PR 和 main push 不再自动触发。当前仅推进 macOS，其余平台后置。它构建 normal 与 coverage 两种验收工具以证明普通 APK 不含 JaCoCo，仅在专用 emulator 运行 coverage APK 的六项 fixture，并把 JaCoCo exec/XML、原始 class 摘要与实际 GitHub tested SHA 写入 artifact。固定检查名为 `android-java-coverage`，不并入 macOS `quality-gate`；`browserTested=false` 明确保留工具覆盖率与浏览器 runtime 验收的边界。

报告区分四种身份：

- `B`：目标 base 的精确 SHA；
- `H`：PR 最终 head；
- `M`：GitHub PR 合并候选，必须恰好以 B/H 为两个父提交；
- `S`：上游合并后的实际提交，也是 DEV main 快进同步的目标。

PR 检查测试 M；main push 检查 S。协调者必须从 GitHub API 回读最新 run/job、run attempt、check source、H/B/M/S 和冲突状态，不能仅信任候选代码生成的报告或评论。PR 同步、base 前移、rebase、修复或 run attempt 更新后，旧结果不能转用。

人工补跑只允许在 `main` 上选择精确 `target_sha`，同时提供其精确祖先 `base_sha`。这种 `workflow_dispatch` 结果保留事件类型，不伪装成 PR 或正常 main push 结果。

## 同一功能分支验证与上游合并

2026-09-15 起，DEV main 是上游 main 的镜像；开发和验证发生在 Fork 的 `codex/*` 功能分支。废止“先合并 DEV main，再重新打包上游提交”的日常流程。

1. 刷新 origin/main 和 upstream/main；正常情况下两者 SHA 必须相同。从 upstream/main 创建隔离功能分支，个人 AGENTS.md 留本机并通过 Git 本地 exclude 忽略，不进入任何提交。
2. 运行本地完整质量门和公开历史检查。向 DEV main 提 Draft PR，只用于托管 CI 和独立审查，禁止启用该验证 PR 的 auto-merge，也不合并它。
3. DEV 最终 H 的检查通过后，用同一个 Fork 分支、同一个 H 向上游 main 提 PR；禁止另行 cherry-pick、squash 或重建导出分支。两边 PR 链接互相记录。
4. 上游执行自己的 PR CI 和审查。上游 base 前移或修复改变 H 时，在原功能分支处理，并重新核验两边最终 H/B/M。DEV 成功不替代上游 CI。
5. 只在上游执行最终合并（由上游决定 merge/squash/rebase 方式），记录实际 S 并等待上游 main push CI 成功。不要独立合并 DEV 验证 PR。
6. 协调者刷新远端，核对 origin/main 是 S 的祖先，且没有未审查的 DEV 独有改动，然后普通推送 S 到 DEV main。禁止日常 force/force-with-lease、同步 PR 的独立 merge commit 或关闭 CI。若保护拒绝，停止并报告，不能偷偷放宽规则。
7. DEV 当前允许管理员 bypass，因此授权协调者可在已核验上游 S 的情况下执行镜像快进推送；该权限不用于跳过上游审查和 CI。等待 DEV 的真实 main push CI 成功，再关闭尚未自动关闭的 DEV 验证 PR（不合并），保留来源链接。
8. 刷新两边 refs，验收 SHA 相同、`git rev-list --left-right --count origin/main...upstream/main` 为 `0 0`、文件 diff 为空、两边 S 的 main CI 成功。

若发现已有历史分叉，停止正常快进同步。先核对树差异并备份；一次性历史修复须明确授权，不能伪装为快进，不能每轮重复强推。历史重写产生的 push.before 非祖先失败保留记录；确需人工补跑时用受控 workflow_dispatch 绑定 S 与真实祖先，不伪装成正常 push 成功。

## 保护与公开历史检查

当前约定：DEV main 必须 1 位维护者 Approve，管理员允许 bypass；上游 main 不要求固定数量 Approve，管理员仍受保护约束。两边保留 quality-gate、禁止强推/删除和解决讨论要求。每次操作回读实际配置。仓库 Allow auto-merge 不等于每个 PR 都应启用；DEV 验证 PR 必须关闭 auto-merge。模型 review 不替代测试和 GitHub 必需条件。

功能分支在提交 DEV 验证之前就必须是可公开历史，执行：

```bash
node scripts/ci/check-public-diff.mjs --base upstream/main --head HEAD
mise exec -- node scripts/ci/run-quality.mjs \
  --scope full --base upstream/main --public-base upstream/main \
  --report-dir ".artifacts/ci/public-$(git rev-parse --short HEAD)"
```

检查覆盖最终差异和每个新提交，拒绝个人 AGENTS.md/agent.md 及大小写变体的新增、修改、删除或重命名；上游 base 已有且未变的同名文件不误删。不能先提交个人文件再删除来隐藏历史。

## Chromium 集成边界与故障分类

报告中的 `nativeIntegration` 采用保守分类：overlay、patch、C++/GN、Chromium 固定版本及关键生成/同步脚本变更为 `REQUIRED`；未明确分类的产品路径为 `REVIEW_REQUIRED`；纯文档和本基础 CI 变更可为 `NOT_APPLICABLE`。`quality:fast` 中的独立 native runner 不等于固定 Chromium workspace 的 GN/GTest 或浏览器集成证明。

目前没有为公开 PR 确认安全、隔离、可销毁的专用 Chromium runner，因此 C 层保持 `BLOCKED`。不得把日常电脑注册成公开 PR runner，不得让候选 PR 自选可信标签或 SHA，也不得与正在使用 `/Volumes/ExternalSSD/repositories/aegis-chromium-151/src` 的任务并发 reset、重放或构建。后续控制器必须从可信端校验产品 H、Chromium/V8 base、patched tree、patch series、GN args、目标与退出码，并隔离凭据、缓存信任域和构建锁。

故障按第一处真实失败分类：

- 产品失败：测试、合同、构建或源码稳定性失败，交实现者修复并重跑；
- 基础设施失败：runner、额度、下载、缓存、账号或 GitHub 服务故障，同 SHA 最多补跑一次；
- 身份/治理失败：H/B/M/S、attempt、check source、保护或 review 不匹配，停止合并；
- C 层环境缺失：保留 `BLOCKED`，不能降级成基础门 PASS 或删除必要测试。

CI 故障期间暂停自动合并。修复配置走普通 PR；产品回退走可审查 revert，不强推 main、不删除用户文件或本地证据。
