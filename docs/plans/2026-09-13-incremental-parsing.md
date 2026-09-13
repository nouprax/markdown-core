# 增量解析实施 tasklist

状态：review、调研和实验已完成；下面的生产实现任务均尚未开始。基准 HEAD 为
`5eca3bc1`。本文件是**当前推进计划**，不同于本目录中已完成的语法扩展历史计划。
设计依据：[架构](../architecture/incremental-parsing.md)、
[性能复审](../reviews/2026-09-13-incremental-readiness.md)、
[调研](../research/2026-09-13-incremental-parsing.md)、
[PoC](../../experiments/incremental/README.md)。

## 已完成的准备工作

- [x] R0：复查当前 C 驱动、元素生命周期、mapped inputs、引用/heading/definition
  resolution、source mapping、Swift store 与 JNI/WASM 投影边界。
- [x] R1：运行 C Release/ASan/UBSan、现有 benchmark、Swift 测试；记录实际覆盖，
  不把未跑的平台写成已通过。
- [x] R2：用 depth × width 复查先前 attachment 问题；补充 sparse attributes 与当前
  hash bucket collision 的可重现实验。
- [x] R3：调研官方 cmark、Lezer、Tree-sitter、micromark、streaming-markdown、
  source tree 和 immutable snapshot 设计。
- [x] R4：以实际 baseline 实验 short/long paragraph streaming 和局部 editor edits；
  保存 parsed-byte/work 数据和 canonical differential checks。
- [x] R5：构造 16 个后续输入改变既有前文的语义反例，形成设计、架构和实施路径。

## 依赖与里程碑

```text
M0 契约/测试骨架
  → M1 source/坐标/事务
  → M2 可恢复 block 与依赖读取
  → M3 全方言正确的局部重解析
  → M4 语义依赖增量化
  → M5 长活动块与 inline continuation
  → M7 端到端门禁与迁移收口

M1 → M6 不可变分段快照/Delta/bindings → M7
B1/B2 baseline 优化与证据补强 → M4/M5 的性能验收
```

M6 的生命周期和 ABI 设计必须在 M0 锁定；分段存储基础应与 M1/M2 对齐，不能等
C parser 全部完成后才发现无法在 bindings 中复用。图中分支是依赖关系，不要求使用
多代理或同时修改同一代码。每个任务可拆成独立 PR，必须有可执行退出条件。

## B：baseline 的剩余性能工作

- [x] **B1 · key index 的对抗性边界（#272，P1）**。统一压缩 radix tree 已替换线性探测，
  保留所有消费者的 borrowed-key / source-order / resource / OOM 契约。真实 baseline hash
  碰撞回放、radix 位序结构 gate、C / sanitizer 测试及同机对照已完成。
  详见 [修复记录](../reviews/2026-09-13-baseline-performance-fixes.md) 与
  [索引架构](../architecture/key-index.md)。
- [x] **B2 · Attribute recognition 的 baseline 空间模型（#273）**。按需前向 facts
  取代整段 reverse DP；同 corpus 的 sparse/dense work、累计申请和 peak gates，以及
  成员后缀和裸值重叠的查询顺序验证已实现。所有者与 EOF 依赖见
  [属性架构](../architecture/attribute-recognition.md)；原始对照见
  [修复记录](../reviews/2026-09-13-inline-performance-fixes.md)。这里完成的是 baseline
  识别和续接所需的事实模型；实际版本化失效、挂起和恢复仍由 I52 验证。
- [ ] **B3 · 分阶段基准**。责任：C benchmark 与 bindings benchmark。分离 input copy、
  block、inline、semantic、projection、free；补充长 code/paragraph、后置 definition、
  顶部换行、深×宽、快照保留。退出：固定 generator/版本、raw 数据和一条复现命令；
  CI gate 使用 work invariants，时间结果用于诊断。

按当前任务顺序，先推进 [baseline issues 修复](2026-09-13-baseline-performance.md)，
再实现增量解析。B1/B2 完成不代表整体“性能已最佳化”；B3 及其余 baseline 工作仍需验证。

## M0：冻结可验证契约

- [ ] **I00**：确定 Snapshot、Session、EditBatch、ChangeSet 和 NodeRef 的公开契约；
  决定新 C value handle API 与旧 Document materialization 的边界。更新 schema/API
  文档，覆盖 Swift/Kotlin/ES 的字段与 ownership，不先发布临时 API。
- [ ] **I01**：锁定 UTF-8 byte ranges、同旧版本 batch、revision conflict、空 edits、
  容量限制、输入前置条件、OOM/取消回滚和 single-writer/multi-reader 规则。
- [ ] **I02**：建立全字段 differential comparator；比较 scope、资源共享等价类、
  metadata、owned labels/captions/definitions/citation affixes；oracle 不能与被测
  增量缓存共用状态。
- [ ] **I03**：把 PoC witnesses 转为正式 source+edit fixtures；补充删除、replacement、
  duplicate winner/loser、负 lookup、CRLF、Unicode、table mapped inputs。
- [ ] **I04**：定义 workStats；测试 source copy、scan/replay、hash probes、index visits、
  分配/复用记录、literal/wire bytes、释放页。为后续每一步提供无时间依赖的门禁。

**退出条件**：exact-prefix、失败后继续使用和跨版本所有权都能由测试表达；API 决策
已形成可评审文档。尚不能宣称任何增量性能收益。

## M1：版本化 source、位置与事务

- [ ] **I10（I00–I01）**：实现 immutable chunks + balanced piece tree，支持
  split/join/insert/delete/replace 和首次 bulk build。度量树高度、碎片和 copied bytes。
- [ ] **I11（I10）**：实现可组合 LF/CR/CRLF 摘要与 byte/line cursor；覆盖 CRLF 跨
  pieces/edits、tab、非 ASCII 和 NUL 的既有 source mapping 行为。
- [ ] **I12（I10–I11）**：实现 source anchors、relative/mapped ranges、version position
  map；前部换行不重写 suffix records，查询不回放无限 edit history。
- [ ] **I13（I10）**：实现 candidate source transaction；全部分配点 OOM sweep、
  revision/range/overflow 拒绝、失败后下一有效 edit 和旧快照继续读取。
- [ ] **I14（I02、I13）**：先连接完整 parser，产生所有输入正确的 Update；显式
  `full_reparse` work reason。它是迁移 scaffold，不能作为完成增量的验收结果。

**退出条件**：任意范围的 source edit 与直接应用 bytes 等价；位置/版本/rollback
通过；未修改历史 source 不复制。全部语法仍然正确。

## M2：可恢复 block engine 与显式读取依赖

- [ ] **I20（M1）**：以 cursor/input abstraction 替换借用连续 source 的长期 raw
  pointers；同一 driver 支持普通输入和 mapped cell inputs；更新全部 callers。
- [ ] **I21（I20）**：定义 persistent container frames 和 checkpoint record，分离
  输入状态、语法事实与临时 workspace；禁止 parser memcpy 和每行 O(depth) 栈复制。
- [ ] **I22（I20–I21）**：所有 block recognizers 记录 read ranges 和 EOF/negative
  dependencies；`matched/rejected/need more` 只改变工作流，不改变 EOF 方言。
- [ ] **I23（I22）**：实现区间依赖失效及 earliest-producer restart；覆盖注释远端
  closer、Properties、table candidate/caption 和隐藏 definition barrier。
- [ ] **I24（I21–I23）**：实现严格状态/来源等价的同步点复用；hash 碰撞测试不得
  使不同状态被当成相同。不能以几行文字相同或 AST kind 相同替代证明。
- [ ] **I25（I24）**：迁移全部 block elements，含 lists/definitions/directives/callouts、
  四类表格、deferred cells 与 source order。原有统一 opener/probe 语法继续唯一。

**退出条件**：普通局部 edit 可复用无关 block；所有 candidate 读取都可追踪；遇到
不稳定区域能正确扩展到 EOF。每个中间 prefix 与 oracle 等价，生产支持完整方言。

## M3：全方言正确的局部重解析

- [ ] **I30（M2）**：以完整 inline owner 为第一复用单位，保留成功和失败的候选
  原始事实；source 改动和语义依赖变化都能触发 owner 重解析。
- [ ] **I31（I30）**：所有 owned fields/mapped inputs 使用同一队列与 invalidation：
  directive labels、callout titles、table captions/cells、definition terms/bodies、
  footnotes/specimens、citation affixes。
- [ ] **I32（I30–I31）**：把 consolidation/autolink/formula completion 的输入与输出
  归于可失效 owner；审计完整树调用者并移除替代后的全树重复 pass。
- [ ] **I33（I02、I32）**：全 fixtures 执行 every-prefix、随机 chunk partitions、
  可重现随机 edits、多 edit batch 与 undo/redo；自动 shrink 失败序列。

**退出条件**：完整方言更新正确，block/inline owner 级复用成立。此时仍可能因全局
resolution、长活动 owner 和投影产生 O(N) 更新成本，必须在报告中保留这些限制。

## M4：文档语义增量化

- [ ] **I40（M3）**：建立 source-ordered declaration multimap，保留 duplicate losers，
  实现 explicit/implicit priority；winner 删除/移动时正确选择后继。
- [ ] **I41（I40）**：记录 reference/footnote/specimen 正负 lookup；只使受影响 owner
  失效，避免 global generation counter 使全文重算。
- [ ] **I42（I41）**：版本化 shared resources 与 occurrence overrides，区分语法变化
  和 destination/title/attributes 变化；旧快照不变，payload 不按 occurrence 重复复制。
- [ ] **I43（I40–I42）**：增量 heading declarations、有效 explicit reservations 和
  generated anchors，保留现有阶段顺序；记录 base/suffix 冲突实际传播。
- [ ] **I44（I40–I43）**：增量 named/inline footnote IDs、Document definitions source
  order；specimen 只保留 authored numbering 信息，展示编号仍由消费者计算。
- [ ] **I45（I43–I44、B1）**：验证无关定义变化不遍历全树，真实 fan-out 变化完整
  传播；补充长 label、anchor reservations、bucket flooding 和 repeated deletion。

**退出条件**：局部声明更新成本随实际依赖和索引操作增长。最坏全文变更可解释、
可计量；没有遗漏 negative lookup 或通过 relabel Text 修补语义的代码。

## M5：长活动块与高频 streaming

- [ ] **I50（M3）**：保留可恢复 inline cursor、delimiter/bracket/field continuation；
  EOF 的临时 canonical projection 与可继续状态分离，不克隆整个活动树。
- [ ] **I51（I50）**：Text/opaque literal 使用共享 chunks；实体、escape、autolink、
  code/formula/HTML/comment、CRLF 边界均用同一语法；改变旧结论时失效相关部分。
- [ ] **I52（I50–I51、B2）**：恢复 attribute/opaque/failed-candidate 扫描状态；
  obsolete EOF/negative facts 必须更新；计入所有 speculative replay，防止计数漏项。
- [ ] **I53（I50–I52）**：建立长 paragraph、长 fence、长 unclosed candidate、晚闭合
  delimiter、密集 references 与 deep×wide streaming 的 deterministic gates。
- [ ] **I54（M4、I53）**：统一 append 与 arbitrary edit 的状态失效/恢复；逐字节
  transport assembler 与 scalar-aligned Session feeds 得到相同已发布结果。

**退出条件**：dependency-free 长 Text/fence 不每 chunk 重扫/复制全部前缀。输出真实
变化造成的大工作量与算法额外成本分开统计；不以 token 合并或乐观补语法代替实现。

## M6：快照、Delta 与 bindings

- [ ] **I60（I00、M1）**：实现 immutable record pages、persistent relations 与版本化
  resource/literal chunks；目录/root relation 修改不能复制整个记录表。
- [ ] **I61（I60）**：实现 allocation-free 或明确受控的迭代回收；测试 deep root、
  retained subtree、跨版本 shared pages、Session close 和大量历史释放。记录 retention。
- [ ] **I62（I60、M3）**：提供 C Snapshot/NodeRef、分段 literal、顺序遍历和 lazy
  position resolution；旧 Document 显式 materialization，全部旧契约测试通过。
- [ ] **I63（I62、M4）**：定义 revisioned ChangeSet/wire，含结构、资源和位置变化；
  delta apply 后结果等价 snapshot。缺 revision 时支持显式 resync，无静默重用。
- [ ] **I64（I60–I63）**：Swift typed segmented store、Sendable Snapshot 与集合；
  新 TextView 显式 String copy。重跑 normal release 和 retained subtree consumer。
- [ ] **I65（I60–I63）**：Kotlin JVM/Android JNI 与 Native session/delta 接口；显式
  close、异常回滚、共享资源与多线程读取；不保留整份 native 与 decoded 历史。
- [ ] **I66（I60–I63）**：ES/WASM persistent session、edit input、delta decoder；
  memory growth 后更新视图、Dispose/resync、worker 示例和浏览器验证。
- [ ] **I67（I64–I66）**：同步 canonical schema、所有 source lists、exports、包内容、
  docs 和 consumer examples。默认操作只触及变更页，不做隐式 whole-Document export。

**退出条件**：C 的局部复用收益能穿过 bindings；文首换行与长 Text append 不产生
全量 wire/string/record copy；旧快照和旧 API 生命周期仍然明确、安全。

## M7：发布前收口

- [ ] **I70（M4–M6）**：C、Swift、Kotlin JVM/Native/Android、ES Node/browser 的
  end-to-end update 基准；分别报告输入更新、解析、resolve、projection、release、
  retained memory。固定 chunk/edit 序列和工具链，保留 raw artifacts。
- [ ] **I71（I70）**：ASan/UBSan/TSan、OOM/取消 sweep、跨 revision 与 retained
  snapshot stress、随机编辑 fuzz；与独立 baseline oracle 比较全部 canonical 值。
- [ ] **I72（I71）**：移除被取代的独立 one-shot 驱动和临时全量 fallback；one-shot
  与 Session 使用同一语义算法。留下的兼容 materialization 明确标注 O(N) 和生命周期。
- [ ] **I73（I72）**：完整 repository checks、全部 package consumers、平台构建与
  external conformance；新文件入 index 后重跑 tracked-file audits，shebang 文件确认
  `100755`；需要 clean checkout 的检查在 commit 后、push 前执行。
- [ ] **I74（I73）**：补齐 API/迁移文档、LLM streaming 和 editor edit 示例、性能
  限制与 chunked text 消费说明，按发布流程验收。

**完成定义**：完整方言 exact-prefix/edit equivalence、snapshot lifetime、事务失败恢复
和端到端工作界全部有证据。不能以“短段落 PoC 很快”“C parser 已增量”或“多数
测试通过”替代完成定义。
