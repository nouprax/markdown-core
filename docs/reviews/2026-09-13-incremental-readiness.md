# Baseline 性能与增量化准备度复审

> 本文保留 `5eca3bc1` 的复审快照。后续 #243 / #248 / #272 修复及重新测量见
> [baseline 性能修复记录](2026-09-13-baseline-performance-fixes.md)。原始数据未覆盖。

评审基准：`5eca3bc1598b0313a0ac6a9e69f078a309feb3e1`，macOS 26.6.2 arm64，
Apple clang 21、Release。当前 HEAD 已包含上次 review 的修复和后续 Markup 统一。

**结论：baseline 是可继续增量化的语义基础，但不能确认“性能已最佳化”。**
已有 attachment 与 Swift 生命周期修复在本轮验证中保持有效；本轮确认一个新的
P2 级复杂度/测试缺口，并量化了已知 attribute index 的空间成本。增量化还必须处理
source、语义解析和 binding projection 的全量成本，不能仅增加 feed 接口。

本轮交付为审查、实验及实现设计，**没有修改生产 parser**。剩余改进已列入
[实施 tasklist](../plans/2026-09-13-incremental-parsing.md)。原始可重现记录：
[PoC 说明](../../experiments/incremental/README.md)、
[实验数据](../../experiments/incremental/results.json)、
[既有 benchmark 数据](../../experiments/incremental/baseline-benchmarks.txt)。

## 1. [P2] 当前 key index 可被 bucket flooding 触发二次方工作，现有碰撞测试未覆盖它

位置：[`core/map.c`](../../packages/markdown-core/core/map.c) 的 `hash_key`、
`find_key_slot`、`markdown_core_key_index_lookup`；
[`pathological_runner.c`](../../packages/markdown-core/tests/runners/pathological_runner.c)
的 `pc_badhash` / `case_reference_collisions`。

当前索引采用固定的 64-bit hash、power-of-two capacity、linear probing，没有为
构造性 home-bucket collisions 提供最坏工作界。可以生成不同 normalized labels，
使它们在目标 capacity 下都从 bucket 0 开始。其 hash 值本身不同，故这不是找到
64-bit hash 相等的声明。固定预分配容量下，第 i 个键需要 i 次 slot visits，总计
`k(k+1)/2`。

本次 generator 调用真正的 `markdown_core_key_index_entry`，验证返回的 hash 与
slot displacement；同组 labels 也构成实际完整 reference document。生成输入的
时间不计入解析时间。

| k | 实际文档 bytes | 已验证的 insertion slot visits | 完整 parse + free 中位数 | 等长普通键 control |
| ---: | ---: | ---: | ---: | ---: |
| 256 | 6,165 | 32,896 | 81.75 µs | 63.46 µs |
| 512 | 12,309 | 131,328 | 194.17 µs | 124.08 µs |
| 1,024 | 24,597 | 524,800 | 501.42 µs | 249.33 µs |
| 2,048 | 49,173 | 2,098,176 | 1,629.29 µs | 552.96 µs |

slot visits 是产品 index 的受控插入实验计数；没有声称它等于完整 parser 的全部
hash 操作数。复杂度结论来自精确工作计数和代码路径，不依赖时间比值。

现有 `pc_badhash` 使用旧的 `h = byte + (h << 6) + (h << 16) - h`，只筛选
`h % 16 == 0`，与当前 hash 不同。因此现有 50,000 键测试通过不能证明当前索引
抗碰撞。当前实现的普通输入表现良好，问题是构造输入下的界及其验证缺口；这不是
新增的语法正确性失败。

建议 B1：给实际索引建立 collision/work gate，评估统一的有确定界索引结构；如果
采用 keyed hash，明确其缓解范围，不能将其写成无条件 O(1)。审计 reference、anchor、
specimen 等全部消费者及 OOM/borrowed-key 生命周期。仅换 hash 常数、加入键数阈值
或移除测试不是解决方案。新增 dependency indexes 不能沿用未经说明的界。

## 2. [P2 优化项] Sparse attribute 会为整个 inline 输入建立密集 reverse-DP index

位置：[`elements/attributes.c`](../../packages/markdown-core/elements/attributes.c)
的 `index_input` / `markdown_core_inline_state_attributes`。

该项在上一轮已作为设计观察保留，本轮没有将其重复宣称为新 bug。算法在单个
inline owner 内共享 index，防止重复失败扫描，仍是线性；问题是局部属性会触发
每字节两个 offsets 的辅助表。实际输入 `[x]{.a} ` 后接 1 MiB 的 `a`，只有一个很短
的属性，但 attribute work 为 1,048,592。

| 输入 | parse + free 中位数 | peak live requested bytes | attribute work |
| --- | ---: | ---: | ---: |
| 1 MiB plain text | 3.137 ms | 3,155,736 | 0 |
| 1 MiB text + 8-byte span prefix | 13.337 ms | 11,544,938 | 1,048,592 |

计数来自单独的 instrumented parse，含 recorder setup；peak 不包含 source、allocator
headers、realloc 的瞬时双驻留、Python 或 binding 内存，不能称为 RSS。

建议 B2：从统一语法事件/连续 runs 的识别模型研究更稀疏的索引及可恢复状态，同时
验证 dense/sparse、valid/invalid、重叠候选的时间与空间。不能用朴素 forward scanner
减掉这张表后重新引入二次方失败扫描。长活动 inline 的增量状态必须纳入同一设计。

## 3. 增量化的结构性成本与正确性边界

这些不是当前 one-shot API 的违约；它们是达到新目标必须修改的架构边界。

| 当前行为 | 增量化影响 | 设计要求 |
| --- | --- | --- |
| 一个完整 source → parser → finalize → parser dispose | 无可恢复 feed/finish/session | 保存逻辑 continuation，移除跨调用 raw source pointers |
| lookahead/EOF absence 只属于一次 parse | 追加 closer 可以推翻很早的失败判断 | 保存正负读取依赖，按生产者而非仅按 AST overlap 失效 |
| 全文完成 references、footnotes、headings、anchors | 只冻结旧 blocks 会保留错误资源/ID/kind | ordered declarations、正负 lookups、版本化语义传播 |
| mutable nodes 有 parent/sibling、绝对 scope 和资源 | 直接共享到新版本会修改旧快照或使位置失效 | immutable records、相对 source maps、版本化 NodeRef |
| 每 parse 对 source 和 literals 做完整输入/投影 | C 增量后仍可能被 copy/wire 成本主导 | source pieces、literal chunks、delta wire |
| Swift flat store/JNI/WASM 完整投影 | 每个更新都复制 N 个 records 仍是 O(N) | 分段持久化快照；旧 Document 显式 materialization |

代码路径包括 `core/blocks.c: S_parse_source/S_finish_parse`、
`elements/document.c`、`elements/heading.c`、`elements/footnote.c`、
`elements/specimen.c`、Swift `DocumentBuilder`、ES `runtime/parser.ts` 和
`bridge.c: es_parse`、Kotlin `markdown_core_kotlin_jni_payload.c` / `JniPayloadDecoder`。

16 个可执行 append witnesses 展示了 remote references、heading references、named
footnotes、explicit anchor reservations、匿名 footnote IDs、percent comment、setext、
pipe table、前后 caption、definition list、list tightness、block identifier、Properties、
fence/directive 等问题。一个 witness 可覆盖多个边界；不是语法库存数量。
例如 `# title` 先生成 `title`，后面追加 `# other {#title}` 后旧 heading 变成
`title-1`；`^[body]` 的旧 ID 也会因后面追加 `[^inline-1]: named` 而改变。

严格结论是“不能无条件冻结旧前缀”，不是“所有 append 都必须全量解析”。

## 4. 本轮再次验证的既有性能与生命周期不变量

对 `![` × depth + `a@b.co ` × width + `](u)` × depth，独立与组合扩大两个维度。
depth=width=4,096 时输入 53,248 bytes，parse + free 中位数 1.958 ms；相关 allocations
随 depth+width 增长。本轮没有复现旧的 ancestor-walk 二次方问题，且正式 regression
和 parser-boundary audit 继续通过。这不是对所有元素组合的完整复杂度证明。

Swift 46 tests / 10 suites 通过，其中 deep root 和 independently retained subtree
的自然释放测试继续覆盖 30,000/65,536 深度。当前 flat-store 模型没有出现上一轮
递归 ARC 销毁问题；增量版本必须保留该生命周期保证，不能重新引入递归共享页回收。

固定 registry、directive/table borrowed recognition、autolink 无匹配 buffer retention、
domain 无阈值语义、资源共享、线性 delimiter/source-map/candidate work 的既有测试
本轮均随 C 完整测试运行。Scope 延续当前 pass-through 契约；不使用 node literal
长度重建原始 source range。

另修正了一处既有架构文档：`heading-resolution.md` 原称 heading closure order 总是
source order、无需排序，但 deferred mapped table cells 会打破这一点。当前代码已在
`markdown_core_block_prepare_headings` 使用共享 radix operation 按原始坐标排序；本轮
只把文档对齐到实现，未修改解析行为。

## 5. 增量 PoC 的结果和解释

PoC 的 island parser 只接收无缩进 ASCII 字母/空格/LF prose；完整语法全部仍由 baseline
C parser 执行。这个受限输入域仅用于建立独立 paragraph 边界，不是生产 fast path。

| 场景 | 更新数 | full parse bytes | island parse bytes | 本机累计 update 时间（full → island） |
| --- | ---: | ---: | ---: | ---: |
| 34,816 bytes 短段落，64-byte chunks | 544 | 9,487,360 | 52,224 | 116.58 → 3.12 ms |
| 34,816 bytes 单个长段落，64-byte chunks | 544 | 9,487,360 | 9,487,360 | 42.18 → 54.04 ms |
| 8,192 段，128 个局部变长 edit | 128 | 49,291,264 | 6,144 | 692.23 → 1.21 ms |

短段落说明复用历史结构的价值。长段落明确显示仅重解析活动 block 的方案无效，
额外 bookkeeping 还会更慢。editor 的 128 次 source lookup 合计 1,792 个 Fenwick
查找步骤；文档从 128 增至 8,192 段，送入局部 parser 的总量仍为 6,144 bytes。

实验做了 1,392 次逐 prefix 和 128 次逐 edit canonical 比较，大 workload 仅比较最终
快照；所有比较包含 scope 和全部 dump fields。源文本 flatten、oracle 和 dump 不在
计时中。时间包含 Python、native parse/free 和 source/island 更新，不包括 binding
projection、delta publication、UI、retained history 或任意跨岛编辑。不能把表中比例
当成完整产品速度提升。

## 6. 评审范围与验证

| 范围 | 本轮证据 |
| --- | --- |
| C core / element driver | 阅读生命周期、ownership、失败路径、扫描/索引及 mapped source 接口；运行既有 complexity/OOM/stress tests |
| 完整 baseline Release | 88/88 通过，包括 correctness 和 conformance |
| C ASan | 88/88 通过 |
| C UBSan | 88/88 通过；最终以 `halt_on_error=1` 运行 |
| 既有 benchmark | 8/8 完成，含 corpus guard；仅诊断数据 |
| Swift | 46 tests / 10 suites 通过，含 conformance 和 ownership |
| AST/source-list/parser-boundary audits | 43 kinds / 16 surfaces、59 sources / 5 lists 及元素边界检查通过 |
| Reference/position audits | reference order 2 cases 无新差异；15,072 position cases 无新差异；16,206 containment relations 维持原有 30 条 ledger 差异 |
| Kotlin / ES | 本轮审查输入、native wire、投影与生命周期代码；未重新运行其 runtime 测试矩阵 |
| 增量 PoC | native allocation runs 全部回到 live bytes=0；hash 工作证明、16 witnesses、受限 incremental differential checks |

本轮不声称完成 Android/iOS devices、Kotlin、浏览器、TSan 或所有外部 parser parity
矩阵，也不声称给生产增量 API 做过 OOM/并发验证，因为该 API 尚未实现。完整发布
验证在实施 tasklist 的 M7 中。测量都来自本机环境，不外推到手机/浏览器性能。

本次没有新增 P1 级失败证据，也没有把先前已修复的问题重新列为未解决。
“没有在这些测试中发现其他问题”不能证明全局最优。建议按 B1/B2 补强 baseline，
同时从 M0/M1 建立语义与存储基础，再依次解决局部语法、语义依赖、长活动块和
端到端投影。
