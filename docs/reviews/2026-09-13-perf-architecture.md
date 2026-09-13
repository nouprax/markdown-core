# Performance 与 architecture review — 2026-09-13

审查基线：`3e6ad875`，macOS arm64。审查覆盖 C parser、元素识别/提交、AST 所有权、后处理、source mapping，以及 Swift、Kotlin JVM/Native、ECMAScript/WASM 的投影与生命周期。下文 1–8 节记录修复前发现；文末记录本轮修复及验证。

**初审结论：不通过。确认 2 项 P1、6 项 P2；另有 2 项设计观察。** 现有测试全部通过，新增的组合压力测试仍发现确定的二次复杂度和 Swift 释放时栈溢出。不能据此声明“没有 perf 问题”或“所有生命周期都安全”。8 项问题均已开 issue，修复状态见文末。

用户确认的 scope 契约优先于现存文档：输入假定为 UTF-8，使用与 cmark 对齐的 editor 起止坐标；不做输入 validation、坐标单位转换或范围修复。scope 不是对解析后 string 的切片范围，也不能据此推断任意节点的 literal。不得改成 UTF-16、grapheme 或统一 half-open range。

## 1. [P1] 通用节点挂接的祖先检查进入 parser 热路径，产生 Θ(n²) 工作

位置：[node.c:88](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/core/node.c#L88)、[autolink.c:710](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L710)。

`S_can_contain` 为防止循环，对每次挂接遍历父节点的所有祖先。这个检查对任意树重挂接有必要，但 parser 将刚创建、尚未发布的 Link 和 Text 插入已有深层树时，也走同一条路径。嵌套深度 D 与叶层新节点数量 W 相乘，形成 Θ(D×W)。已有单独测深度或宽度的 benchmark 没有验证这一组合。

复现输入：`"![" * n + "a@b.co " * n + "](u)" * n`。相同结构、将邮件替换为等长 `abcdef `，作为控制组。

| n | 输入字节 | 祖先访问次数 | Release parse + free，中位数 |
| ---: | ---: | ---: | ---: |
| 512 | 6,656 | 527,872 | 1.862 ms |
| 1,024 | 13,312 | 2,104,320 | 6.812 ms |
| 2,048 | 26,624 | 8,402,944 | 25.942 ms |
| 4,096 | 53,248 | 33,583,104 | 217.969 ms |
| 8,192 | 106,496 | 134,275,072 | 982.437 ms |

计数严格符合 `2n² + 7n`；控制组为 `2n`，n=8,192 时耗时 2.324 ms。Span 嵌套也复现。计数来自只增加循环计数器的独立 `node.c` 副本，生产源码未改动；计时来自未插桩的 Release 库，每项取 3 次中位数。复杂度结论依赖确定的操作次数，而非计时比率。

修正方向：让“新建、未发布节点的提交”具有明确所有权不变量，使其能够常数时间挂接；对任意已存在节点的重挂接继续检查循环。审计所有 `append/insert/replace` 调用者，包括 link、citation、table、formula、footnote。不能重新引入全局 safety 开关、按深度绕过检查，或只为 autolink 增加特殊算法。回归门槛应覆盖独立变化的 D、W，以及二者一起增长。

## 2. [P1] Swift 值树在正常 ARC 释放时递归栈溢出

位置：[List.swift:45](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/swift-markdown-core/Sources/MarkdownCore/Markup/List.swift#L45)、[Document.swift:38](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/swift-markdown-core/Sources/MarkdownCore/Document.swift#L38)、[MarkdownCoreSuites.swift:312](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/swift-markdown-core/Tests/MarkdownCoreTests/MarkdownCoreSuites.swift#L312)。

`NativeTreeBuilder` 和 visitor 使用迭代遍历，但最终值树通过数组和 existential 持有下一层值；最后一个持有者释放根时，ARC 仍递归释放所有后代。现有深层树测试明确逐层保留下一个节点、释放上一个节点，以绕过这种销毁方式，因此测试没有覆盖普通调用者的生命周期。

独立消费程序链接当前源码的优化版 Swift/C 库，解析 `"- " * depth + "leaf\n"`，用 `withExtendedLifetime` 保证打印和 flush 发生在释放前：

| 深度 | 完成解析 | 正常释放 | 对照：解析后直接 `_exit(0)` |
| ---: | :---: | :---: | :---: |
| 1,000 | 是 | 成功 | 成功 |
| 10,000 | 是 | 成功 | 成功 |
| 30,000 | 是 | SIGSEGV | 成功 |
| 65,536 | 是 | SIGSEGV | 成功 |

主机 LLDB 在释放阶段捕获 `EXC_BAD_ACCESS`；栈中重复出现 `_swift_release_dealloc`、`swift_arrayDestroy`、`_ContiguousArrayStorage.__deallocating_deinit` 和生成的对象销毁函数。故障在数组所有权链的销毁阶段，不是 C parse 或 Swift 构建阶段。具体触发深度受线程栈与编译条件影响，30,000 不是可依赖的安全上限。

修正方向：内部所有权需要保证构建、遍历、完整根释放、保留子树后的最后释放都具有有界调用栈，并保持既有不可变值语义和跨 isolation 传递能力。修复必须有真实消费者的正常释放测试。手动逐层释放的调用约定、增大线程栈、深度阈值以及额外全局清理队列都不能作为架构结论。

## 3. [P2] Directive 探测在完整识别前分配属性，失败后撤销；成功探测也丢弃再解析

位置：[directive.c:257](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/directive.c#L257)、[directive.c:537](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/directive.c#L537)、[directive.c:590](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/directive.c#L590)。

`scan_directive_block` 先调用属性解析，构造 classes、records、字符串，再检查尾部是否只有空白。`probe_directive_block` 无论成功失败都会释放结果；真正的 open 再走同样的解析。

独立 allocator 计数：失败的 `::: {.a k=1} junk\n` 单次 probe 为 **7 次分配、550 bytes**；有效的 `::: {.a k=1}\n` 为 **7 次分配、510 bytes**，但 probe 随后丢弃全部属性。class word 形式的无效尾部也会先分配再撤销。

8,192 个无效 directive 段落累计 131,185 次分配、17,045,224 bytes；只把开头换成 `;;;` 的等长控制组为 41,071 次、7,100,136 bytes。多出 90,114 次分配。属性工作发生在局部 attribute parser 中，probe 的 `parser.attribute_work` 仍为 0，现有工作计数不能反映这条路径。

修正方向：识别阶段保留借用的名字、label 和属性起止信息，完整验证候选后才物化一次语义字段；共享属性 recognizer，避免另一套 grammar。所有计入复杂度的识别工作必须汇总到同一度量。这里的问题是成功提交前的语义分配，不是必须等待整个 directive block 的结束 fence 才允许创建合法 block。

## 4. [P2] Pipe table continuation 构造并销毁行数据，随后重复构造

位置：[table.c:607](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/table.c#L607)、[table.c:486](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/table.c#L486)。

`matches` 调用 `row_from_string` 分配行及单元格，仅为返回布尔值，然后全部释放。`try_opening_table_row` 再解析同一行；它还先挂接 AST TableRow，再在后续失败时删除。对 `| a | b |\n`，仅 continuation 判断就发生 **3 次分配、144 bytes**，这些临时结果全部被丢弃。

另外，header 识别将整个 paragraph 交给 `row_from_string`，每遇到前置行都构造并清空该行 cells，最终只保留选中的 header。这里确认的是多余线性工作与分配，不据此声称二次复杂度。

修正方向：行识别提供借用的边界事实，在选定 header/body 并提交时构造实际 cells。不要把临时解析产物做成需要与 AST 同步的长期缓存。有效 continuation 的前置检查通常已确认语法，不能把所有 TableRow 创建都误报成“未闭合候选对象”。

## 5. [P2] Autolink 无命中时也复制整段文本，且创建后删除空 Text

位置：[autolink.c:563](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L563)、[autolink.c:712](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L712)、[autolink.c:747](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L747)。

`postprocess_text` 在确认任何邮件命中前就移出 owned literal，改成借用 slice；即使不存在 `@`，函数末尾也重新分配复制 literal，再释放原 buffer。90,112 bytes 普通文本在 autolink 阶段累计申请 90,208 bytes，其中 96 bytes 是迭代/合并辅助开销，剩余为整段复制。无效邮件候选同样如此。

每次切分又无条件创建尾部 Text；如果 `post_len == 0`，它马上被删除。空前缀也先进行所有权转换再删除。外层 parser 已经完成 Text consolidation，而第一个 postprocessor autolink 又执行一遍；中间没有修改树的处理阶段。

修正方向：借用扫描直到首个确定命中，届时才转移原 buffer 的所有权；只提交非空 fragments，统一文本合并所处的生命周期。保持 source mapping 和 OOM 的完整失败边界，不添加根据长度或命中数量选择算法的分支。

## 6. [P2] 固定方言的注册信息每次 parse 都被重建为多条堆链表

位置：[blocks.c:95](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/core/blocks.c#L95)、[core-elements.c:41](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/core-elements.c#L41)。

每次 parse 都附加相同的 32 个静态 descriptor，重新构造 `elements`、`inline_elements`、`inline_lifecycle_elements`、`block_elements` 和 `block_alternatives`。追加还重复遍历链表尾部，inline registration 重新排序。这里复杂度是对元素数 E 的 O(E²)，E 当前固定；不是对输入长度的二次复杂度。

空输入总计 90 次分配、10,048 bytes；读取输入之前已经 76 次。76 包括 parser/root 等初始化，不能全部归因于注册链表。但对于每次编辑都 parse 的调用方式，静态 membership/order 反复分配与释放是无必要的固定成本和失败边界。

修正方向：以一个不可变、连续的注册布局表达固定顺序与分类；需要保留的测试注入也使用相同注册模型。不要用进程级 lazy cache、锁或一套生产专用与一套测试专用机制来替代这些链表。

## 7. [P2] Autolink 根据域名分段数量改变语义，以隐藏旧的复杂度问题

位置：[autolink.c:245](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L245)。

代码明确在域名最后两段包含 `_` 时，只有 `np <= 10` 才拒绝；更多段则接受。注释解释这是为避免 quadratic behavior，并假定正常 URL 不会有这么多段。

已复现：`www.a.a.a.a.a.a.a.a.a._b` 为 Text；增加一段得到 `www.a.a.a.a.a.a.a.a.a.a._b` 后变成 Link。这是按输入 cardinality 改变语义的真实分支，违反本仓库当前要求的共享一般算法原则。

修正方向：把域名拒绝规则与扫描复杂度分开，在确定的方言规则下对候选后缀提供可复用的扫描进度或索引。不能只删除阈值分支并重新引入旧的二次问题。此行为有继承的兼容性背景；修复时需同步规范、兼容性基线和长失败候选测试，不能静默改变 grammar。

## 8. [P2] Scope 的实现、规范文字和审计不变量不一致

位置：[canonical-ast.md:68](https://github.com/nouprax/markdown-core/blob/3e6ad875/docs/specs/canonical-ast.md#L68)、[base.md:76](https://github.com/nouprax/markdown-core/blob/3e6ad875/docs/specs/dialect/base.md#L76)、[audit-position-places.mjs:80](https://github.com/nouprax/markdown-core/blob/3e6ad875/scripts/audit-position-places.mjs#L80)。

**实现方面符合用户明确的单位与直传约定。** [C facade](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/ast.c#L305) 直接复制 native 起止坐标；Swift、ES/WASM wire、Kotlin JVM/Native 投影没有转换成各语言 string index。Swift 的 Scope 注释也明确不应拿来取 substring。内部 source marks 将去除前缀、解码或拼接后的输入片段映射回原始编辑器坐标，是必要的 parser 映射，不是改变公共 scope 单位。

输入 `> é &amp; 🚀\n> next\n` 的第一个 Text literal 是 `é & 🚀`，scope 却仍为 `1:3..1:15`；第二行 Text 为 `2:3..2:6`。这些 Text 坐标与本地固定版本 cmark 0.31.2 相同。literal 的长度不能推导 scope。

剩余不一致：

- canonical spec 将起止定义为首末字节，并写入“inclusive”及切片式解释；base spec 还建议需要换行 bytes 的消费者读取 scope。这会引导调用者把公共坐标当作 string range。
- 真正零字节输入的 Document scope 是 `1:1..0:0`，与固定版本 cmark 一致；文档却写 `1:1..1:0`，后者是只有换行等另一类输入所产生的结果。
- position-place 审计统一拒绝 line 0 和 end-before-start，无法容纳原样保留的 cmark 空文档值。当前语料没有覆盖真正零字节输入，所以 15,071 个坐标检查全绿不能验证该边界。
- Swift Position 注释声称 line 总从 1 起，未描述空文档的原生 sentinel。

修正方向：以用户明确的 cmark 坐标直传契约统一文档与测试，记录实际 sentinel，不给 native 坐标做修复或 half-open 归一化。测试应比较空输入、空行、LF/CRLF、非 ASCII、tab、实体、escape、嵌套容器、引用 occurrence 与表格的精确值。表格跨行单元格的 scope 可覆盖多个结构片段，不能为了满足 string-range containment 而修改它。

## 需要保留的两个设计观察

**Formula 的后处理转换构造新对象、复制 literal，然后释放旧 CodeBlock。** [formula.c:664](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/formula.c#L664) 仅保留 scope 和修剪后的 literal。输入 fenced `formula {#eq .wide k=1}` 最终得到 `anchor=null attributes={}`。但 [formulas.md:227](https://github.com/nouprax/markdown-core/blob/3e6ad875/docs/specs/dialect/formulas.md#L227) 已明确写了丢弃其他字段，所以不能把它直接认定为实现偏离现行规范。它仍是需要审查的语义转换及分配成本：确认通用属性是否也应丢弃，并在语义确定时使用一致的提交/转换机制，避免附加另一轮修复后处理。

**属性 recognizer 的索引按整个输入长度分配。** `markdown_core_attributes_end` 对以 `{}` 开头的 1 MiB 输入返回 end=2，却先申请 8,388,616 bytes 并执行 1,048,577 次索引工作。它保持线性复杂度，不能仅凭这个样例宣布算法错误；但稀疏属性场景有明显的辅助空间成本。后续评估应覆盖整个 grammar 的失败候选总工作与峰值内存，不应给短属性或某个长度阈值添加特例。

## 架构与所有权判断

- 当前“一次 native parse、各 binding 投影为不可变结果”的边界是合理的。共享 reference resource 与 occurrence 的 scope/属性属于不同语义，不能为了减少对象而混在一起；source mapping 与 canonical scope 也不是同一份重复状态。
- C 的统一构造、事务式 kind conversion、迭代释放是应保留的不变量。Swift 证明仅把构建和遍历改成迭代仍不足以保证生命周期完整。
- 已识别的主要冗余集中在固定 descriptor 注册、probe/open 重复物化、后处理借用/复制/删除与重复 consolidation。无需用额外同步缓存来解决这些问题。
- 没有在已审查的 parser 路径发现需要新增跨 parse 同步的理由。parser 状态保持局部；JVM 类加载/初始化的一次性同步有生命周期依据；ES 的同步 WASM 调用与实例所有权也不能简单当作可删的冗余。
- 并非所有遇到 opener 时产生的状态都应删除。用于闭合匹配的 delimiter/bracket 栈、最终保留为普通文本的 fallback、共享线性扫描索引都有必要性。重点是完整语义尚未成立时，是否已经分配最终对象或字段，又在否决后撤销。Directive 和 table 的 probe 已明确违反这一点；autolink 还存在已知为空却仍物化的情况。
- 修正顺序应先处理两项 P1，再统一 recognition → commit 的所有权与数据模型，随后消除静态注册和多余后处理成本。scope 文档/审计可独立统一；不需要引入坐标转换层。

## 实际验证范围与证据

| 验证 | 结果 |
| --- | --- |
| C Release，排除 benchmark | 88/88，通过 |
| C ASan 全套 | 88/88，通过 |
| C UBSan 全套 | 88/88，通过 |
| C TSan 并发专项 | 2/2，通过 |
| 仓库 benchmark | 8/8 完成；不构成上述新输入的回归门槛 |
| Swift | 37 tests / 10 suites，通过；另有优化版独立释放探针失败 |
| ECMAScript Node + 当前源码构建的 WASM | 41/41，通过 |
| ECMAScript conformance | 40/40，通过 |
| Kotlin JVM | 42 tests + 8 conformance，通过 |
| Kotlin Native macOS arm64 | 31 tests + 8 conformance，通过 |
| Source-list / AST projection / element-parser 边界 | 通过 |
| Position places | 15,071 个节点，0 findings；空输入模型仍缺失 |
| Scope containment | 16,206 个关系，30 个已登记差异 |
| Inline cmark positions | 71 项，43 个已登记差异 |

已登记的 containment 和 inline 差异不能直接当成 30/43 个新 bug；其中包括方言差异和非字符串范围的表格坐标。这次也没有通过修订 ledger 来消除新发现。

未执行完整的平台矩阵、Android/iOS 真机或模拟器、浏览器运行矩阵、全量外部 AST parity campaign。计时是本机诊断数据；allocation bytes 是累计申请量，包括 realloc 的目标容量，**不是峰值存活内存或 RSS**。没有将 sanitizer 或现有测试通过解释为全输入的性能/无泄漏证明。

独立审查产物保存在 [build/review](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review)，该目录被 Git 忽略；本报告是持久的审查记录：

- [scaling.py](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/scaling.py)、[scaling-baseline.jsonl](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/scaling-baseline.jsonl)：8 类规模递增输入和原始计时。
- [ancestor-probe.c](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/ancestor-probe.c)、[node-instrumented.c](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/node-instrumented.c)、[ancestor-work.txt](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/ancestor-work.txt)：确定的祖先访问次数。
- [probe.c](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/probe.c)、[allocation-baseline.txt](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/allocation-baseline.txt)：allocator、directive/table probe 与属性索引证据。
- [release.swift](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/release.swift)、[swift-optimized-release-results.json](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/swift-optimized-release-results.json)、[swift-lldb-backtrace.txt](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/swift-lldb-backtrace.txt)：普通释放与跳过释放的对照，以及主机调用栈。
- [semantic-probes.txt](/Users/donz/.codex/worktrees/1f65/markdown-core/build/review/semantic-probes.txt)：scope、域名阈值和 Formula 属性的原始输出。

对初审保留的基线构建，可重跑 `python3 build/review/scaling.py nested_media_mail nested_media_prose`、`build/review/ancestor-probe`、`build/review/probe`。Swift 用 `build/review/swift-release-optimized-probe 30000` 复现正常释放崩溃；追加任意第三个参数走诊断性的 skip-release 对照，该对照不是修复方案。


## 修复结果

8 项 issue 的实现和回归均已完成；issue 通过修复 PR 关联，合并后关闭。

| Issue | 修复 |
| --- | --- |
| [#232](https://github.com/nouprax/markdown-core/issues/232) | 一个常数时间 splice 服务 parser 的独立子树所有权转移和检查后的任意重挂接；保留公共重挂接的循环检查。所有 parser 调用者已迁移，删除旧 inline 专用 splice。 |
| [#233](https://github.com/nouprax/markdown-core/issues/233) | Swift 使用不可变平坦 records 和索引关系，删除递归 ARC 子树所有权。原测试的逐层释放规避已删除。 |
| [#234](https://github.com/nouprax/markdown-core/issues/234) | Directive probe 借用属性范围，完整 opener 成立后才物化属性；共用属性 recognizer，并汇总其工作计数。 |
| [#235](https://github.com/nouprax/markdown-core/issues/235) | Pipe row 使用同一借用 iterator 识别及提交；删除临时 row/cell 堆数组。continuation、header 不匹配、前置行识别均不物化几何对象。 |
| [#236](https://github.com/nouprax/markdown-core/issues/236) | 未命中邮件时保持 Text 原 buffer；只创建非空提交片段，删除重复 consolidation。 |
| [#237](https://github.com/nouprax/markdown-core/issues/237) | 固定 descriptor 表直接借用；删除五套每次 parse 重建的链表。测试扩展使用同一连续 registry，不增加全局缓存或锁。 |
| [#238](https://github.com/nouprax/markdown-core/issues/238) | 删除域名 10 段语义阈值；共享单调拒绝边界保持失败后缀扫描线性，同时允许 underscore 后的有效候选。规范同步说明这一继承行为的改变。 |
| [#239](https://github.com/nouprax/markdown-core/issues/239) | 统一 C/Swift/ES/Kotlin 注释、规范和审计；增加真实零字节及 UTF-8 边界测试。生产坐标保持原样。 |

Swift 子关系从数组改为只读 `MarkupCollection<Element>`，定义的分组关系使用
`MarkupGroups<Element>`，均支持 `RandomAccessCollection`；需要数组的调用者
使用 `Array(...)`。集合直接存入所属节点的 `Fields`，分组不占独立 record。
持有容器子树会保留整个不可变 Swift store，直到最后一个持有者释放。没有 native handle、
缓存、同步或清理队列。Document 独有的 metadata payload 间接存储一次，避免
其容量决定所有普通节点的 stride。详见 [Swift storage](../architecture/swift-storage.md)。
`MarkupStore` 集中字段查询和投影；29 个节点类型通过 `@Stored` 引用字段，
不再各自保存下标并检查 enum。定位信息保存在统一引用中，视图仍为 16 bytes。

### 修复证据

- 深宽组合 n=8,192 的 Media + mail 从 982.437 ms 降为 3.838 ms，Span 为
  4.622 ms。独立变化深度与宽度的回归检查完整树，源代码边界审计防止 parser
  再调用任意树重挂接 API；公共循环检查仍有原有测试保护。
- 空输入 C 分配由 90 次降为 19 次，读取输入前由 76 次降为 6 次。
  directive probe 仅可能分配共享 recognizer 索引，class word probe 为零；
  pipe continuation 和列数不匹配 header 为零；无命中 autolink 仅分配一个
  48-byte iterator，测试同时断言原 buffer 地址未变。
- Swift 正常释放与独立保留子树的 30,000／65,536 层回归通过，并通过 weak
  引用确认 store 已回收。独立优化版消费者也正常打印 `released`。
- Swift 同一 C 实现下的优化版前后测量覆盖空文档、宽文档和深层列表。
  未观察到稳定的宽文档耗时回退；4,000 层列表由约 4.0–4.8 ms 降为
  2.3–3.0 ms。主机诊断 workload 的峰值 RSS 由 42,270,720 bytes 降为
  36,519,936 bytes。计时和 RSS 是具体 workload 的观测，不是全输入保证。
- 域名测试覆盖阈值两侧、最后/倒数第二/倒数第三段 underscore、URL/www
  两种入口，以及 16–8,192 个重叠候选；确定的扫描工作不超过 3× 输入字节。
- C Release 88 项 correctness 加 8 项 benchmark，ASan/UBSan 各 88 项，
  TSan 并发 2 项通过。Swift 42 tests / 10 suites 及独立 consumer 的 2 项测试通过。
  定义分组与所有权测试亦通过 Release 构建；8,192 个 body 不再增加集合 record。
  ES 41 correctness / 40 conformance、Kotlin JVM 42+8、Native 31+8 通过。
- position places 15,072 项无新增差异（现在包含真正零字节输入）；
  containment 16,206 项及 inline positions 71 项仍保持原 ledger。

没有通过修改现有兼容性 ledger 掩盖回归。上文两个设计观察仍保持其原有边界：
Formula 字段丢弃是现行规范行为，属性索引是线性辅助空间成本，本轮不额外改变
这两项语义/算法。未运行完整移动平台或浏览器矩阵。
