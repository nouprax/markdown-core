# Phase 10 Migration Report:C 缺陷清零与并发契约冻结

本报告记录 Phase 10 的线程模型、portable once 设计、registry lifecycle 决策、
修复内容、测试复现方式、sanitizer/TSan 结果、platform/package 审计和 workaround
删除情况。缺陷明细(复现、根因、影响面、回归测试、关闭证据)见
`2026-07-12-phase-10-defect-ledger.md`。

## 1. 冻结的线程模型

公开契约完整写入 `packages/markdown-core/include/markdown_core.h` 头部注释:

1. **初始化**:library 在 `markdown_core_document_parse` 内部经进程级 once
   自初始化;并发首次调用安全,无需 warmup、外部锁或显式 init。
2. **registry 生命周期**:首次成功注册后 extension registry 对整个进程
   生命周期 immutable;不存在 teardown 或重初始化路径。
3. **不同 document**:parse/traverse/dump/free 可完全并发;parse 调用之间
   不共享任何可变状态。
4. **同一 document**:parse 返回后 document 及其 nodes 经此 API 逻辑上
   immutable,多线程只读访问(traverse、accessors、dump)安全;
   `markdown_core_document_free` 是唯一 mutation,由调用方保证与所有其他
   访问互斥且此后不再访问。node handle 与 string view 借用自所属 document,
   随其一同结束。
5. **error/dump 所有权**:out-parameter 返回的 error 与 dump buffer 归调用方
   所有,分别经 `markdown_core_error_free`/`markdown_core_dump_free` 释放
   (均接受 NULL)。
6. 契约声明自身完备:bindings 不得依赖未公开约定。

## 2. Portable once 设计

`core/once.{h,c}`(internal header,不进入公开 API):

- POSIX(macOS、Linux、Android、Emscripten):`pthread_once_t` +
  `pthread_once`。
- Windows(MSVC、MinGW-w64):`INIT_ONCE` + `InitOnceExecuteOnce`;函数指针
  经 union 穿越 `PVOID` 参数,规避 C99 函数指针/对象指针转换限制。
- 两个原语均保证回调恰好执行一次,且所有经过 once 的线程观察到回调的全部
  写入(happens-before),因此 once 事务内的 node-type 计数器、node flag
  和 registry 写入对所有 parse 线程可见。
- 满足项目 C99 baseline;CMake 侧经 `find_package(Threads)` 以
  `CMAKE_THREAD_LIBS_INIT`(纯 flag,不向 install(EXPORT) 引入 imported
  target 依赖)链接。

`markdown_core_core_extensions_ensure_registered` 用该 once 包住**整个**
core-extension 注册事务;没有对 `markdown_core_register_node_flag` 的局部
加锁,也没有任何 consumer warmup 要求。

## 3. Registry lifecycle 决策

方案:**process-lifetime 冻结,删除释放路径**。

- `markdown_core_release_plugins` 从 `registry.h`/`registry.c` 删除,CLI
  (`main.c`)不再调用。理由:该 API 与 once 状态本质脱节(见 ledger
  MC10-02),而 registry 是每 process 一次、几 KB 级的固定描述符集合,
  由 OS 在进程退出时回收是唯一无竞争的生命周期。
- `registry.h` 写明:注册是 initialization-time 操作,必须先于并发 parse;
  注册后的 extensions immutable 且 process-lifetime;不存在
  release/unregister,因此"已初始化标志为真但 registry 已释放"的状态在
  类型系统层面不可再现。
- 全局 registry 指针保持可达,LeakSanitizer 不将其计为泄漏(ASan 全套
  通过)。

## 4. 修复与代码变更

| 变更 | 文件 |
| --- | --- |
| 新增 portable once | `core/once.h`、`core/once.c`(加入 core CMake、Package.swift、Android JNI CMake 三处源列表) |
| once 包住注册事务 | `extensions/core-extensions.c` |
| 删除 release 路径、冻结 registry | `core/registry.{h,c}`、`core/main.c` |
| special/skip 字符表 parser-local 化(ledger MC10-03) | `core/parser.h`、`core/inlines.{h,c}`、`core/blocks.c` |
| 公开线程契约 | `include/markdown_core.h` |
| 并发/生命周期回归 | `packages/markdown-core/tests/runners/concurrency_runner.c`、`packages/markdown-core/tests/CMakeLists.txt` |
| sanitizer 全包插桩 + Tsan build type | `packages/markdown-core/CMakeLists.txt`、`core/CMakeLists.txt` |
| tsan presets/targets/CI、补 ubsan CI job | `CMakePresets.json`、`Makefile`、`.github/workflows/ci.yml` |
| 删除 Swift warmup workaround | `packages/swift-markdown-core/Tests/MarkdownCoreTests/MarkdownCoreSuites.swift` |
| 冻结契约文档同步 | `docs/specs/test-architecture.md` |

单线程解析行为零变更:全部既有 goldens(spec/extensions/regression/
pathological/fuzz)在修复前后 byte-for-byte 一致。

## 5. 测试与复现方式

新增 3 项 CTest(沿用冻结 label taxonomy,无新 label):

| 测试 | Label | 内容 |
| --- | --- | --- |
| `facade_concurrent_first_parse` | `facade` | 全新进程,barrier 同时释放 8 线程进入各自**首次** parse(无 warmup),覆盖 parse、extension attach、traverse、dump、free;全部 dump 与线程 join 后计算的单线程参考 byte-for-byte 一致 |
| `facade_concurrent_stress` | `facade` | 初始化完成后 8 线程 × 200 轮 × 6 输入 × 3 ParseOptions 变体(default/minimal/split)并发,专门混合互相矛盾的 extension 集合以钉住 parser-local 字符表;线程内重复解析亦须自一致 |
| `regression_registry_lifecycle` | `regression` | 2000 次 parse/free 循环交错失败路径(NULL source),末次 parse 仍附加全部 extensions 且 dump 与首次一致 |

Runner 为纯原生 C(POSIX pthread / Win32 threads + 自实现 barrier),无
脚本语言、无网络、无 warmup;TSan 不可用的平台经 default preset 运行同一
批测试,不静默跳过。

**缺陷敏感性验证**(修复回退演示,均已恢复):

- 将 once 临时回退为旧 `static int registered`:
  `facade_concurrent_first_parse` 在 TSan build 下确定性失败——
  `flag initialization error in markdown_core_register_node_flag` abort。
- 将字符表临时回退为进程级:`facade_concurrent_stress` 报多起 TSan
  `data race` 且线程 dump 出现功能性分歧。

## 6. 验证矩阵(本机 macOS arm64,Xcode clang)

| 验证 | 结果 |
| --- | --- |
| `ctest --preset correctness`(Release,shared) | 56/56 通过 |
| `ctest --preset correctness-asan`(static) | 56/56 通过 |
| `ctest --preset correctness-ubsan`(static,全包插桩) | 56/56 通过 |
| `ctest --preset correctness-tsan`(static,新增) | 56/56 通过 |
| `swift test`(并行,无 warmup,真实并发首次调用) | 4 suites / 10 tests 通过 |
| C/C++ consumer(`consumer_facade_cplusplus`) | 通过(上列各 preset 内) |
| packaging guard(`packaging_corpus_guard`) | 通过 |
| `scripts/audit-test-topology.sh` | 全部检查通过 |
| `pnpm format:c:check` / `format:cmake:check` / `lint:c` / `check:contracts` | 通过 |

CI 侧新增 `ubsan`、`tsan` jobs(ubuntu-latest/clang),与既有
default(shared/static × clang/gcc × ubuntu/macos/windows)、asan、Swift
jobs 共同构成矩阵;Windows 无 TSan,由 default 矩阵运行同一并发回归。

## 7. Platform/package 审计

- 编译 C engine 的全部四处构建已同步 `once.c`:core CMake、SwiftPM
  (`Package.swift`)、Android JNI(`android/src/main/cpp/CMakeLists.txt`)、
  (nmake/appveyor 委托 CMake,无独立源列表)。
- 公开导出面未扩大:`once.h`/`registry.h` 仍为 internal headers;
  `exports/markdown_core.map` 无变更(`markdown_core_release_plugins` 本就
  不在导出集内);安装的公开 header 仍只有 `include/markdown_core.h`。
- `markdown_core_parse_inlines` 等 internal API 签名变更均在同一 commit 内
  完成全部调用点迁移,无 compatibility shim、无 deprecated alias。

## 8. Workaround 删除情况

- Swift Testing 的全局 facade warmup(`facadeWarmedUp` global `let` 及每次
  parse 前的 `#expect`)已删除;Swift 并行测试现在直接覆盖真实首次调用。
- C 侧不存在其他测试侧串行化或 binding 层锁;`concurrency_runner` 的三个
  用例均为无预热原生并发。
- Swift、Kotlin、ES production binding 中无任何被迁移的 workaround
  (Kotlin/ES binding 尚未创建;Swift 侧仅存在测试 target,已清理)。

## 9. Acceptance

- [x] 并发首次与后续 facade parse 无需预热或外部锁 ✅(first_parse/stress 测试)
- [x] registry 初始化/释放无竞争、无状态脱节 ✅(release 路径删除 + lifecycle 回归)
- [x] 原生并发 regression、TSan(支持平台)、ASan、UBSan、Release、shared/static、consumer、package 验证全部通过 ✅(§6)
- [x] defect ledger 中截至阶段开始已确认的 C 缺陷全部关闭 ✅(ledger 4/4)
- [x] Swift 测试 warmup 已删除 ✅(§8)
- [x] platform bindings 可只依赖公开 C 契约 ✅(§1 契约写入公开 header)
