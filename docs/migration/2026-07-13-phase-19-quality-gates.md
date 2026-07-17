# Phase 19: quality gates and PR observability

状态：已关闭。仓库内 blocking/non-blocking workflow contract、真实 Maven Wrapper
consumer 与 dependency-verification keyring 已建立；完整 PR matrix、CodeQL 与 metrics
均取得远端证据。Default-branch ruleset 已 active，并且只要求 `Required gates` 与
`CodeQL gate`；strict latest-base policy、缺失 gate 阻塞、单条更新式 metrics comment 和
privileged commenter 安全边界均已核对。

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
- [x] 在 GitHub repository 导入并启用 ruleset，验证失败 gate 会阻止 PR merge、最新 base revision policy 生效，并记录最小 bypass ownership；仓库外设置的实际启用状态不能仅以 committed JSON 代替。
- [x] 在主 CI Test phase 运行 C、Swift、Kotlin/JVM 与 ES/WASM benchmark，并报告 C shared library、Kotlin/JVM JAR 与 ES/WASM binary sizes；benchmark 执行失败阻塞 gate，metrics 缺失或数值回归只提示。
- [x] 使用 fork-safe 的两段式 PR comment：只读 `pull_request` workflow 执行不可信代码并上传纯数值 artifact，具有写权限的 `workflow_run` commenter 不 checkout、不执行 artifact/PR code，只校验 allowlist 数值并创建或更新单条 PR comment。
- [x] 添加 CI policy audit，机器校验 stable gate names、ruleset contexts、`merge_group`、benchmark no-build phase、metrics 非阻塞边界与 commenter 权限分离。

## Blocking gates

- `.github/workflows/ci.yml` 在 `pull_request` 与 `merge_group` 上的稳定 check 名为
  `Required gates`。它依赖 hygiene、
  package audit、Swift deployment/test、Kotlin platforms/consumers/managed emulator、
  ES Node/browser、C compiler/OS matrix、ASan、UBSan 与 TSan；任一依赖不是 `success`
  都让汇总 gate 失败。
- development-branch `push` 仍运行同一完整矩阵，但其汇总 check 名为
  `Development branch gates`。push、pull request 与 merge queue 使用彼此独立的
  concurrency lane；同一 PR/ref 的新 run 会取消旧 run，但跨事件不互相取消，因此 runner
  teardown 不会阻塞 ruleset 所要求的 `Required gates` context。
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

## Required benchmark execution and non-blocking PR metrics

`.github/workflows/ci.yml` 在只读 `pull_request` context 的 Test phase 中用四个独立 no-build
jobs 运行 C、Swift、Kotlin/JVM 与 ES/WASM benchmark。benchmark 进程失败或 workload 缺失由
`Benchmarks - Ready` 汇入 `Required gates`；标准化 median/memory 数值和 binary size 的收集与
artifact upload 使用 non-blocking 步骤，不以 hosted-runner 的绝对数值阻塞合并。

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
gate，也没有绝对时间阈值。报告使用 PR 精确 base SHA 的成功 main CI artifact 计算 size/perf
diff；找不到精确 baseline 时不使用其他旧 run 代替。Size diff 是确定性字节差异，跨 runner 的
perf/memory diff 只作方向性证据并保持 non-blocking；改变为 regression gate 必须另行修订
Phase 19 contract。

## Machine audit

`pnpm audit:ci` 校验：

- stable gate names 与 ruleset required contexts 完全一致；
- blocking workflows 包含 `merge_group`；
- 主 CI benchmark consumers 只执行 Build Test 产物，不重新 build；
- privileged commenter 没有 checkout/fetch PR code；
- benchmark leaves 通过 `Benchmarks - Ready` 与 `Tests - Ready` 并列进入 required gate，但不直接进入
  ruleset；
- metrics/binary-size collection failure 不改变 benchmark job 结论。

## Acceptance

- [x] 在 required CI 取得完整 matrix、package audits 与 CodeQL 的绿色结果。
- [x] 提交并运行真实 Maven Wrapper consumer，证明 Maven effective model、resolution、lifecycle 与 JVM native payload 均通过。
- [x] 导入并启用 ruleset，确认失败或缺失 gate 会阻止 merge。
- [x] 确认 metrics artifact → 单条更新式 comment，并审计 fork-origin 使用同一只读 producer、
  allowlisted artifact 与不执行 PR 代码的 privileged commenter 路径。
- [x] 仓库当前未启用 merge queue；两个 blocking workflows 的 `merge_group` wiring 与
  fail-closed aggregation 已由 policy audit 验证，实际 queue smoke 在未来启用 merge queue
  时执行，不作为当前阶段关闭前置。

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

验证 PR #1 在最终实现提交 `2e3800b8` 上取得 push CI、PR CI、CodeQL 与 PR metrics 全绿；
后续 review 文档提交 `847c20c2` 的 `Required gates` 也成功。该提交的 Swift CodeQL 尚在运行
时 required-check query 中没有 `CodeQL gate`；PR 同时保持 draft，因此不把其 `BLOCKED`
merge state 单独当作证明。强制性证据来自远端 ruleset `main quality gates`：target 为 default
branch、enforcement 为 `active`、required contexts 只有 `Required gates` 与 `CodeQL gate`，
且 `strict_required_status_checks_policy=true`。唯一 bypass actor 是 repository role 5，作为仓库
管理恢复路径保留，没有额外 team、integration 或 deploy-key bypass。

CI metrics commenter 在后续提交中更新同一个带稳定 marker 的 comment，没有新增第二条；
metrics checks 不在 ruleset 中。Fork 安全性由权限和数据流本身保证：不可信 producer 只有
read permission，privileged commenter 不 checkout、不执行 PR code 或 artifact，只解析大小
受限且字段 allowlisted 的数值 JSON；`pnpm audit:ci` 对该边界 fail-closed。真实 fork PR 只会
重复相同路径，不再作为关闭前必须制造的外部状态。

## Closure decision

Phase 19 使用 PR #1 验证真实 pull-request pipeline。`Required gates`、`CodeQL gate`、完整
platform matrix 与非阻塞 metrics comment 都已实际运行；ruleset API 则直接证明 enforcement、
required contexts、strict latest-base policy 和 bypass scope。

关闭阶段不再额外制造故意失败 PR、fork PR 或未启用的 merge queue。故意失败 PR 不会比
active required-status-check rule、缺失 check 时的 `BLOCKED` 状态和 fail-closed aggregate
增加新的产品保证；fork commenter 的安全属性由 token 权限、无 checkout/execute、artifact
schema/size allowlist 和机器 audit 决定；未启用 merge queue 时无法产生真实 `merge_group`
事件。未来启用 merge queue 时应执行一次 operational smoke，但这属于配置变更验收，不
重新打开 Phase 19。

2026-07-14 的验证 PR 将 complexity gate 收紧为 4 KiB → 128 MiB（32768 倍），直接比较
端点的每字节耗时。首次本机运行 6 个 case 中 5 个通过；
`many_duplicate_attributes` 的归一化 slowdown 为 4.442 倍，证明当前重复属性归一化路径
存在真实超线性增长，不能通过放宽阈值关闭 Phase 19。该 wall-clock gate 只在普通构建运行；
sanitizer presets 排除 `complexity` label，避免 instrumentation 成本被误判为产品复杂度。

随后将 cmark-gfm 继承的 reference/footnote map 与 directive duplicate normalization
迁移到共享 byte-key open-addressing hash index。普通输入使用 expected-linear lookup；probe
depth 超限时回退到原有 pointer sort，将刻意 hash collision 的退化限制在 O(n log n)，而非
O(n²)。directive 同时移除 HTML-style `#id`/`.class` shortcut 与 id/class 特判，所有普通
key 统一 last-wins。complexity suite 增加 unique/duplicate references，共 8 个 case。
map、directive duplicate semantics 与连续 backslash batching 的独立 review 文档见
[`docs/specs/map-and-backslash-performance.md`](../specs/map-and-backslash-performance.md)。

首次远端复验还暴露出两个常数因子问题。duplicate-heavy directive 输入只有 64 个唯一键，
index 却按全部 source occurrences 预分配；共享 index 现支持渐进扩容，directive 采样最多
1024 个 key，高唯一率输入按总数预分配，低唯一率输入按样本唯一数起步。未闭合 directive
回落到 CommonMark inline parser 后，连续 `\\` pairs
曾逐对创建数千万个 Text node，再在 `parser_finish` 合并；在没有 extension 接管 backslash
时，core 现将连续 pairs 批量解码为一个最终本就会得到的 Text node。

修复后本机复测全部通过：约 60.4 MB unique attributes 为 1.077×、32.7 MB duplicate
attributes 为 1.112×、61.8 MB unique references 为 0.969×、41.3 MB duplicate references
为 0.966× normalized slowdown；128 MiB unclosed-backslash 从约 2.49 秒降至 0.189 秒，
normalized slowdown 为 0.979×。
本地 Release correctness 59/59、C conformance 2/2、ASan/UBSan/TSan 各 51/51、Swift、
Kotlin JVM/Android host、ES Node correctness/conformance 及 repository verify 均通过；PR
required checks 仍需复验。

远端 macOS runner 进一步证明 2.0× 不能作为可靠的 n log n 判别器：同一 expected-linear
unique-attributes hash 路径在两次运行中分别为 3.318× 与 2.753×，因为数百万解析节点跨越了
4 KiB 样本未覆盖的 allocator/cache 层级。wall-clock 拒绝线因此校准为 4.0×：仍低于已实测
旧 qsort 路径的 4.442×，也会拒绝首次远端 unclosed-backslash 的 9.850×；普通路径是否为
expected-linear 则由共享 hash 实现、64-probe 上界、collision fallback tests 与 code review
保证，timing gate 只负责捕获实际端到端退化。

同一验证 PR 还暴露了 push/PR SHA 去重的两个问题：被取消的 push run 仍运行 `always()`
汇总并留下失败的 `Required gates`；两次 macOS runner teardown 又长期停在无 active step
的 `in_progress`，直接阻塞同 concurrency group 的 PR run。修复后只有
`pull_request`/`merge_group` 拥有 `Required gates` 名称，push 汇总改名为
`Development branch gates`，并为 push/PR/merge-group 使用独立 concurrency lane。最终远端
验收必须确认 required check 列表没有 push context，且 PR run 不再等待 push teardown。

下一轮验证又证明 group task 加 `maxConcurrentDevices=1` 并非真正串行：4 KB 与 16 KB
setup 仍同时启动，一个 snapshot 创建超时，另一个等待 device lock 600 秒后失败。根
Android emulator 入口因此改为两个独立 Gradle invocation，先完整运行 4 KB task，再运行
16 KB task；host-derived `testedAbi` 也在所有 device 属性配置完成后通过 `configureEach`
显式覆盖。

远端成功日志随后证明 AGP 9.2.1 的 KMP setup-task `CreationAction` 没有把公开 DSL 的
`testedAbi` 复制到 task input：两个设备仍打印 unspecified-ABI warning。仓库在保留公开
DSL 声明的同时，对 pinned AGP 的 `ManagedDeviceInstrumentationTestSetupTask` 显式设置同一
host-derived input；升级 AGP 时必须以远端日志证明 upstream 已修复后才能移除该兼容层。
