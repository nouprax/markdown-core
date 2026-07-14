# Phase 7:冻结全仓统一测试架构并收敛 CTest suites

状态:已完成。历史 CTest 迁移与 test routing ownership 修订均已实现；当前冻结契约
见 `docs/specs/test-architecture.md`，本文记录迁移 inventory、关键决策与验证结果。

> Phase 18 将唯一 canonical AST conformance corpus 提升到
> `specs/canonical-ast/`。它是 runner-free product contract data，不是 root
> `tests/` 或共享 harness；普通 correctness fixtures 的 package ownership、原生
> runner discovery 和 correctness/conformance/benchmark 互斥结论保持不变。

> Phase 17 audit 证明历史 flat `test:<lang>` 入口缺少真实平台覆盖；第一轮修复又
> 过度扩展成 language → platform → suite task matrix。最终修订将根级 pnpm 的责任
> 收窄为 task family → execution platform，suite discovery/filter 继续归原生 runner。
> 因此 CTest 迁移结论仍有效，路由实施与 Phase 7 均已关闭。required CI 的实际
> 平台运行证据归 Phase 19 CI 验收，不反向阻塞测试架构阶段。

## 1. Pro Git benchmark 删除

- 删除 Makefile 的 `progit`、`bench`(旧实现)、`newbench`、`directive-bench`、
  `leakcheck`、`fuzztest`、`operf` targets,以及 `BENCHFILE`/`benchinput.md`
  与 `$(ALLTESTS)` 的 generation/dependency/cleanup 规则。
- 删除 `benchmarks.md`(Pro Git 运行说明与历史对比表)、
  `packages/markdown-core/benchmarks/stats.py`、`statistics.py`。
- 删除 `.gitignore` 中的 `progit/`、`benchinput.md`、`alltests.md`、
  `afl_results/` 条目;AFL campaign 输出改写入 build 目录。
- `packaging_corpus_guard`/`benchmark_corpus_guard`(CTest)与
  `scripts/audit-test-topology.sh` 持续断言这些路径不存在且不被 gitignore 隐
  藏。大输入 benchmark 覆盖由 `benchmark_large_document`(tracked samples 块
  重复至历史 11MB 语料同量级)替代,完全离线、确定性、可再分发。

## 2. C test inventory:旧 → 新映射

旧 graph(24 项,Python/ctypes/CLI 驱动)全部迁移为原生 CTest suites(57 项
correctness + 6 项 benchmark),无 Python、无网络、无降级 skip:

| 旧测试(runner) | 新测试(label) | 说明 |
| --- | --- | --- |
| `api_test`(原生) | `api_engine`(api) | 原样保留,重命名注册 |
| `facade_test`(原生) | `facade_native`(facade) | 原样保留 |
| `facade_cplusplus_test`(原生) | `consumer_facade_cplusplus`(consumer) | 原样保留 |
| `canonical_ast_cli`(ast_dump_tests.py) | `facade_dump_cli`(facade) | `dump_cli_runner` 驱动 CLI `-t ast`,fixture 清单由 glob 单源生成 |
| `html_normalization`(doctest normalize.py) | 删除 | normalizer 已删;roundtrip 采用文档化的 `<pre>` 外空白折叠 |
| `spectest_library`/`spectest_executable`(spec_tests.py) | `spec_commonmark`(spec) | `spec_runner` 进程内库调用,byte-exact |
| `smartpuncttest_executable` | `spec_smart_punctuation`(spec) | |
| `entity_library`(entity_tests.py) | `spec_entities`(spec) | `entity_runner` 直接 `#include "entities.inc"` |
| `roundtriptest_library`(roundtrip_tests.py) | `spec_roundtrip_commonmark`(spec) | commonmark→HTML roundtrip,空白折叠比较 |
| `extensions_executable` | `extensions_gfm`(extensions) | |
| `formula_github_compatibility_executable` | `extensions_formula_github`(extensions) | |
| `formula_option_gates_executable`(`-t xml`) | `extensions_formula_option_gates`(extensions) | spec_runner `--mode xml` |
| `formula_latex_compatibility_executable` | `extensions_formula_latex`(extensions) | |
| `formula_conflicts_executable` | `extensions_formula_conflicts`(extensions) | |
| `directive_extension_executable` | `extensions_directive`(extensions) | 原 normalize 比较实为 byte-exact,现固定 byte-exact |
| `directive_option_gates_executable`(`-t xml`) | `extensions_directive_option_gates`(extensions) | |
| `roundtrip_extensions_executable` | `extensions_roundtrip_gfm`(extensions) | |
| `option_table_prefer_style_attributes` | `extensions_option_table_style`(extensions) | |
| `option_full_info_string` | `extensions_option_full_info_string`(extensions) | fixture 内含 NUL 字节;runner 按 byte length 传参 |
| `regressiontest_executable` | `regression_commonmark`(regression) | |
| `pathological_tests_library`(21 cases,单进程) | `pathological_<case>` ×21(pathological) | 逐 case 注册,CTest TIMEOUT 30s;正则断言移植为 repeated-segment 匹配 |
| `directive_pathological_executable`(5 cases + 6 original scaling cases) | `pathological_directive_*` ×5、`pathological_complexity_*` ×8 | complexity 扩展为 6 个 directive scanner/attribute case 加 2 个 reference-map case，使用 4 KiB → 128 MiB endpoint 每字节成本与 2.0× 拒绝线，`RUN_SERIAL` |
| `inline_delimiter_stack_tests_executable`(4 checks) | `pathological_formula_*` ×4 | |
| (无;fuzztest 用 /dev/urandom) | `fuzz_smoke`(fuzz) | 确定性:固定种子 xorshift 生成 + tracked corpora;parse/traverse/dump×2/free,断言 dump 确定性 |
| (无) | `packaging_corpus_guard`(packaging)、`benchmark_corpus_guard`(benchmark) | corpus/workspace 政策 guard |
| `make bench`/`newbench`/`directive-bench`(Make+Python stats) | `benchmark_*` ×5(benchmark) | `bench_runner`:representative/large_document/deep_nesting/extensions/adversarial,warmup 1 + repeats 5 中位数,仅 doubling 相对比率断言 |

共享原生 test support:`packages/markdown-core/tests/support/`(fixture 加载、
spec 解析、进程内转换、UTF-8 byte 比较、确定性 diff、segment 匹配、PRNG、单调
时钟),所有 runner 复用,无 subprocess/ctypes glue 复制。

## 3. 关键决策

1. **Roundtrip 比较的空白折叠。** 旧 Python normalizer(HTML 解析级规范化)只
   在 roundtrip 类 suites 中吸收 commonmark writer 的纯空白差异(setext 标题
   softbreak 展平、code span padding;spec.txt 中共 5 例)。原生实现改为文档
   化的确定性 canonicalization:比较前对两侧折叠 `<pre>` 外的空白 run。所有
   非 roundtrip suites 一律 byte-exact(旧 graph 中 directive suite 名义上走
   normalize,实测本就 byte-exact,已固定为 byte-exact)。
2. **`-e footnotes` 语义。** 与 CLI 相同,映射为 `OPT_FOOTNOTES` 选项位而非
   extension attach;`OPT_DIRECTIVE` 隐含 attach directive extension(复刻
   CLI `attach_option_extensions`)。
3. **Pathological 正则断言移植。** 旧断言均为「重复字面量」形状的锚定正则,
   移植为 `ts_match_segments`(连续重复字面量段匹配);`backticks` 用字符类
   扫描;directive 精确期望用全量字符串比较。语义均不弱于原断言。
4. **Swift 真实 suite。** `pnpm test:swift` 从 `swift build` 迁移为
   `swift test`;tools-version 升至 6.0,新增 Swift Testing target
   `MarkdownCoreTests`(`packages/swift-markdown-core/Tests/`),suites `api`/`errors`/`unicode`/
   `ownership` 共 10 tests,经 SwiftPM module 直接测试 C facade。`swift build`
   保留为独立 `pnpm build:swift`。
5. **发现一处 facade 缺陷(由 Phase 10 修复)。** 并发首次
   `markdown_core_document_parse` 会在 `markdown_core_register_node_flag` 中
   竞争。根因是整个 core-extension 首次注册事务非线程安全:extension node
   type/flag 分配和 registry mutation 都是 process-global mutable state,Swift
   Testing 并行执行时会暴露为崩溃。测试侧暂以线程安全的一次性 warmup 规避;
   新增的 C 缺陷清零 Phase 10 必须修复 registry 初始化与生命周期、添加无 warmup
   的原生并发/TSan regression,并在 Phase 11 Swift binding 开始前删除该 workaround。

## 4. 验证

- `ctest --preset correctness`:57/57 通过(labels:api 1、facade 2、consumer 1、
  spec 4、extensions 10、regression 1、pathological 36、fuzz 1、packaging 1)。
- `ctest --preset benchmark`:6/6 通过(guard + 5 workloads,串行)。
- `swift test`:4 suites / 10 tests 通过。
- `scripts/audit-test-topology.sh`:全部检查通过(无 .py runner、无 Python
  CMake 依赖、无 corpus 痕迹、无运行时网络、pnpm 路由逐字符合契约、全部
  label 非空、无 disabled、correctness/benchmark 互斥、CMake 注册与 runner
  `--list` 一致、Swift suites 非空)。
- `make test`/`make bench` 分别委托 correctness/benchmark presets;asan/ubsan
  经 `asan`/`ubsan` presets 复用同一 graph;CI(`ci.yml`)全部改用 presets,
  benchmark 由独立 `benchmark.yml`(weekly schedule + 手动)调度。

## 5. Routing contract revision

Phase 7 写回的修订 task 已以如下边界替换 flat routing 和随后出现的 suite task
matrix：

- 根级只提供 `test:<platform>`、`conformance:<platform>`、
  `benchmark:<platform>` 三个并列 task families；平台名称已包含语言。禁止 language
  aggregation、suite 后缀、`:full` aliases、root suite matrix 和通用 family router。
- 平台叶子覆盖 C host/compiler matrix、Swift macOS/iOS Simulator、Kotlin
  JVM/Android host/Android emulator/macOS ARM64/Linux x64，以及 ES Node/browser。
- suite/case 的列举与过滤只由 CTest、Swift Testing/`xcodebuild`、Gradle/
  instrumentation 和 ES package runner 提供；pnpm 与 CI 不复制 suite 清单。
- `test` 只运行传统 correctness；`conformance` 是并列的 contract/spec/schema
  target family，不由 `test` 隐式包含；`benchmark` 只拥有计时、吞吐量、scaling
  和性能基线。
- 不建立公开 `stress` task。Large/deep/repeated input 在 correctness 下注册为
  `robustness` cases，在 benchmark 下注册为独立 timed workloads；两者目的与验收
  证据分离。

Topology audit 必须拒绝 suite 级 pnpm tasks，并核对平台入口、具名原生 target 与 CI
destination。实现已删除第一轮通用 router，且 required CI workflow 已映射全部声明
平台，因此两项 routing implementation tasks 与 Phase 7 均已关闭。每个平台的实际
远端执行、缺失 destination 时失败且不静默跳过，以及 Linux x64/repo-managed Android
emulator 绿色证据，统一归 Phase 19 CI 验收。Linux x64 不建立本机
container/emulation 替代入口。

本地修订证据：C correctness 57/57、conformance 2/2，并单独通过
large-document/deep-nesting benchmarks；Swift macOS 与 iOS Simulator 分别通过
5 项 `MarkdownCoreTests` correctness 和 2 项 `MarkdownCoreConformanceTests`；
Kotlin JVM、Android host、macOS ARM64 的配对 correctness/conformance tasks 通过，
JUnit reports 证明 `AstTest` 只出现在 conformance target；ES Node correctness 9 项、
conformance 2 项、browser correctness 与两项 benchmark workloads 通过；topology
audit 通过。Gradle DSL 固定声明 Pixel 10 Pro XL/64-bit Google APIs Managed Devices
group；`markdownCoreAndroidPageSizesGroupAndroidDeviceTest` 分别 provision API 36/
4 KB 与 API 37/16 KB system images；每个 emulator 分别通过 10 项 correctness 和
2 项 conformance，因此 group 总计执行 20 项 correctness 和 4 项 conformance，
第二次执行复用 configuration cache。该 target 不读取 Android Studio 已有 AVD 或
serial，根入口与 required CI 均直接调用它。Linux x64 不在本机模拟，由 Phase 19
在 required CI 的 `ubuntu-latest` platform job 验收。GMD 自动停止运行实例并恢复
干净 snapshot；`clean:kotlin-android-emulator` 仅作为按需清理 managed AVD cache
的显式 maintenance task，不挂到 correctness/conformance lifecycle。

## 6. Closure re-audit

本轮按「架构实施」与「远端 CI 执行」分别审计：

- 已关闭：execution-platform routing contract 修订；correctness、conformance、
  benchmark 原生 targets 互斥实现；stress-shaped inputs 的 correctness robustness /
  benchmark workload 拆分；CI platform/destination mappings；Phase 7 整体。
- 转交 Phase 19：required CI 对 Kotlin Linux x64 与 repo-managed Android emulator
  的 correctness/conformance 绿色执行证据，以及缺失 execution destination 时的
  fail-not-skip 行为。本机 Android GMD 通过仍作为实现证据保留。
