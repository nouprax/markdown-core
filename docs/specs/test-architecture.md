# Markdown Core 测试架构契约(Phase 7 execution-platform revision)

本文档冻结全仓统一测试架构。后续 phase(尤其 Phase 8–13)必须在该契约内实现;
修改本契约需要先评审本文档,再改实现。

## 1. 根级入口与路由

`pnpm` scripts 只做平台路由:不包含测试 case、fixture、timeout、filter 或预期
结果逻辑。固定入口:

| 入口 | 语义 |
| --- | --- |
| `pnpm test:<platform>` | 直接调用该 execution platform 的具名原生 correctness target |
| `pnpm conformance:<platform>` | 直接调用该 execution platform 的具名原生 conformance target |
| `pnpm benchmark:<platform>` | 直接调用该 execution platform 的具名原生 benchmark target；没有可信测量环境的平台不暴露空 target |

约束:

- 不提供跨 incompatible hosts 的无 platform aggregate。Required CI 直接列举并执行
  各 family 的 platform tasks；`verify` 只聚合可在单一 checkout 完成的静态检查。
- 任何 `test:*` 必须运行该平台当前声明支持的完整 correctness suites;任何
  `conformance:*` 必须运行该平台完整 contract checks；任何 `benchmark:*` 必须运行
  完整 benchmark workloads。三者都不得退化为 build/lint、不得
  静默 skip、不得用空/no-op task 为尚未实现的 target 假装通过。因此
  platform target 必须在 product 引入的同一阶段接入。
- pnpm 没有中间 routing layer 或 language aggregation，只拥有三个 task family 到 execution
  platform 的一层映射。平台标识包含语言，例如 `swift-macos`、`kotlin-jvm`、
  `es-browser`。禁止追加 suite、`:full`、root suite matrix 或通用 family router；suite
  discovery/filter 属于原生 target。不存在公开 `stress` task。
- Kotlin Linux x64 只由 required CI 的 `ubuntu-latest` runner 验收；仓库不提供
  Apple `container`、Rosetta 或其他本机模拟入口作为替代证据。平台 target 在不支持
  的 host 上必须失败，不得静默通过。macOS ARM64 与 Linux x64 的 required-CI
  platform jobs 共同构成 Kotlin Native 全量验收证据。
- Swift iOS Build Test producer 使用 `generic/platform=iOS Simulator` 构建 test products，不把
  artifact 绑定到 runner 上某个设备 UDID、具体手机型号或移动的 `OS=latest` alias。每个纯 Test
  consumer 在自己的 macOS runner 上发现已安装 iOS runtime 和可用 iPhone；device set 为空时用
  已安装 runtime 创建临时 simulator，boot 完成后才执行 `test-without-building`。禁止假设 hosted
  image 永远预创建某个型号；缺失 runtime 时必须输出 runtime/device/SDK diagnostics 并失败。
- Android 开发机入口由 repo 中的 Gradle Managed Devices group 定义固定 Pixel 10 Pro XL
  和 64-bit Google APIs images：API 36 同时覆盖 4 KB 与 16 KB page size，不读取 Android
  Studio 已有 AVD、serial 或开发者本机配置。correctness 与 conformance 使用原生
  instrumentation runner arguments 保持 selection 互斥：

  ```sh
  pnpm test:kotlin-android-emulator
  pnpm conformance:kotlin-android-emulator
  ```

  两个入口保留给本机原生开发/IDE 调试。Required CI 不调用 GMD test task：单一 Linux build
  producer 以 `packageAndroidDeviceTest` 只构建 x86_64 instrumentation APK，写入 manifest 和
  SHA-256 后上传。随后 `{4K,16K} × {correctness,conformance}` 四个 `fail-fast: false` jobs
  下载同一 APK，各自创建一个无 snapshot AVD、`adb install` 并直接执行 `am instrument`。
  consumer 不安装 JDK、Gradle、Node、NDK 或 CMake，不运行 publication，也不携带其他 suite。
  每个 job 拥有并清理自己的 emulator lifecycle，失败时上传 emulator stdout、logcat、getprop
  与 instrumentation output。

  本机 GMD cache 需要回收时只通过显式 maintenance task
  `pnpm clean:kotlin-android-emulator` 委托原生 `cleanManagedDevices`；test 与
  conformance 不得自动 depends/finalize cleanup，SDK system images 也不属于该 task。
- 构建入口独立:`pnpm build:c`、`pnpm build:swift`(`swift build`)。
- 仓库级契约检查(`check:contracts`、`check:gradle-model`)与审计
  (`audit:tests`、`audit:packages`)属于 `verify` 链,不属于 correctness 测试
  路由。

## 1.1 Required CI artifact DAG

Required CI 使用 build-once/test-many DAG，而不是把 build 和多个 suite 顺序塞进同一个 runner：

1. `Health Check - <scope>` jobs 并行完成 repository/C/ES/Kotlin/Swift 的 formatting、lint、contract
   与 topology audit；显式 health barrier 成功后才允许任何 build producer 启动；
2. `Build - <platform>` host-specific producers 只构建可交付产品，例如 tests-off C product tree、
   ES dist/WASM、Kotlin staged publications、Swift product 与声明的 deployment targets；native/cinterop
   产品必须由兼容 host 构建，禁止伪装成单 host cross-build；
3. 每个 artifact 都包含 source SHA、类型/target manifest 和 digest；Unix 可执行位通过 tar 保留；
4. `Build Test - <platform>` 层在 Build barrier 后编译/链接 CTest tree、SwiftPM/Xcode test products、
   Kotlin JVM/Android-host/Native test products、Android instrumentation APK、ES conformance fixtures 和
   ASan/UBSan/TSan instrumented trees；package contents 与真实 consumer build contract 也属于该层。
   Build Test 不是对 product artifact 做一次 checksum 后就宣称测试已构建；
5. `Test - <platform>` correctness、
   conformance、sanitizer、browser、simulator 与 page-size leaves 一次性并行启动，各自下载 artifact，
   只安装运行环境并调用原生 runner 的 no-build 模式；
6. `Tests - Ready` fail-closed 聚合 test 层，稳定的 `Required gates` 只依赖该聚合点；任一前置层失败
   导致 test skipped 时，聚合点也必须失败而不是把 skipped 当成成功；
7. consumer job 中出现 compiler、Gradle build task、`swift build`、`xcodebuild build`、`emcc` 或
   publication 即为架构回归；cache 只能加速 producer，不能代替可校验 artifact；
8. packaging/deployment/consumer contract 本身属于 build/resolve 验证时可保持独立 contract job，
   但不得混入纯 runtime suite，也不得让 runtime suite 为它重复构建。

Build artifact 的配置 cache 会随 artifact 一起跨 runner 传播。若产品 Build 显式关闭 tests（例如
`MARKDOWN_CORE_TESTS=OFF`），Build Test 必须显式覆盖为 tests ON，并在上传前断言原生 runner 的
test inventory 非空；不能依赖 configure default，也不能把 CTest 的空图 exit 0 当成成功。

## 2. Runner ownership

每个平台的 suite graph 只有一个事实来源;不得用 pnpm、shell、Make 或另一平台
runner 重建第二份:

| 平台 | 事实来源 |
| --- | --- |
| C | CTest(唯一 CMake graph,presets + labels) |
| Swift | SwiftPM `MarkdownCoreTests` 与 `MarkdownCoreConformanceTests` test targets、`MarkdownCoreBenchmarks` executable；iOS 由 xcodebuild 按 target 选择 |
| Kotlin | Gradle/KMP 具名 correctness/conformance tasks，例如 `jvmTest`/`jvmConformanceTest`、`macosArm64Test`/`macosArm64ConformanceTest`；Android instrumentation 使用原生 class/notClass selection |
| ES | package-native Node/browser correctness scripts、独立 conformance script 与 benchmark script |

`make test` 委托 CTest correctness preset,`make bench` 委托 benchmark preset;
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

`stress` 只描述输入压力，不是公开 task family 或 suite taxonomy。同一种 large document、
deep nesting 或 repeated parse/release shape 必须按目的注册两份独立执行：
correctness 下的 `robustness` cases 断言结果、错误与生命周期；benchmark 下的同名
workloads 负责 warmup/repeat、计时、吞吐量、relative scaling 与性能基线。两者可以
复用确定性 input generator，但不得复用测试注册、断言或执行入口。

C 侧 CTest label taxonomy(每个测试恰有一个 label):

| Label | 覆盖 |
| --- | --- |
| `api` | legacy engine API harness(`api_engine`) |
| `facade` | facade 行为与并发 correctness(`facade_concurrent_first_parse`、`facade_concurrent_stress`) |
| `conformance` | 公开 facade/schema shape 与 reviewed canonical dumps(`facade_native`、`facade_dump_cli`)；不进入 correctness preset |
| `consumer` | C++ consumer 编译/链接/运行(`consumer_facade_cplusplus`) |
| `spec` | CommonMark spec、smart punctuation、entities(全部为 canonical AST dump 断言) |
| `extensions` | GFM/formula/directive extension specs 与 option gates |
| `regression` | 固定回归语料与 registry 生命周期(`regression_commonmark`、`regression_registry_lifecycle`) |
| `pathological` | 逐 case 注册的对抗输入与 directive 复杂度(`pathological_*`) |
| `fuzz` | 确定性 fuzz smoke(`fuzz_smoke`) |
| `packaging` | corpus/workspace 政策 guard(`packaging_corpus_guard`) |
| `benchmark` | 独立调度的性能 workloads(`benchmark_*`) |

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
| benchmark | `benchmark:<platform>` | `benchmark:c-host` | `benchmark:swift-macos` | `benchmark:kotlin-jvm` | `benchmark:es-node` |

CI 必须分别调用 correctness 与 conformance 平台入口；确需按功能/成本诊断分片时直接使用
原生 label/filter 机制(如 `-L spec`、`-L pathological`、`--tests`)，不得为
这些 filters 新建 pnpm suite task 或另建 case 清单。

C 数据驱动 runner 自身提供第二级 discovery:`spec_runner --list/--example/--section`、
`pathological_runner --list/--case`、`complexity_runner --list/--case`、
`bench_runner --list/--workload`、`concurrency_runner --case`(三个固定 case:
`first_parse`/`stress`/`lifecycle`,逐一注册为 CTest 测试)。CMake 中注册的
case 清单由 `scripts/audit-test-topology.sh` 与 runner `--list` 输出强制一致。

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
  spec/extension 120–240s、complexity 120s、fuzz 240s、benchmark 600s);Swift
  由 Swift Testing traits 持有。
- Expected failure 必须显式建模(当前无);禁止静默 skip;缺少必需工具时在
  configure 阶段失败(`MARKDOWN_CORE_TESTS=ON` 而无库目标时 FATAL_ERROR),不
  降级跳过。
- 临时文件只进入 build 目录;进程清理由 runner 负责(in-process 转换,无子进
  程残留;CLI 测试通过管道等待退出)。
- 串行/资源锁:benchmark 与 complexity 测试标记 `RUN_SERIAL`;benchmark preset
  以单 job 执行。
- Performance 测量固定 warmup/repeat(complexity:短样本 median-of-3、长样本
  单次完整 parse;benchmark:warmup 1 + repeats 5 取中位数)。complexity 以
  4 KiB → 128 MiB endpoint 的每字节成本断言渐近趋势；benchmark 使用 doubling
  相对比率；均不使用绝对 wall-clock 阈值。
- 诊断输出确定性:不输出指针、环境路径、locale 或时间戳(benchmark 的时间数
  值除外,其格式固定)。
- 各平台 helper 使用本平台原生实现(C:`packages/markdown-core/tests/support/`;Swift:test target 内
  helper),不引入跨语言 test bridge、新 test framework 或新 package 依赖。

## 7. Benchmark 与 corpus 政策

- Benchmark 是正常但独立调度的 CTest suite(label `benchmark`),覆盖
  representative documents、large input(采样块重复至历史 Pro Git 语料同一量
  级)、deep nesting、extensions 与 adversarial size-doubling cases。
- 输入全部离线且确定:tracked samples(`packages/markdown-core/benchmarks/samples/`)
  或进程内确定性生成;运行时禁止 clone/download,禁止把生成输入写入源码树。
- CI 在 Build Test phase 预构建 benchmark products，并在 Test phase 与 correctness/conformance
  并行执行。C、Kotlin/JVM、ES/Node 与 Swift/macOS benchmark 必须是四个独立 no-build jobs；
  不得把无关 toolchain 和 workload 顺序塞入同一 runner。执行结果汇入 required gate；各 job
  分别上传受限 JSON 数值，再由不 checkout PR code 的 privileged commenter 汇总。数值趋势保持
  informational，不使用 hosted-runner 绝对 wall-clock 阈值。Diff baseline 必须匹配 PR 精确 base
  SHA 的成功 main CI；size diff 是确定性字节差异，跨 hosted-runner 的 perf diff 只作方向性证据。
- 外部 corpus 只能按 `packages/markdown-core/tests/corpora/README.md` 的
  manifest/license/hash 政策一次性导入;
  `packaging_corpus_guard`/`benchmark_corpus_guard` CTest tests 与
  `scripts/audit-test-topology.sh` 强制该政策。
- 长时间 fuzz campaign 是显式非默认任务(`make afl`、`make libFuzzer`),复用
  `packages/markdown-core/tests/core/` 下的 harness 与 corpus;确定性 fuzz
  smoke(parse/traverse/dump/free)属于 correctness(label `fuzz`)。

## 8. 审计

`scripts/audit-test-topology.sh`(`pnpm audit:tests`,verify 链与 CI 均执行)
只验证会改变质量结论的事实：四个平台都接入共享 canonical contract，测试与 benchmark
不在运行时获取可变网络输入，外部 corpus 具备 manifest/license/hash，CTest 的 required
labels 非空且没有 disabled test，correctness/conformance/benchmark selection 互斥，runner
discovery 与 CTest registration 一致，Swift suite discovery 非空。

源码目录、文件合并方式、pnpm script 的具体实现文本、router/alias 命名、Android managed
device 的内部编排方式，以及维护时选择的 GitHub Action major 都不是 CI 合同。这些内容可在
设计或维护文档中记录，但不得作为 required gate 的静态字符串 policy。
