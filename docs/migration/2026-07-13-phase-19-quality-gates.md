# Phase 19: quality gates and PR observability

状态：仓库内 blocking/non-blocking workflow contract、真实 Maven Wrapper consumer 与
dependency-verification keyring 已建立；提交 `a43cbae` 的 required CI 和 CodeQL 已取得
首次完整远端绿色证据。Default-branch ruleset 的 active enforcement 已由受保护分支推送
响应确认；故意失败 PR、latest-base policy、最小 bypass ownership、同仓/fork PR metrics
comment 与 `merge_group` 仍待专项远端验收。

## Boundary

Phase 19 只负责质量控制：决定 PR 是否可合并的 required checks，以及不参与合并决策的
性能/体积信息。版本、registry、签名、OIDC、release environment、发布 dry-run 与正式
publish workflow 全部属于 Phase 20。
最终 Git snapshot、物理工作区清理、环境 onboarding 和 clean-checkout 总复验属于
Phase 21，不作为启用 quality gates 或搭建 release workflow 的前置条件。

## Tasks

- [x] 在 required CI 实际运行完整 platform matrix，并取得 Kotlin Linux x64 与 repo-managed Android emulator correctness/conformance 绿色证据；缺少 host、simulator、browser 或 emulator 时必须失败，不能静默跳过。
- [x] 取得 required CI 中 public-surface/package-content/pkg-config/CMake consumer audits 与 CodeQL 全部绿色证据；workflow wiring 可在前序阶段完成，远端执行结果统一由本阶段验收。
- [x] 添加 repo-owned Maven Wrapper 与最小 JVM Maven consumer，从同一隔离 local Maven repository 运行 `verify` 并实际调用 `Document.parse`；required CI 必须执行该真实 Maven smoke，且不得要求开发机预装全局 `mvn`。
- [x] 为 blocking CI 与 CodeQL 分别建立稳定、唯一的 `Required gates` 与 `CodeQL gate` 汇总 checks；汇总 job 必须在任一依赖失败、取消或跳过时失败，ruleset 不直接依赖易变的 matrix job names。
- [x] 让所有 blocking workflows 同时监听 `pull_request` 与 `merge_group`，使同一 required checks contract 可安全启用 GitHub merge queue。
- [x] 提交可导入的 default-branch ruleset recipe，只要求 `Required gates` 与 `CodeQL gate`；禁止把 benchmark、binary size、coverage trend 或其他 informational pipeline 加入 required status checks。
- [ ] 在 GitHub repository 导入并启用 ruleset，验证失败 gate 会阻止 PR merge、最新 base revision policy 生效，并记录最小 bypass ownership；仓库外设置的实际启用状态不能仅以 committed JSON 代替。
- [x] 建立非阻塞 PR metrics workflow，运行 C、Swift、Kotlin/JVM 与 ES/WASM benchmark，并报告 C shared library、Kotlin/JVM JAR 与 ES/WASM binary sizes；任何 metrics 缺失或回归都只能提示，不能改变 required gate 结论。
- [x] 使用 fork-safe 的两段式 PR comment：只读 `pull_request` workflow 执行不可信代码并上传纯数值 artifact，具有写权限的 `workflow_run` commenter 不 checkout、不执行 artifact/PR code，只校验 allowlist 数值并创建或更新单条 PR comment。
- [x] 添加 CI policy audit，机器校验 stable gate names、ruleset contexts、`merge_group`、metrics 非阻塞边界、commenter 权限分离与 scheduled benchmark 不进入 PR gate。

## Blocking gates

- `.github/workflows/ci.yml` 的稳定 check 名为 `Required gates`。它依赖 hygiene、
  package audit、Swift deployment/test、Kotlin platforms/consumers/managed emulator、
  ES Node/browser、C compiler/OS matrix、ASan、UBSan 与 TSan；任一依赖不是 `success`
  都让汇总 gate 失败。
- `.github/workflows/codeql.yml` 的稳定 check 名为 `CodeQL gate`，要求全部产品语言
  matrix 成功。
- 两个 blocking workflows 都监听 `pull_request` 与 `merge_group`。Ruleset 只引用
  两个稳定汇总名，不引用会随 matrix 演进而变化的 leaf job names。
- `.github/rulesets/main.json` 是 default branch 的可导入 recipe。提交 recipe 不等于
  GitHub 远端已经启用；Phase 19 必须在 repository Settings → Rules → Rulesets 导入并
  验证实际 merge blocking 行为。

Kotlin publication 继续由 Gradle/KMP 官方流程生成并发布。由于产品承诺真实 Maven
project 可消费，required CI 要使用固定版本与 distribution checksum 的 repo-owned Maven Wrapper 运行最小 Maven
consumer `verify` 并调用 `Document.parse`；开发机和 runner 都不需要预装全局 `mvn`。
Kotlin 官方发布指引使用 Gradle publish task，Apache Maven 官方建议项目通过 Wrapper
固定并启动 Maven：
[KMP Maven Central publication](https://kotlinlang.org/docs/multiplatform/multiplatform-publish-libraries-to-maven.html)、
[Apache Maven Wrapper](https://maven.apache.org/tools/mavenwrapper.html)。

仓库使用 Maven Wrapper 3.3.4 的官方 `only-script` 启动器，
`.mvn/wrapper/maven-wrapper.properties` 固定 Maven 3.9.16 distribution URL 与
SHA-256。`scripts/check-kotlin-consumers.sh` 先把 KMP/JVM/Android publications 发布到
`build/kotlin-consumer-repository`，再以同一路径作为 `maven.repo.local` 运行
`packages/kotlin-markdown-core/consumers/jvm-maven/pom.xml` 的 `verify` lifecycle；consumer
直接依赖 `com.nouprax:kotlin-markdown-core-jvm:1.0.0`，并从 Java 调用
`Document.Companion.parse`，因此同时验证 Maven effective model、POM resolution、compile/
package/verify lifecycle 与当前 desktop JNI payload。Wrapper distribution cache 也隔离在
ignored `build/maven-user-home`，不依赖全局 `mvn` 或用户级 Maven 写权限。

远端启用顺序固定为：先把 workflows 合入 default branch；通过一个 PR 让两个稳定 check
names 在 repository 中实际出现；再导入 `.github/rulesets/main.json` 并确认 target 是
default branch、enforcement 是 active、required checks 只有两个 gate；最后用故意失败的
测试 PR 验证 merge 被阻止。GitHub 官方说明 ruleset 可从 JSON 导入，并且 required status
checks 必须成功才允许更新受保护 ref：
[importing rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/managing-rulesets-for-a-repository#importing-a-ruleset)、
[required status checks](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/available-rules-for-rulesets#require-status-checks-to-pass-before-merging)。

## Non-blocking PR metrics

`.github/workflows/pr-metrics.yml` 在只读 `pull_request` context 中运行 C、Swift、
Kotlin/JVM 与 ES/WASM benchmark，收集标准化 median/memory 数值，并报告 C shared
library、Kotlin/JVM JAR 与 ES/WASM 的字节数。每个 workload、collection 与 artifact
upload 都是 informational；它们不被 `Required gates`、`CodeQL gate` 或 ruleset 引用。

GitHub 支持通过 issue-comment API 在 PR timeline 创建普通 comment。为支持 fork PR 且
不把 write token 暴露给不可信代码，`.github/workflows/pr-metrics-comment.yml` 只在
`workflow_run` 后读取 artifact：它不 checkout、不 fetch、不执行 PR code 或 artifact，
下载前先通过 Actions API 限制 artifact name/count/size，下载后再次限制 JSON 文件大小，
并只接受固定 platform/runtime/workload/artifact allowlist 中的非负整数。评论带稳定
marker，因此后续 commit 更新原 comment，不制造 comment spam。

普通 PR timeline comment 使用 GitHub issue-comment API；PR 在该 API 中按 issue number
处理。安全边界遵循 GitHub 对 `pull_request_target`/`workflow_run` 的明确警告：privileged
workflow 不得 checkout 或执行 fork/PR code，下载的 artifact 也必须视为不可信数据。
[secure `pull_request_target`](https://docs.github.com/en/actions/reference/security/securely-using-pull_request_target)、
[issue comments API](https://docs.github.com/en/rest/issues/comments)。

当前报告是 hosted-runner 单次 snapshot，只供 review context，不是稳定 performance
baseline，也没有阈值。若将来引入 base/head delta、历史趋势或 regression budget，它们
仍默认 non-blocking；改变为 gate 必须另行修订 Phase 19 contract。

## Machine audit

`pnpm audit:ci` 校验：

- stable gate names 与 ruleset required contexts 完全一致；
- blocking workflows 包含 `merge_group`；
- PR metrics workflow 没有 write permission；
- privileged commenter 没有 checkout/fetch PR code；
- metrics/benchmark/binary-size jobs 不进入 required gate；
- scheduled benchmark workflow 不监听 `pull_request`。

## Acceptance

- [x] 在 required CI 取得完整 matrix、package audits 与 CodeQL 的绿色结果。
- [x] 提交并运行真实 Maven Wrapper consumer，证明 Maven effective model、resolution、lifecycle 与 JVM native payload 均通过。
- [ ] 导入并启用 ruleset，确认失败或缺失 gate 会阻止 merge。
- [ ] 在同仓 PR 与 fork PR 各验证一次 metrics artifact → 单条更新式 comment。
- [ ] 若启用 merge queue，验证 `merge_group` 上两个稳定 gates 都会产生且成功。

## Local evidence

2026-07-13 在 macOS arm64、JDK 26.0.1 上完成：

- `./mvnw --version` 下载并校验固定发行版，报告 Apache Maven 3.9.16；
- `scripts/check-kotlin-consumers.sh` 通过 KMP Gradle、JVM Gradle、Android AAR 与真实 JVM Maven 四类 consumer，Maven `exec:java` 在 `verify` phase 成功调用 `Document.parse`；
- 按 Gradle 官方 dependency-verification 流程导出并提交 70-key ASCII-armored
  `gradle/verification-keyring.keys`，删除全部 `ignored-key`，设置
  `<key-servers enabled="false"/>`；全新 `GRADLE_USER_HOME` 下 Kotlin/JVM 编译、Unified
  Test Platform 与 Android device-test classpath 均通过；
- Kotlin gate 以 `--offline --no-build-cache --rerun-tasks` 通过 JVM/Android host
  correctness、conformance 与 `ktlintCheck`；
- C complexity suite 使用 4 KiB → 16 MiB、总跨度 4096 倍的 endpoint scaling，短样本
  累积计时至少 25 ms；完整 suite、ASan 与 UBSan 均通过；
- `pnpm audit:ci`、`pnpm audit:repository`、shell syntax 与 `git diff --check` 通过。

Gradle keyring 的导出与本地-only keyserver 配置遵循
[Gradle dependency verification](https://docs.gradle.org/current/userguide/dependency_verification.html)
的 committed keyring 做法。本地证据不替代下述远端验收。

## Remote evidence

2026-07-14 在提交
[`a43cbae`](https://github.com/nouprax/markdown-core/commit/a43cbae0e1d88b4325e8c364ca232269a1058f2d)
上完成：

- [required CI run 29305643974](https://github.com/nouprax/markdown-core/actions/runs/29305643974)
  20/20 jobs 成功，稳定汇总 `Required gates` 为 `success`；覆盖 Linux/macOS/Windows C、
  ASan/UBSan/TSan、Swift 与 deployment targets、Kotlin Linux x64/macOS arm64/Android
  host、repo-managed Android emulator correctness/conformance、Kotlin publication
  consumers、ES Node/browser、package audit 与 hygiene；
- [CodeQL run 29305644011](https://github.com/nouprax/markdown-core/actions/runs/29305644011)
  5/5 jobs 成功，C/C++、Java/Kotlin、JavaScript/TypeScript 与 Swift analysis 全部通过，
  稳定汇总 `CodeQL gate` 为 `success`；Kotlin 使用禁 build cache 与强制重编，Swift 保留
  `swift build --target MarkdownCore`，其 CodeQL 初始化耗时 23 秒、tracer 下目标编译耗时
  12 分 01 秒；
- macOS 与 Windows hosted runners 均通过 4096 倍 endpoint complexity gate。一次被放弃
  的中间尺度相邻比率判定曾受 scheduler pause 影响；最终 gate 只以完整尺度 endpoints
  判断渐近增长，中间尺度继续输出作诊断；
- 推送响应确认 default branch ruleset 已 active：禁止 force-push、要求经 PR 更新，并期望
  `Required gates` 与 `CodeQL gate` 两个 required checks；本次维护使用授权 bypass。

仍未完成的远端验收是：以故意失败 PR 验证 merge blocking/latest-base policy 并记录最小
bypass ownership；在同仓 PR 与 fork PR 验证单条更新式 metrics comment；若启用 merge
queue，在 `merge_group` 上验证两个稳定 gates 均产生且成功。

## PR validation protocol

Phase 19 关闭前使用一个同仓、非功能性文档 PR 验证真实 pull-request pipeline。该 PR
必须同时满足以下条件：

- `Required gates` 与 `CodeQL gate` 均出现并成功，且 ruleset 将它们识别为 required；
- PR metrics workflow 运行但不参与 merge 决策，commenter 只创建或更新一条 metrics comment；
- 未满足 required checks 时 PR 不可合并，base 更新后 required checks 必须针对最新 revision
  重新成功；
- fork PR 与故意失败 PR 的安全和 blocking 验证仍需单独留证，不能由同仓绿色 PR 代替。

只有同仓绿色 PR、故意失败 blocking、latest-base policy、最小 bypass ownership、fork metrics
comment 均有远端证据后，才能关闭 Phase 19；若仓库启用 merge queue，还必须补充
`merge_group` 上两个稳定 gates 的绿色证据。

2026-07-14 的验证 PR 将 complexity gate 收紧为 4 KiB → 128 MiB（32768 倍），直接比较
端点的每字节耗时，并以 2.0 倍作为拒绝线。首次本机运行 6 个 case 中 5 个通过；
`many_duplicate_attributes` 的归一化 slowdown 为 4.442 倍，证明当前重复属性归一化路径
存在真实超线性增长，不能通过放宽阈值关闭 Phase 19。该 wall-clock gate 只在普通构建运行；
sanitizer presets 排除 `complexity` label，避免 instrumentation 成本被误判为产品复杂度。
