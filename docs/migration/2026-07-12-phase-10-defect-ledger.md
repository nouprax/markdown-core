# Phase 10 C Defect Ledger

本 ledger 汇总截至 Phase 10 开始时,由 migration reports、sanitizer、并发测试和
人工审计确认的全部 C engine/facade 缺陷。每项记录复现、根因、影响面、回归测试
和关闭证据。所有修复均落在 C 层;未引入任何 platform workaround。

状态汇总:**4 项缺陷,全部关闭**。无未关闭的 C correctness、memory-safety、
thread-safety 或 lifecycle 缺陷阻塞 Phase 11。

---

## MC10-01 并发首次 parse 的无同步 core-extension 注册

- **来源**:Phase 7 migration report §3.5(Swift Testing 并行执行时崩溃)。
- **复现**:全新进程中多个线程同时调用 `markdown_core_document_parse`。
  `markdown_core_core_extensions_ensure_registered` 的无同步
  `static int registered` 使多个线程同时执行整个注册事务。确定性复现:
  `concurrency_runner --case first_parse`(barrier 同时释放 8 线程)。
- **根因**:首次注册事务包含三类 process-global mutation——extension
  node-type 计数器(`MARKDOWN_CORE_NODE_LAST_BLOCK/_INLINE` 经
  `markdown_core_syntax_extension_add_node`)、node flag 分配
  (`markdown_core_register_node_flag`,重复注册直接 `abort()`)和 registry
  链表 append——全部无同步。
- **影响面**:任何多线程 consumer 的首次 parse:崩溃
  (`flag initialization error in markdown_core_register_node_flag` abort)、
  重复/撕裂的 extension 注册、错误 node type 分配。三端 binding 全部暴露。
- **修复**:新增可移植进程级 once(`core/once.{h,c}`:POSIX `pthread_once`,
  Windows `InitOnceExecuteOnce`),包住**整个**注册事务
  (`extensions/core-extensions.c`)。未采用局部锁,未要求 consumer warmup。
- **回归测试**:`facade_concurrent_first_parse`(facade label,无 warmup、
  全新进程、barrier 同步首次 parse,覆盖 parse/attach/traverse/dump/free 并
  与单线程参考 dump byte-for-byte 比较)。
- **关闭证据**:修复前该测试在 TSan build 下确定性失败
  (`Subprocess aborted`,stderr 出现 4 次 flag initialization abort);修复后
  default/ASan/UBSan/TSan 全部通过。Swift Testing 全局 warmup 已删除,
  `swift test`(并行执行,真实并发首次调用)10/10 通过。

## MC10-02 `markdown_core_release_plugins` 与初始化状态脱节

- **来源**:Phase 10 规范审计(spec §Phase 10 任务列表)。
- **复现**(代码路径审计):`markdown_core_release_plugins` 释放并清空
  registry,但注册侧的"已初始化"标志保持为真。此后同进程内的
  `markdown_core_document_parse` 在 `attach_extension` 找不到 extension,
  全部失败(`required syntax extension is unavailable`);若与并行 parse
  重叠,则是对 registry 链表的并发释放/读取(use-after-free)。
- **根因**:注册(once 语义)与释放(可重复调用)两个生命周期互不知情;
  存在"已初始化标志为真但 registry 已释放"的非法状态。
- **影响面**:CLI teardown 路径(`main.c` 曾在退出前调用);任何未来在
  library 场景调用该 API 的 consumer。
- **修复**:冻结 registry 生命周期——首次成功注册后 extension descriptors
  对 facade parse 保持 process-lifetime immutable。删除
  `markdown_core_release_plugins`(`registry.h`/`registry.c`)及 CLI 调用点;
  `registry.h` 写明注册是 initialization-time 操作、无 release/unregister
  路径。进程退出时由 OS 回收(全局指针保持可达,LeakSanitizer 不报泄漏)。
- **回归测试**:`regression_registry_lifecycle`(regression label,2000 次
  parse/free 循环、交错失败路径,最终 parse 仍附加全部 extensions 且 dump
  与首次 byte-for-byte 一致)。
- **关闭证据**:符号已从源码/头文件/调用点删除(全仓 grep 无余留);回归
  测试在 default/ASan/UBSan/TSan 下通过;ASan(leak 检测启用的平台)对 CLI
  与全部测试无泄漏报告。

## MC10-03 `SPECIAL_CHARS`/`SKIP_CHARS` 的每次 parse 进程级写入

- **来源**:本阶段规定的 facade parse 路径 process-global mutable state 审计
  (Phase 10 任务)中新确认。
- **复现**:`process_inlines`(每次 `markdown_core_parser_finish`)按当前
  parser 附加的 inline extensions 在进程级表 `SPECIAL_CHARS[256]`/
  `SKIP_CHARS[256]` 上先加后删。两个并发 parse 只要 extension 集合不同
  (例如一个启用 strikethrough、一个关闭),就互相污染:字符 `~`/`$`/`:`
  的 special/emphasis-skip 状态在另一个 parser 的 inline 扫描与 emphasis
  flanking 判定中间翻转,产生错误 AST;TSan 视角为纯粹 data race。确定性
  复现:`concurrency_runner --case stress`(混合 option 组合并发 parse)。
- **根因**:per-parse 的可变状态错误地放在进程级——初始化边界(MC10-01 的
  once)无法覆盖,因为它在每次 parse 中都要按 parser 的 option 集写入。
- **影响面**:初始化完成**之后**的所有并发 parse(比 MC10-01 更广):静默的
  AST 语义错误(emphasis 边界、extension inline 构造)、TSan data race。
- **修复**:表迁移为 parser-local(`parser.h` 新增 `special_chars[256]`/
  `skip_chars[256]`,`markdown_core_parser_reset` 以 immutable 基表初始化;
  `markdown_core_inlines_add/remove_special_character` 与
  `markdown_core_manage_extensions_special_characters` 全部改写 parser 表;
  `subject` 借用 parser 表指针,无 parser 的 reference 解析路径借用 const
  基表)。进程级不再存在任何 parse 期写入的表;`SMART_PUNCT_CHARS` 与基表
  声明为 `const`。
- **回归测试**:`facade_concurrent_stress`(facade label,8 线程 × 200 轮 ×
  6 输入 × 3 option 变体,含 `*a~b*c~`、`*a$b*c$` 等 skip-char 敏感输入,
  全部 dump 与参考 byte-for-byte 比较)。
- **关闭证据**:以进程级表模拟修复前行为时,该测试在 TSan 下报多起
  `WARNING: ThreadSanitizer: data race` 且出现功能性 dump 分歧
  (`thread 0 reported a violation`);恢复 parser-local 实现后
  default/ASan/UBSan/TSan 全部通过,单线程 goldens(spec/extensions/
  regression 全部 56 项 correctness 测试)无任何行为漂移。

## MC10-04 UBSan 插桩缺口(验证基础设施)

- **来源**:本阶段 sanitizer 配置审计。
- **复现**:`Ubsan` build type 的 `-fsanitize=undefined` 只在
  `core/CMakeLists.txt` 目录作用域追加,extensions 与 tests 目录未插桩;
  `correctness-ubsan` 因此从未真正检查过 extensions/ast.c、六个 extension
  实现和全部 test runners 的 UB。
- **根因**:sanitizer flag 放错目录作用域。
- **影响面**:UBSan 验证结论对超过一半的 C 代码无效。
- **修复**:sanitizer build type 的编译 flags 上移到
  `packages/markdown-core/CMakeLists.txt`(core、extensions、tests 全部
  插桩);同层新增 `Tsan` build type 与 `tsan`/`correctness-tsan` presets、
  `make tsan-test`、CI `ubsan`/`tsan` jobs(此前 CI 亦无 ubsan job)。
- **回归测试**:`ctest --preset correctness-ubsan` / `correctness-tsan`
  本身(现在覆盖全部 56 项 correctness 测试与全部 C 源)。
- **关闭证据**:全插桩后 UBSan/TSan 各 56/56 通过。

---

## 审计范围备注(非缺陷)

facade parse 路径其余 process-global state 审计结论:

| 状态 | 结论 |
| --- | --- |
| `MARKDOWN_CORE_NODE_LAST_BLOCK/_INLINE`、node flag 计数器、registry 链表 | 只在 once 事务内写入,之后 process-lifetime immutable;once 建立 happens-before |
| `MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR`、`syntax_extension.c:_mem` | 静态初始化后从不写入 |
| `inlines.c` 基表、`SMART_PUNCT_CHARS`、scanners/entities/houdini 表 | `const`/只读 |
| `arena.c` 静态 arena | 仅 CLI(`main.c`)使用的诊断 allocator,不在 facade parse 路径;facade 固定使用 default allocator |
| `node.c:enable_safety_checks` | legacy engine API 的启动期开关,不属于 facade 公开面;必须在并发开始前设置(已在头文件契约中排除未公开约定) |

facade 失败路径复审:parse 的四条失败路径(invalid argument、parser 分配
失败、document 分配失败、root 缺失)均正确释放已获取资源且不触碰全局状态;
`set_error` 在 error 结构自身分配失败时保持 `*error == NULL`(调用方按
document == NULL 判定失败,不解引用 error),无泄漏、无双重释放。dump 的
determinism 由每个并发/生命周期用例的双 dump byte 比较持续验证。
