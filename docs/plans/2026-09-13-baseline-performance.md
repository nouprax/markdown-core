# 3.0 baseline performance remediation

状态：进行中。已完成 #244、#247、#272、#273；#243、#248 部分完成，另有 26 项待处理（其中 #271 为汇总 issue）。先完成 baseline 性能修复，再推进增量解析实现。审查基线：`5eca3bc1`。

Issues 中的测量是待独立验证的证据；建议中的 API、所有权和复杂度变化必须符合当前规范。完成项需要代码、语义/失败边界测试及可复现的性能证据，不能以单个计时结果代替。

## Task list

- [ ] [#243](https://github.com/nouprax/markdown-core/issues/243) [P1] 表格元素在每行、每层新开容器上发起推测 lookahead，深层嵌套列表的缩进续行退化到 Θ(D²) 以上
- [x] [#244](https://github.com/nouprax/markdown-core/issues/244) [P1] Inline 位置模型对每个 inline 节点做 4 次二分查找和一次 memcmp，长段落上占 20% 指令
- [ ] [#245](https://github.com/nouprax/markdown-core/issues/245) [P1] 一次 parse 事务的堆分配次数是节点数的数倍：节点、delimiter、iterator、block content 各自独立 calloc，逐节点释放
- [ ] [#246](https://github.com/nouprax/markdown-core/issues/246) [P1] 节点记录 224 字节：attributes 向量、content strbuf、user_data 与 content-mark 字段出现在每一个 inline 节点上
- [x] [#247](https://github.com/nouprax/markdown-core/issues/247) [P1] 文本 run 在 `w`、`:`、`!`、`<`、`%`、`$`、`~` 等字节上被无谓切分：普通英文散文 4 倍分配，不可配对的 `~` 21 倍
- [ ] [#248](https://github.com/nouprax/markdown-core/issues/248) [P1] `markdown_core_text_parse` 对每个文本字节做 UTF-8 解码与 Unicode 空白分类，只为记录最后一个空白边界；散文上约 19% 指令
- [ ] [#249](https://github.com/nouprax/markdown-core/issues/249) [P2] Block 起始分派对每行、每个新开容器顺序遍历全部 32 个 descriptor 四轮，没有首字节索引
- [ ] [#250](https://github.com/nouprax/markdown-core/issues/250) [P2] 定义列表在每个段落起始行都发起一次 lookahead 事务，并对 `[` 开头的行完整跑一次引用定义解析
- [ ] [#251](https://github.com/nouprax/markdown-core/issues/251) [P2] Block 阶段之后至少 6 次全树遍历，且每个 inline 节点在创建时都调用 `visit_inline_subtrees`
- [ ] [#252](https://github.com/nouprax/markdown-core/issues/252) [P2] 每个 inline root 的 init/finish/dispose 各遍历 32 个 descriptor，短 block 密集的文档上约 5% 指令
- [ ] [#253](https://github.com/nouprax/markdown-core/issues/253) [P2] 发布构建没有 LTO 与隐藏可见性；平凡查找函数跨 TU 调用；SwiftPM/Wasm/Kotlin 各自的 C 编译配置也不一致
- [ ] [#254](https://github.com/nouprax/markdown-core/issues/254) [P2] 源字节在 block 与 inline 阶段被复制三次以上；行尾查找逐字节；围栏代码块整体 memmove
- [ ] [#255](https://github.com/nouprax/markdown-core/issues/255) [P2] 文档含标题后每个 `]` 都对括号内容做 case-fold 查表；脚注引用归一化三次、定义两次并复制
- [ ] [#256](https://github.com/nouprax/markdown-core/issues/256) [P2] 管道表格每个正文行被扫描三次，header 识别重扫整个段落并三次 strlen；网格几何工作区逐候选分配
- [ ] [#257](https://github.com/nouprax/markdown-core/issues/257) [P2] 首个 `{` 触发整段 8 字节/字节的属性后缀索引并双重解码；Unicode 类别谓词没有 ASCII 快路径
- [ ] [#258](https://github.com/nouprax/markdown-core/issues/258) [P2] 约 25 个复杂度工作计数器在发布构建的逐字节内循环中无条件自增
- [ ] [#259](https://github.com/nouprax/markdown-core/issues/259) [P2] 每个 inline 构造 2～6 次独立堆分配与源字节复制：bracket、citation、formula、directive、code span、heading、comment、cross link
- [ ] [#260](https://github.com/nouprax/markdown-core/issues/260) [P3] 生成扫描器逐字节边界检查；citation 花括号预扫描用私有 HTML flags 重复扫描；两个从未调用的扫描器仍被生成
- [ ] [#261](https://github.com/nouprax/markdown-core/issues/261) [P3] Element 侧其余重复扫描：directive 属性索引三次、段落引用定义解析两次、properties 三遍访问、ATX 标题重数 `#`、每行标题都跑属性尾扫描
- [ ] [#262](https://github.com/nouprax/markdown-core/issues/262) [P1] Swift 读侧：每次元素访问经 `any Sendable` 装箱与动态转换、整份 `Fields` 复制；叶子类型超出 existential 内联缓冲；walker 用 43 步 cast 链分派
- [ ] [#263](https://github.com/nouprax/markdown-core/issues/263) [P2] Swift `Document.parse` 复制整个源、子链双走、`get_kind` 双调用；每个 literal 一次 String 分配加 UTF-8 校验
- [ ] [#264](https://github.com/nouprax/markdown-core/issues/264) [P1] Kotlin JVM/Android 解码器：每节点约 5 个 lambda 与一次 megamorphic invoke、11 次分配的空 Attributes、3 个对象的 Scope、列表构建 2～4 次复制且无 RandomAccess
- [ ] [#265](https://github.com/nouprax/markdown-core/issues/265) [P1] Kotlin/Native 物化每节点 10～12 次 cinterop 调用、CValue/memScoped 往返与约 15 次分配；应复用 JNI 的扁平 payload 编码器
- [ ] [#266](https://github.com/nouprax/markdown-core/issues/266) [P2] Kotlin 其余常数项：walker 每节点两个 Pair 与两次 43 路 instanceof 链、JNI 源字符串可避免的第二次复制、payload 编码器逐字节写入与子链双走、`get_kind` if 链
- [ ] [#267](https://github.com/nouprax/markdown-core/issues/267) [P1] ES 解码器每节点常数项：`defineProperty` 注入 dump 闭包、空 Attributes 重建并冻结、`base()` 双算、`record()` 12 次分配且跑两遍、每个字符串一次 `TextDecoder.decode`
- [ ] [#268](https://github.com/nouprax/markdown-core/issues/268) [P2] ES 传输与构建：源两次复制、每节点 160 字节 wire 记录加 184 字节中间记录与三遍 C 遍历、Wasm 构建缺少 LTO/bulk-memory/INITIAL_MEMORY、加载器多复制一次二进制且不用 instantiateStreaming
- [ ] [#269](https://github.com/nouprax/markdown-core/issues/269) [P2] 四个 dumper 都按行重建树形前缀：O(行数 × 深度)，10,000 层嵌套的 dump 需数十秒
- [ ] [#270](https://github.com/nouprax/markdown-core/issues/270) [P3] 基准方法：样本是 73～3,789 字节的小文件重复 200 次，没有吞吐、参考实现对照、分配/指令计数与绑定侧测量，深层嵌套只测单行
- [ ] [#271](https://github.com/nouprax/markdown-core/issues/271) [P1] 3.0 baseline 性能总览：相同 CommonMark 输入比 pinned cmark 0.31.2 慢 4～9 倍，每节点约 300～400 ns，累计分配为输入的 213 倍
- [x] [#272](https://github.com/nouprax/markdown-core/issues/272) [P1] key index 在真实 hash 的 bucket flooding 下退化为 Θ(k²)：refmap、heading anchor、footnote、specimen 全部受影响，现有 gate 只测 cmark 旧 hash
- [x] [#273](https://github.com/nouprax/markdown-core/issues/273) [P2] Attribute recognition 的空间模型：每个 inline root 无论属性多少都为整段申请 8 B/字节、316.8 Ir/字节，反向 DP 无法续接，每行路径逐行重建
- [ ] [#274](https://github.com/nouprax/markdown-core/issues/274) [P2] 分阶段基准：现有 runner 不计时 free、无任何阶段划分、workload_version 为字面量、原始样本被丢弃、样本无分隔拼接改变形状、work 计数器与 bindings 均未接入

## 当前实现顺序

- [x] 第一批：确定性复杂度：#272 key index、#243 的嵌套候选退化部分。
- [ ] 第二批：C 解析器数据流：#244–#261、#273，按共同根因合并修复。
- [ ] 第三批：Swift / Kotlin / ES 的存储、解码和访问成本：#262–#269。
- [ ] 第四批：基准及发布构建：#253、#268、#270、#274，复核 #271 总览。


## 第一批结果

代码及验证详见 [修复记录](../reviews/2026-09-13-baseline-performance-fixes.md)、
[key index 架构](../architecture/key-index.md) 和
[原始测量](../../experiments/baseline-performance/results.json)。复选框表示本地实现和验证完成，
不表示 GitHub issue 已关闭或变更已合并。

- #243：只有完整 dash separator 可以优先于列表/分隔线打开表格；失败位置在同一行的嵌套候选间共享。
  普通文本表头继续走原有表格入口，逐行 lookahead 固定成本尚未消除，与 #250 的共享行探测一起推进。
  lookahead 保留已扫描的缩进，并将祖先链建立、保存、恢复纳入工作计数。
- #272：统一压缩 radix tree 替换线性探测，支持二进制 key、前缀 key、借用生命周期与无分配 commit。
  真实 baseline hash 的碰撞回放覆盖 references / anchors / footnotes / specimens。
- #248：普通 text slice 的空白只需要最后一个 boundary；从尾部扫描，保留现有 Unicode 空白集合。
  delimiter 两侧的 ASCII 分类路径仍待完成，因此本 PR 不关闭该 issue。

## 本轮 #244、#247、#273

- [x] 共用端点定位结果，前向 mark 游标与非单调回查都保留准确源位置。
- [x] 按 owner 仲裁 `can_start`，让已知字面符号保持连续 Text，并保留共享 delimiter 语义。
- [x] 属性改为按需前向 facts，覆盖重叠失败的工作和空间上界，Directive scan/apply 共用识别。
- [x] 独立对照、旧属性语法差分与边界验证见 [修复记录](../reviews/2026-09-13-inline-performance-fixes.md)。

## PR benchmark 与 CI

- [x] 修复 MSVC C4244：radix 分支与局部 mask 统一为 `uint16_t`，保留 warnings-as-errors。
- [x] base/head 在同一 runner 按精确 SHA 干净构建，移除历史 baseline 和 fallback 路径。
- [x] 共用一个 driver、输入和 CPU affinity；新进程、预热、ABBA 轮换与完整原始样本。
- [x] 分开 parse/free，配对估计和整组 bootstrap 区间；校验 canonical 输出和两侧身份。
- [x] reporter 只执行其 workflow revision，校验当前 base/head、run attempt 和有界结果。
- [x] 本机真实流程与同 revision A/A 校准通过；方法及复现见[测量契约](../architecture/pr-benchmark.md)。

#270 / #274 的更广泛 workload、parser 内部分阶段和 binding 侧基准仍未完成。

## 需要保留的审查结论

- #244 的“节点总是按 offset 单调产生”只适用于前向 token；bracket、citation、attribute
  completion 会回到 opener。后续位置缓存必须覆盖这些非单调查询，不能用任意方向线性回退。
- #245 的 arena 不能改变 detached subtree / shared resource 的独立存活契约；应与 #246
  一起从分配所有权设计，不能把所有节点简单绑定到 Document 的一次 free。
- #253 需要检查实际目标编译参数：core 的静态目标有 hidden 设置，elements 中重新编译的
  public archive/shared product 以及绑定目标仍须统一，不能只检查一个 CMake 变量。
- #269 的完整树形 dump 本身需要 O(输出字节数)，深度链的输出有二次大小。可以消除
  临时数组及前缀重复构造，不能承诺整份 dump 相对于节点数线性；避免保存每层的完整前缀造成二次常驻空间。
- #270 / #274 的完整基准工程仍待实现；本次独立对照工具只提供当前三项修复的可复现实验。
- PR 首批 CI 的 +40.2% 来自独立 hosted-runner 的历史 base/head 对比；本机隔离 map
  约慢 3.5%，首批整体基本持平。新 pipeline 使用同一 runner 的交错对照，旧百分比不能
  与新结果直接拼接。详见[隔离记录](../reviews/2026-09-13-inline-performance-fixes.md#首批-map-改动与-ci-回退)。
