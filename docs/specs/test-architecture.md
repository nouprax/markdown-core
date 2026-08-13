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
| `pnpm coverage:<platform>` | 在该 execution platform 的插桩构建上运行其 correctness 与 conformance suites，并把 toolchain-native 报告交给全仓唯一的 coverage gate |

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
6. `Tests - Ready`、`Benchmarks - Ready` 与 `Coverage - Ready` 分别 fail-closed 聚合 test、benchmark
   与 coverage 层，稳定的 `Required gates` 同时依赖这三个并列聚合点；任一前置层失败导致 consumer
   skipped 时，对应聚合点也必须失败而不是把 skipped 当成成功。coverage 层直接依赖 health barrier
   而不是 build artifact：它必须自行编译插桩树，这正是 test consumer 层所禁止的，因此它是独立层
   而非 test consumer；
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

C 侧 CTest label taxonomy(每个测试恰有一个主 suite label;`complexity` 是
唯一的次级调度 label):

| Label | 覆盖 |
| --- | --- |
| `api` | legacy engine API harness(`api_engine`) |
| `facade` | facade 行为与并发 correctness(`facade_concurrent_first_parse`、`facade_concurrent_stress`) |
| `conformance` | 公开 facade/schema shape 与 reviewed canonical dumps(`facade_native`、`facade_dump_cli`)；不进入 correctness preset |
| `consumer` | C++ consumer 编译/链接/运行(`consumer_facade_cplusplus`) |
| `spec` | CommonMark spec、smart punctuation、entities(全部为 canonical AST dump 断言) |
| `equivalence` | session 增量编辑 replay 与 one-shot parse 的 dump 等价 + delta mirror 校验(`equivalence_*`) |
| `extensions` | GFM/formula/directive extension specs 与 option gates |
| `regression` | 固定回归语料与 registry 生命周期(`regression_commonmark`、`regression_registry_lifecycle`) |
| `pathological` | 逐 case 注册的对抗输入与 directive 复杂度(`pathological_*`) |
| `complexity` | `pathological_complexity_*` 附加的次级调度 label:sanitizer presets 用它排除 process-CPU 复杂度 gate,这些 case 的主 label 仍是 `pathological` |
| `fuzz` | 确定性 fuzz smoke(`fuzz_smoke`) |
| `packaging` | corpus/workspace 政策 guard(`packaging_corpus_guard`) |
| `benchmark` | 独立调度的性能 workloads(`benchmark_*`) |

Swift correctness suites:`api`、`errors`、`unicode`、`ownership`、
`robustness`、`text`、`edits`、`depth`、`consumer`；`ConformanceSuite` 位于独立
`MarkdownCoreConformanceTests` target。测试与 consumer package 位于
`packages/swift-markdown-core/Tests/`，只通过公开 Swift API 验证
C-to-Swift node/field/nullability/scope/error/ownership mapping。
`edits` 覆盖 `Document.edit` 契约:streaming/clean-boundary/kind-change 的
id-stability、(series, id, revision) 等值语义、空 delta 纯位移、delta 的
children-before-parents postorder、被 edit 读过的 document 仍可自答,以及
模拟真实 LLM 消费端的 conflated-streaming 驱动(多 turn、不规律 render tick、
20-30 token 量级消息混合小 flush、裸字符偏移切点(mid-word/mid-marker/块边界
换行之间)、turn 边界已定稿块冻结、Σ|delta| 近线性上界断言;三端共用同一确定性
发生器,突发形状逐条一致)；`depth` 覆盖超出调用栈预算的对抗性嵌套。
`ConformanceSuite` 另以 per-line append 通过 `Document.edit` 回放 manifest
corpus，逐 commit 校验 dump 等价与 delta 完整性(id 不重复、delta 未命名的
节点 revision 不变、retired id 从树中消失)。

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
`pathological_runner --list/--case`、`concrete_runner --list/--case`、
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
  parse/immutable AST/MarkupVisitor/MarkupWalker/MarkupDumper 路径枚举同一 manifest；bindings
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

## 5.0 外部权威对标门禁

仓库里除这两个门禁外，全部测试都是拿 Markdown Core 和它自己比。spec fixtures 看着
像外部权威（装的是 CommonMark/GFM 官方例子），但 expected 块是本解析器自己生成的
canonical AST dump，只钉住"别再变"，不证明"是对的"。

| 门禁 | 权威 | 覆盖的语义域 |
| --- | --- | --- |
| `pnpm check:upstream-parity` | cmark-gfm 0.29.0.gfm.13（commit 锁定） | CommonMark / GFM 基础语言 |
| `pnpm check:mdast-parity` | unified / remark（pnpm lockfile 锁定） | directive、math、footnote 位置、引用链接模型 |

两者都要求：差异要么是缺陷，要么是登记在 `specs/*/deltas.json` 并写进
`canonical-ast.md` 的有意差异；**已登记的差异必须仍然复现**，否则判失败——上游哪天
把某条修了，会变成需要评审的事件而不是一条没人再看的登记。未映射的节点种类同样判
失败：两个未映射的种类会渲染成相同字符串从而"比较相等"，那是最容易假通过的形态。

### 语料范围：oracle 能判的 golden 一律送进去

**凡是某个 oracle 有能力判定的 golden 语料，必须出现在该 oracle 的 corpus 里。**
一份从未被任何 oracle 读过的 golden，只是"输出没变过"的记录——它同样忠实地钉住
缺陷和正确行为，而且分不出哪个是哪个。这条规则曾经被违反：网关最初只跑 `spec.txt`，
`regression.txt`、`extensions.txt`、`smart_punct.txt` 与全部 extension fixture
从未对过外部权威。

反过来，**oracle 没有能力判定的东西不得登记为分歧**。登记会读成"权威不同意"，而
实际情况是它根本没被问到。这类范围写在 `deltas.json` 的 `oracle.notAnAuthorityFor`
里，不写进分歧表。

### 一条 delta 必须被网关执行，而不只是被描述

`deltas.json` 里的每一条都必须落到两种机制之一，选哪种本身是对"这个差异有多宽"的
断言：

- **model delta** —— 差异是一条规则，构造出现在哪里就在哪里出现。它实现为
  normalizer 里的投影（如 `footnote-resolution-model` 把上游的脚注解析模型施加到
  本仓库的源忠实树上），语料再宽也不需要新增条目。
- **input delta** —— 上游某个函数里的点状行为，没有规则可投影（如
  `tasklist-checked-marker` 的子串搜索）。它按精确输入登记，且必须仍在语料中出现：
  一条不再被触及的登记和一条不再复现的登记是同一种腐烂。

两种都不是的条目判失败：网关不作用于它的 delta 是散文，不是规则。

因为要编译两个 parser，它们和 coverage 一样是自带编译的独立层，不属于禁止编译器的
test consumer 层。

### golden 为什么不能被 oracle 取代

自然的想法是：既然有了外部权威，就让 oracle 直接当 golden，删掉自产的 expected 块。
不行——两者回答的不是同一个问题。oracle 回答"我们和参考实现是否一致"，golden 回答
"我们的输出是否变了"。后者覆盖前者够不到的部分，而那部分不小：

- **oracle 比较的是投影，不是完整 canonical AST。** 上游 XML writer 不输出表格列
  对齐、fenced/indented 标志、fence 是否闭合、inline code 的 placement、脚注标签；
  这些字段在 oracle 那边根本没有对应物可比。它们只由 golden 钉住。
- **两个 oracle 都不比较 scope 位置。** 本轮修复改动了三处 golden 的位置，全部只有
  golden 抓到。
- **`cross_link` / `embed` 没有任何 oracle。** 上游侧只能验证"扩展关闭时不干扰"。
- **golden 在纯 CTest 图里跑，不依赖外部 toolchain。** oracle 需要编译 cmark-gfm 和
  安装 remark；把正确性判定挂到它们上面，等于每次跑测试都要拉起两套外部工具链。
- **已登记的有意差异，本来就只能由 golden 钉住**——那些地方参考实现按定义是错的。

所以分工是固定的：**oracle 判定它有权威的部分，golden 钉住其余的全部。** golden 的
自产性不是问题，只要它能被 oracle 判定的部分都真的送进了 oracle——这正是上面那条
语料范围规则存在的原因。

## 5.1 行为钉住覆盖率门禁

**这个门禁的唯一目的是：证明重构不改变 `source -> canonical AST` 的输出。**
它不是代码质量分，也绝不能被用来把当前行为登记为"可接受"。

因此产出者只运行**断言解析输出**的套件（`spec`、`extensions`、`regression`、
`conformance`、`equivalence`、`pathological`）。这条口径是全部含义所在：一个被
覆盖的分支，是"它的行为若改变会有 golden 断言失败"的分支；一个未覆盖的分支，
是重构可以静默改掉而无人发现的行为。跑过代码但不断言解析输出的套件
（`api`、`facade`、`consumer`、`fuzz`、`packaging`）不计入——它们提高数字却不
提供任何回归保护。

由此，`unpinned` 记录的是**未受保护的行为面**，不是"欠写的单元测试"。它靠**新增
语料输入**变小，不靠新增执行代码的测试：一个提高覆盖率却不断言解析输出的测试，
不偿还任何东西。

生成代码**不可豁免**。没有人评审生成代码，它的行为比手写代码更需要被钉住而不是
更不需要。`exempt` 唯一正当的理由是"任何 source 输入都无法到达该文件"。而任何
source 输入都到达不了的代码，是缺陷或死代码，需要决策，**不得**登记为 `unpinned`
——登记它等于承诺一个写不出来的测试。

全仓只有一份政策数据 `specs/coverage/policy.json` 和一个执行器
`scripts/check-coverage.mjs`；每个 execution platform 只负责产出 toolchain-native
报告，不得自带阈值、豁免或第二套"什么算已覆盖"的定义。

| 平台 | 产出者 | 报告格式 |
| --- | --- | --- |
| `swift-macos` | `scripts/coverage-swift-macos.sh`（SwiftPM `--enable-code-coverage`） | llvm-cov export |
| `kotlin-jvm` | `scripts/coverage-kotlin-jvm.sh`（JaCoCo，JVM target） | JaCoCo XML |
| `es-node` | `scripts/coverage-es-node.sh`（Node 内置 coverage） | LCOV |

目标是 lines/functions/branches 全部 100%。落地规则四条：

- 政策中**没有 `unpinned` 条目的 measured file 必须 100%**——这是新增代码从诞生
  起就必须满足的门禁；
- **`unpinned` 条目是该文件未钉住数的上界**，只能减少，不能增加；
- **`exempt` 条目把文件移出测量**，仅在"任何 source 输入都无法到达该文件"时正当；
- 平台**无法产出某项 metric 时必须在 `unsupportedMetrics` 中声明并写明理由**。

`unpinned` 用上界而非等值比较，是因为同一份源码在 required jobs 的不同操作系统上
编译结果不同，等值比较会把无关的平台差异变成覆盖率失败。条目变松时每次运行都
会报告，并由 `--update-ledger` 重写。

**归属必须按代码的真实来源文件计算。** `llvm-cov` 的 per-file summary 把一个函数
的全部分支记在该函数**起始**所在的文件上，因此被预处理器展开进函数体的代码（函数体内
`#include` 的生成表、header 里的 inline helper）会被记到宿主文件头上，而其自身报告
0/0 看起来完全覆盖。所以 llvm-cov 产出者一律导出全量报告而非 `-summary-only`，由
`scripts/lib/coverage.mjs` 从 function-level 记录按各分支自己的 `fileId` 重算；
执行器直接拒绝 `-summary-only` 报告——那不是"信息少一点"，而是朝着好看方向错的。

覆盖率门禁真正的失败模式不是数字低，而是数字高却什么都没测。三条反粉饰规则：

- `minimumMeasuredFiles` 记录报告应覆盖的文件数下界。文件整体从报告中消失
  （插桩器静默跳过无法读取的 class、filter 不再匹配）否则会被读成改进；
- 报告中无法映射到仓库路径的文件**直接判失败**，不得静默丢弃；
- 某项 required metric **完全没有计数**时判失败——插桩器悄悄停止产出分支数据，
  否则会通过全部逐文件规则却什么都没证明。

C 侧 `coverage` preset 在断言解析输出的 label 集合内再排除 `complexity`：那些 case
断言的是**用时**而不是输出，且其执行路径受调度影响，会让 required gate 变成 flaky。
它们在普通 preset 下仍是 required gate，被排除的是覆盖率归属，不是测试本身。

`unpinned` 不是永久豁免。其收敛排期由
`docs/migration/2026-08-01-incremental-canonical-ast-plan.md` 的里程碑拥有；
该文档同时记录 M7 的验收方式：同一份语料在新旧实现下的 canonical dump 必须逐字节
相同。

已知未覆盖到的一层：`swift-macos`、`kotlin-jvm`、`es-node` 三个 producer 目前仍运行
各自的完整套件，而不是按上面的口径只选断言解析输出的套件。三端 binding 主要是
对 C 结果的再暴露，`source -> AST` 的真值在 C 侧，但这三个数字因此**不是**行为钉住
覆盖率，读的时候不能与 `c-host` 同口径比较。对齐它们属于后续工作。

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
- Performance 测量固定 warmup/repeat。complexity runner 统一使用 process
  user+kernel CPU time，排除 hosted runner 将进程 deschedule 的时间：parse-scaling
  每个 endpoint warmup 1，随后长样本单次完整 parse、短样本 3 个至少 25 ms CPU
  的 sample 取中位数。benchmark runner 单独使用 monotonic wall-clock，warmup 1 +
  repeats 5 取中位数。complexity 的 parse-scaling cases 分别以
  scanner/map/reference 4 KiB → 128 MiB 与 delimiter-dense 4 KiB → 64 KiB
  endpoint 的每字节 CPU 成本断言渐近趋势；benchmark 使用 doubling 相对比率；
  均不使用易波动的绝对时间 gate。
- Scope-table complexity 使用 512 → 32768 的 adversarial deep-chain doubling
  序列：每个深度先 warmup，再取 3 个至少 25 ms CPU sample 的中位数；gate
  比较六个相邻区间 normalized growth 的中位数。这样持续的 ancestor-walk
  quadratic growth 会在多数尺度上失败，而单次 allocator/cache 层级切换不会被
  错当成复杂度类别。
- Footnote-renumber complexity 同样是 trend-based：256 → 4096 的 doubling
  序列，gate 比较四个相邻区间 normalized growth 的中位数。这条 case 的
  per-commit 成本按构造就是 footnote 数量的线性函数（delta 为每个被重编号的
  footnote 报告一个 changed node），所以被测的信号只是**对线性的偏离**，两个
  孤立 endpoint 之间的一次 allocator/cache 切换与该信号同量级。之前的两点比值
  形式正是这样失效的：同一份 C 代码在一个 commit 上通过、在只改了一个文本
  文件的下一个 commit 上以 4.099x 失败。中位数形式实测健康实现为
  0.984x–0.996x；把 footnote index diff 的 dedup set 换成线性扫描后为
  1.934x–1.952x，gate 正确失败。
- Benchmark 是诊断证据和回归 gate，不是根据当前样本设计另一套算法的 oracle。
  禁止为了追回某个局部数字，按 benchmark 观察到的 cardinality、input size 或
  “常见形状”增加实现分支（例如 `count == 1` 快路径）。同一个语义操作必须只有
  一套连贯的算法与数据模型；常数优化应改进这套共享算法或底层数据结构，而不是
  复制策略。只有可文档化的语义、ownership 或 lifecycle invariant 确实定义了
  不同操作时才允许独立路径，且必须分别有 correctness/complexity 覆盖。
- 复杂度 gate 必须验证一般不变量和能击穿旧实现的 adversarial shape。一次更快的
  benchmark 结果不能为违反上述单一算法约束的 special case 提供正当性。
- Delimiter engine 的 test-only deterministic counters 直接断言 arena growth、
  per-rule candidate visits、reduction、unlink 与 truncate work；process-CPU
  delimiter-dense scaling 只是其平台级补充证据。
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
