# Markdown Core 测试与流水线架构契约

本文档冻结全仓统一测试架构。后续 phase(尤其 Phase 8–13)必须在该契约内实现;
修改本契约需要先评审本文档,再改实现。

## 1. 根级入口与路由

`pnpm` scripts 只做平台路由:不包含测试 case、fixture、timeout、filter 或预期
结果逻辑。固定入口:

| 入口 | 语义 |
| --- | --- |
| `pnpm test:<platform>` | 直接调用该 execution platform 的具名原生 correctness target |
| `pnpm conformance:<platform>` | 直接调用该 execution platform 的具名原生 conformance target |
| `pnpm benchmark:c-host` | 显式运行隔离的 C 本地性能测量工具；只输出观测值，不给出 correctness/回归结论 |

约束:

- 不提供跨 incompatible hosts 的无 platform aggregate。Required CI 直接列举并执行
  各 family 的 platform tasks；`verify` 只聚合可在单一 checkout 完成的静态检查。
- 任何 `test:*` 必须运行该平台当前声明支持的完整 correctness suites;任何
  `conformance:*` 必须运行该平台完整 contract checks。两者都不得退化为
  build/lint、不得静默 skip、不得用空/no-op task 为尚未实现的 target 假装通过。因此
  platform target 必须在 product 引入的同一阶段接入。
- `benchmark:c-host` 是 developer 实验入口，不进入 required CI、release 或 test
  artifact。独立的 PR benchmark workflow 可以从同一个 C runner 采集固定 workload，
  但不得以绝对时间、跨 runner 差值或少量样本比率改变退出状态。如果观测暴露风险，
  必须把风险转化为可复现的结构、资源上界、操作计数或语义 invariant 后才可成为 gate。
- pnpm 没有中间 routing layer 或 language aggregation，只拥有 task family 到 execution
  platform 的一层映射。平台标识包含语言，例如 `swift-macos`、`kotlin-jvm`、
  `es-browser`。禁止追加 suite、`:full`、root suite matrix 或通用 family router；suite
  discovery/filter 属于原生 target。不存在公开 `stress` task。
- Kotlin Linux x64 只由 required CI 的 `ubuntu-latest` runner 验收；仓库不提供
  Apple `container`、Rosetta 或其他本机模拟入口作为替代证据。平台 target 在不支持
  的 host 上必须失败，不得静默通过。macOS ARM64 与 Linux x64 的 required-CI
  platform jobs 共同构成 Kotlin Native 全量验收证据。
- Android emulator 由 repo 中的 Gradle Managed Devices group 定义固定 Pixel 10 Pro XL
  和 64-bit Google APIs images：API 36 同时覆盖仍占主流的 4 KB page size 与
  新设备方向的 16 KB page size；不得读取 Android Studio 已有 AVD、serial 或开发者
  本机配置。
  AGP 自动 provision、启动、恢复和清理 device。correctness 与 conformance 分别调用
  同一个具名 managed-device instrumentation target，并使用原生 runner arguments
  保持 selection 互斥：

  ```sh
  pnpm test:kotlin-android-emulator
  pnpm conformance:kotlin-android-emulator
  ```

  两个入口都以独立 Gradle invocation 依次运行 4 KB 与 16 KB managed-device task；不得
  用 device group 并发 setup 再以 `maxConcurrentDevices=1` 等锁，因为 snapshot setup
  仍可能并发并造成 lock/snapshot timeout。

  GMD 在 test 完成后停止 emulator，并以 snapshot 恢复干净状态；可复用的 managed
  AVD/snapshot cache 默认保留。需要回收时只通过显式 maintenance task
  `pnpm clean:kotlin-android-emulator` 委托原生 `cleanManagedDevices`；test 与
  conformance 不得自动 depends/finalize cleanup，SDK system images 也不属于该 task。

  本机与 required CI 使用完全相同的两个入口；环境只需提供受支持的 JDK、Android
  SDK/emulator 与硬件虚拟化。CI 不使用第三方 action 重建 AVD lifecycle。
- 构建入口独立:`pnpm build:c`、`pnpm build:swift`(`swift build`)。
- 仓库级契约检查(`check:contracts`、`check:gradle-model`)与审计
  (`audit:tests`、`audit:packages`)属于 `verify` 链,不属于 correctness 测试
  路由。

## 2. Runner ownership

每个平台的 suite graph 只有一个事实来源;不得用 pnpm、shell、Make 或另一平台
runner 重建第二份:

| 平台 | 事实来源 |
| --- | --- |
| C | CTest(唯一 CMake graph,presets + labels) |
| Swift | SwiftPM `MarkdownCoreTests` 与 `MarkdownCoreConformanceTests` test targets；iOS 由 xcodebuild 按 target 选择 |
| Kotlin | Gradle/KMP 具名 correctness/conformance tasks，例如 `jvmTest`/`jvmConformanceTest`、`macosArm64Test`/`macosArm64ConformanceTest`；Android instrumentation 使用原生 class/notClass selection |
| ES | package-native Node/browser correctness scripts 与独立 conformance script |

`make test` 委托 CTest correctness preset；`make bench`/`pnpm benchmark:c-host`
显式配置独立的 `build/benchmark` tree，普通 test build 不编译 benchmark runner。
Makefile 不实现第二套测试或 benchmark runner。sanitizer 任务
(`make asan-test`/`ubsan-test`/`tsan-test`、CI asan/ubsan/tsan jobs)复用同一
graph 的 `asan`/`ubsan`/`tsan` presets。TSan preset(Phase 10)在支持的平台上
验证冻结的并发契约;TSan 不可用的平台仍通过 default preset 运行同一批原生
并发 regression,不得静默跳过。

## 3. Suite/workload taxonomy

原生 runner 内部的 correctness 类别(跨端统一；平台可扩展，但相同语义不得改名):

`api`、`ast`、`consumer`、`errors`、`ownership`、`unicode`、`robustness`、
`pathological`、`packaging`。

`conformance` 是独立验证通道，回答实现是否符合公开 facade/schema、field shape、
nullability、scope 和 binding mapping contract。它不是传统 correctness suite，
不得由 `test` 隐式发现；它仍是 required release gate，并由原生 runner 独立选择。

`stress` 只描述输入压力，不是公开 task family 或 suite taxonomy。Large document、
deep nesting、repeated parse/release 与 adversarial shape 必须在 correctness 中断言
结果、错误、生命周期或可计算资源上界。相似输入可以被本地 benchmark 复用，但测量
结果不能替代这些断言，也不能建立另一份 required 注册表。

C 侧 CTest label taxonomy(每个测试恰有一个 label):

| Label | 覆盖 |
| --- | --- |
| `api` | legacy engine API harness(`api_engine`) |
| `facade` | facade 行为与并发 correctness(`facade_concurrent_first_parse`、`facade_concurrent_stress`) |
| `conformance` | 公开 facade/schema shape 与 reviewed canonical dumps(`facade_native`、`facade_dump_cli`)；不进入 correctness preset |
| `consumer` | C++ consumer 编译/链接/运行(`consumer_facade_cplusplus`) |
| `spec` | CommonMark spec、smart punctuation、entities(全部为 canonical AST dump 断言) |
| `extensions` | GFM/formula/directive extension specs 与 option gates |
| `regression` | 固定回归语料、实例生命周期与严格 OOM 语义(`regression_commonmark`、`regression_instance_lifecycle`、`regression_strict_oom`) |
| `pathological` | 逐 case 注册的对抗输入、固定资源上界与语义断言(`pathological_*`) |
| `fuzz` | 确定性 fuzz smoke(`fuzz_smoke`) |
| `packaging` | corpus/workspace 政策 guard(`packaging_corpus_guard`) |

`benchmark` label 只存在于显式 `MARKDOWN_CORE_BENCHMARKS=ON` 的 CMake graph；本地
入口与独立 PR benchmark 可以创建该 graph，默认、sanitizer 与 required-CI artifact
graph 中不存在该 label 或 runner。

Swift correctness suites:`api`、`errors`、`unicode`、`ownership`、
`robustness`、`consumer`；`ConformanceSuite` 位于独立
`MarkdownCoreConformanceTests` target。测试与 consumer package 位于
`packages/swift-markdown-core/Tests/`，只通过公开 Swift API 验证
C-to-Swift node/field/nullability/scope/error/ownership mapping。

Kotlin correctness suites:`api`、`errors`、`unicode`、`ownership`、`robustness`、
`consumer`、`packaging`；`AstTest` 只由具名 conformance tasks 选择。`commonTest` 复用于 JVM、Android host、Android emulator、
macOS ARM64 与 Linux x64；`AstTest` 的 focused cases 因而在所有 runtime 上验证
native/JNI-to-Kotlin schema mapping；consumers、
compile contracts 与内部 `android-runtime` module 均由
`packages/kotlin-markdown-core/` 独占。`verifyKotlinNativePackaging` 验证 desktop
JNI payload 和 Android 四 ABI AAR。

ES Node correctness suites:`api`、`ast`、`consumer`、`errors`、`ownership`、
`robustness`、`unicode`、`types`、`packaging`；browser target 提供 `api`；独立
`run-conformance.mjs` target 提供 `conformance`。correctness runner 支持 `--target`、
`--list` 与 `--suite`；
`types` 与 runtime `consumer` 都安装实际 `npm pack` tarball，TypeScript 使用
NodeNext 从 package `exports.types` 解析声明，不允许通过 `paths` 直连仓库
`dist/index.d.ts`；
browser target 在真实 headless Chrome/Chromium 中通过 HTTP ESM/WASM 加载路径
执行同步 parse，不能由 Node suite 替代。

## 4. Discovery / filter 契约

每个平台 runner 必须支持列出 suites/cases、按名称/label 单独运行、机器可读退出
状态和可定位 diff。等价命令映射:

| 操作 | pnpm | C 原生 | Swift 原生 | Kotlin 原生 | ES 原生 |
| --- | --- | --- | --- | --- | --- |
| correctness | `test:<platform>` | `test:c-host` | `test:swift-macos` / `test:swift-ios-simulator` | `test:kotlin-*` 平台 targets | `test:es-node` / `test:es-browser` |
| contract conformance | `conformance:<platform>` | `conformance:c-host` | `conformance:swift-macos` / `conformance:swift-ios-simulator` | `conformance:kotlin-*` 平台 targets | `conformance:es-node` |
| 列出测试 | — | `ctest --test-dir build/cmake -N` | `swift test list` | `scripts/gradle.sh :packages:kotlin-markdown-core:tasks --group verification` | `node packages/es-markdown-core/scripts/run-tests.mjs --list` |
| 按 suite/label 运行 | 不提供 pnpm task | `ctest --preset correctness -L spec` 等 | `swift test --filter` / `xcodebuild -only-testing` | 对应 platform test task 加 `--tests` 或 instrumentation runner arguments | package runner `--target <target> --suite <suite>` |
| 按名称运行 | — | `ctest --preset correctness -R pathological_backticks` | `swift test --filter <test>` | `jvmTest --tests <class.method>` | Node native `--test-name-pattern` |
| 本地性能测量 | `benchmark:c-host` | `benchmark:c-host` | — | — | — |

Required CI 必须分别调用 correctness 与 conformance 平台入口，且不得调用 performance
measurement 入口；确需按功能/成本诊断分片时直接使用
原生 label/filter 机制(如 `-L spec`、`-L pathological`、`--tests`)，不得为
这些 filters 新建 pnpm suite task 或另建 case 清单。

C 数据驱动 test runner 自身提供第二级 discovery:`spec_runner --list/--example/--section`、
`pathological_runner --list/--case`、`concurrency_runner --case`(三个固定 case:
`first_parse`/`stress`/`lifecycle`,逐一注册为 CTest 测试)。CMake 中注册的
case 清单由 `scripts/audit-test-topology.sh` 与 runner `--list` 输出强制一致。
Opt-in `bench_runner --list/--workload` 只描述本地与独立 PR 测量，不参与 test topology
audit。

IDE 契约:仓库提交 `CMakePresets.json`(configure/build/test presets),
VS Code/CLion 直接消费;Xcode 通过 SwiftPM 发现 Swift Testing suites;
IntelliJ/Android Studio 消费 Gradle test tasks(Phase 12 起)。Kotlin library 额外提供
developer-only 根 Gradle `allKotlinTests`，聚合当前 host 可执行的具名 correctness/
conformance tasks 与两台 Android managed-device 全量测试；shared IDE configuration
只调用该 task，不建立 sample app。该入口不是 pnpm/CI/release routing，不能替代各
execution platform 独立的 required gate，也不复制 suite/case discovery。除此之外不依赖
任何个人 IDE state。

## 5. Shared conformance 与 package-local correctness 契约

- Canonical Markdown/`.ast` conformance data 只有一份，位于
  `specs/canonical-ast/`；`manifest.json` 是唯一 case list，并显式冻结 paths、
  parse options、顺序、编码/换行和 coverage tags。该目录不含 runner。
- C、Swift、Kotlin、ES 的现有原生 conformance targets 使用各自公开
  parse/immutable AST/Visitor/Walker/TreeDumper 路径枚举同一 manifest；bindings
  不调用 C dump/test runner、不读取另一 binding 输出，也不以 dump 构造生产 AST。
- Swift test bundle 由 SwiftPM build-tool plugin 在 plugin work directory 从 root
  spec source 生成；Kotlin common tests 由 cacheable Gradle task class 从同一
  manifest 生成 build-only Kotlin data；ES package 由 `preconformance` lifecycle
  生成 package-local build output。测试只读取各自构建产物，均不依赖 repo cwd、
  网络、越界 symlink 或 tracked 平台副本。
- 仓库根目录不得存在 `tests/`、跨 package test harness 或职责不明的 runner。
  除 root canonical contract data 外，consumer、compile contract、correctness
  fixtures 与 packaging tests 必须位于唯一 owning package 内。
- C spec/extension fixtures 位于 `packages/markdown-core/tests/fixtures/`
  (CommonMark 32-backtick example 格式)。自 Phase 8 起 expected block 一律是
  canonical AST dump;`spec_runner` 对每个例子解析一次、dump 两次(断言 dump
  确定性)并与 expected byte-for-byte 比较。`spec_runner --rewrite` 是显式维护
  模式,用当前 parser 重新生成 expected;生成的 fixture diff 必须经人工审查后
  才能提交,不得用于隐藏未经批准的 parser drift。

## 6. 通用执行策略

- 比较一律为 UTF-8 byte comparison;golden 比较 byte-for-byte,失败时输出可定
  位的逐行 diff。没有任何 canonicalization/normalization 层:Phase 8 删除
  renderer 断言后,唯一的比较对象是 canonical AST dump 与 typed accessor 值,
  规范化过程无从隐藏 drift。
- 文本产物使用 LF 与单一 final newline。
- Timeout 由 runner 声明层持有:CTest `TIMEOUT` 属性(pathological 30s、
  spec/extension 120–240s、fuzz 240s);Swift
  由 Swift Testing traits 持有。
- Expected failure 必须显式建模(当前无);禁止静默 skip;缺少必需工具时在
  configure 阶段失败(`MARKDOWN_CORE_TESTS=ON` 而无库目标时 FATAL_ERROR),不
  降级跳过。
- 临时文件只进入 build 目录;进程清理由 runner 负责(in-process 转换,无子进
  程残留;CLI 测试通过管道等待退出)。
- Build-once/run-elsewhere producer 必须从 CI 专属的空 staging/build tree 生成制品；
  不得打包共享 `.build` 或未清理的 Gradle/CMake tree，因为已删除 target 的陈旧二进制
  也会因此进入制品。Swift 使用 `build/ci-swift-tests` scratch paths，Kotlin 清空
  `ci-test-artifact` staging root，C 在配置前重建对应 CTest tree。
- Performance measurement 可固定 warmup/repeat 并输出中位数或 doubling ratio。独立 PR
  comment 可以显示 exact-base 百分比，但只能用于提出假设，不得提供警告阈值或
  pass/fail。复杂度门禁必须直接验证通用 invariant；当前 reference expansion 以 AST
  payload/source byte ratio 断言，其余 adversarial cases 断言固定 AST
  shape/content/lifecycle 并由宽松 timeout 防止失控执行。
- 测试诊断输出确定性:不输出指针、环境路径、locale、时间戳或 wall-clock 数值。
- 各平台 helper 使用本平台原生实现(C:`packages/markdown-core/tests/support/`;Swift:test target 内
  helper),不引入跨语言 test bridge、新 test framework 或新 package 依赖。

## 7. 本地与 PR 性能观测、corpus 政策

- Benchmark 是显式 C 工具，不是测试层、required gate 或发布流水线。它使用独立
  benchmark preset 与 build tree；本地入口覆盖
  representative documents、large input(采样块重复至历史 Pro Git 语料同一量
  级)、deep nesting、extensions 与 adversarial size-doubling cases。
- Swift、Kotlin 与 ES 不保留原先为 PR metrics collector 服务的少量 wall-clock/RSS
  scripts；它们既没有受控环境，也没有趋势存储，并且 Swift executable target 会被
  `swift test` 无条件编译，直接污染测试成本与 artifact graph。
- 输入全部离线且确定:tracked samples(`packages/markdown-core/benchmarks/samples/`)
  或进程内确定性生成;运行时禁止 clone/download,禁止把生成输入写入源码树。
- 独立 `PR Benchmark` workflow 只运行一个版本化 C parser workload 和 shared-library
  size。它先按 exact base SHA 查找 main 发布或同一 PR 先前发布的 baseline；不存在时
  checkout exact base、现场构建并以 `pr-benchmark-baseline-<SHA>` 发布。comparison
  artifact 同时携带经过 schema 校验的 base/head 数值，不依赖普通 main CI 是否生成过
  性能 artifact。
- PR producer 只有 read 权限；privileged `workflow_run` commenter 不执行 PR 中的代码，
  只读取名称、数量、大小和字段均受限的 JSON。评论没有 `boundary` 维度，因为固定
  workload 只测量一个 parser operation。hosted-runner timing/RSS 只作为方向性观察，
  binary size 只在工具链输入相同时可确定比较，任何一项都不进入 merge gate。
- 若将来需要性能门禁，必须先提供受控且可重复的测量环境、统计设计、版本化 workload
  与明确 false-positive budget，并通过架构评审；hosted runner 的跨 run 对比不满足要求。
- 外部 corpus 只能按 `packages/markdown-core/tests/corpora/README.md` 的
  manifest/license/hash 政策一次性导入;
  correctness 中的 `packaging_corpus_guard` 与 `scripts/audit-test-topology.sh` 强制该政策；
  opt-in C benchmark graph 也复用同一 corpus guard。
- 长时间 fuzz campaign 是显式非默认任务(`make afl`、`make libFuzzer`),复用
  `packages/markdown-core/tests/core/` 下的 harness 与 corpus;确定性 fuzz
  smoke(parse/traverse/dump/free)属于 correctness(label `fuzz`)。

## 8. 审计

`scripts/audit-test-topology.sh` 的无参数形式(`pnpm audit:tests`、verify 链与 repository
health-check)只做无编译的仓库契约审计；C test-artifact producer 在完成既有 build 后把
CTest tree 路径传给同一脚本，追加动态 inventory/discovery 交叉检查。该分层禁止 health-check
为“审计”预先重复构建 C 或 Swift。

审计只验证会改变质量结论的事实：四个平台都接入共享 canonical contract，测试与性能测量
不在运行时获取可变网络输入，外部 corpus 具备 manifest/license/hash，默认 CTest 的
required labels 非空且没有 disabled test、没有 performance label，correctness/conformance
selection 互斥，test runner discovery 与 CTest registration 一致，Swift suite discovery
非空。`scripts/audit-ci-policy.sh` 另行强制 required CI 不含 benchmark job、测试制品不携带
benchmark payload，并冻结独立 PR benchmark 的 exact-SHA fallback、artifact 和权限边界。

源码目录、文件合并方式、pnpm script 的具体实现文本、router/alias 命名、Android managed
device 的内部编排方式，以及维护时选择的 GitHub Action major 都不是 CI 合同。这些内容可在
设计或维护文档中记录，但不得作为 required gate 的静态字符串 policy。
