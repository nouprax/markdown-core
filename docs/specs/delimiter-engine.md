# Delimiter Engine 结构设计

状态：实施契约
日期：2026-07-30
范围：C inline parser、bundled inline extensions、复杂度与 OOM 契约

## 结论

Delimiter engine 必须把以下三个概念彻底分离：

1. **source trigger**：输入中触发 inline grammar dispatch 的真实 byte；
2. **delimiter rule identity**：决定哪些 opener/closer 可以配对，以及如何搜索的语法规则；
3. **owner**：创建 delimiter 并负责 reduction/finalization 的 core handler 或 extension。

一个 delimiter 在 push 时必须绑定唯一的 parser-local rule binding；之后的搜索、仲裁和
reduction 都不得再通过 source byte、extension registration order 或全局 extension list
反查 owner。

运行时表示使用可搬迁的 contiguous arena 和 opaque integer handle。Delimiter 同时存在于：

- 一条保持 source order 的 global chain；
- 一条只包含相同 rule 的 per-rule chain。

Opener search 只访问 per-rule chain。`openers_bottom` 不再按 byte 建二维数组，而是每条 rule
按其有限的 closer search class 保存 exhaustion floor。Link、image、directive label、
CrossLink 和 Embed 等共享 close trigger 的语法通过同一套 semantic-opener arbitration
决定 owner，registration order 不参与语义。

这是一套完整的数据模型，不允许保留 sentinel delimiter byte、phase-two owner lookup、
raw public delimiter prefix 或 benchmark shape fast path 作为并行机制。

## 1. 目标与非目标

### 1.1 目标

- CommonMark emphasis/strong、smart punctuation、link/image/footnote 与全部 bundled
  delimiter extensions 保持 canonical AST、scope 和 option semantics。
- 任意 delimiter-dense 输入的解析工作与输入 byte 数、实际创建的 delimiter 数及输出
  byte 数保持线性关系；不同 rule 的 delimiter 不互相放大 opener search。
- Extension attachment order 只定义 enabled set，不定义 grammar precedence。
- Delimiter allocation、link-boundary cleanup 和 OOM 行为拥有一个一致的生命周期。
- Parser 配置与 transient delimiter state 都是 parser-local，不引入 process-global mutable
  state。
- 数据结构直接表达 owner、rule、source span 和 stack boundary，不依赖 magic value 或
  callback 约定猜测。

### 1.2 非目标

- 不提供任意第三方 runtime grammar registry。Bundled extensions 仍是 immutable
  compile-time descriptors。
- 不改变 canonical AST kind、platform binding 或 installed read-only C facade。
- 不以当前 benchmark 的 delimiter 数、input size 或常见 cardinality 选择不同算法。
- 不在 delimiter engine 中解释 CrossLink/Embed reference 的 heading、block 或 display
  suffix。
- 不让 hash、pointer address、attachment order 或 allocator behavior 成为语义的一部分。

## 2. 术语

### Source trigger

输入中真实存在的 byte，例如 `*`、`_`、`[`、`]`、`!`、`$`、`\\`、`:` 或 `~`。
Trigger 只负责把当前位置路由到一个有序 handler bucket。

内部 delimiter identity 不是 source trigger，因此不得进入 trigger table、
special-character bitmap 或 flanking-transparent table。

### Delimiter rule

Immutable grammar descriptor。它定义：

- owner-local rule kind；
- pairing/search policy；
- closer search class 数量及分类方式；
- topology reduction policy；
- 可选的 shared-close claim 信息；
- 可选的纯 lexical close probe。

Formula 的四种 delimiter、CrossLink、Embed、directive label、strikethrough 以及 core
emphasis rules 都是独立 rule。两个 rule 即使使用相同 source bytes，也不共享 identity。

### Rule binding

Parser attachment 时从 immutable rule descriptor 编译出的 parser-local binding。它持有：

- owner descriptor；
- dense rule slot；
- reducer 与 search policy；
- owner-local rule kind；
- 当前 parser 中的 enabled/configuration state。

Rule slot 在一次 parser configuration generation 中稳定。

### Delimiter handle

`delimiter_id` 是 1-based arena index；`0` 表示 null。Handle 仅在当前 inline parse 及其
尚未 truncate 的 arena suffix 内有效，不得进入 AST、extension opaque payload 或跨
callback 保存。

### Delimiter mark

在进入 link/image/footnote inline ownership boundary 时保存的 stack snapshot，至少包含：

- arena allocation count；
- 当前 global-chain tail。

Mark 表示 delimiter lifecycle boundary，而不是 source byte offset。
Shared-close 的 claim clock 不属于 mark：它在一次 inline parse 中只增不减，inner truncate
不会让后来创建的 opener 获得已经使用过的 order。

Phase two 不允许 push，也不会把部分 reduction state 暴露给调用者；因此一个合法 mark 的
非空 `tail` 必须等于其 physical `count`。Process/truncate 拒绝 count 范围内但不代表真实
arena prefix 的 forged mark，并保持 engine state 不变。

## 3. Grammar plan 与 attachment

### 3.1 Parser-local compiled plan

Parser 持有一个编译后的 inline grammar plan：

- `dispatch[256]` 将 source byte 映射到 contiguous extension-handler range；
- `close_dispatch[256]` 将 shared close trigger 映射到 contiguous rule-binding range；
- `seam_dispatch[256]` 保存需要纯条件探测的 incremental seam handlers；
- source-special bitmap 由非空 handler range 直接导出；
- flanking-transparent bitmap 只包含真实 source bytes；
- seam-barrier bitmap 只包含真实 source bytes；
- rule bindings 使用 dense slot；
- shared-close buckets 直接保存 claim rule binding。

Plan 在 parser construction/extension attachment 时建立，并在 parser renew 时保留。Inline
parse 不遍历 `inline_extensions` linked list，不扫描每个 extension 的
`special_inline_chars`，也不临时 add/remove special characters。

Core 的固定 CommonMark routing 仍由 inline parser 的 built-in switch 表达；extension
bucket 只负责已 attachment 的 descriptors。两者的语义接缝由真实 source grammar
决定：例如 `\`、`[` 和 `!` 在相应 built-in handler 前探测 extension form，而所有共享
close trigger 进入统一 arbitration。当前 bundled descriptors 在普通 extension trigger
上不存在两个 matcher 竞争同一 source form；若未来加入这种 grammar，必须先定义独立于
attachment order 的语义仲裁，不能把 bucket iteration order 当作 precedence。

### 3.2 Attachment 原子性

Attachment 必须先完成以下全部准备，再一次提交：

1. 验证 descriptor、duplicate attachment、rule/trigger/seam 契约；
2. 分配 attachment entry 与 dense rule bindings；
3. 为所有 dispatch、close-dispatch 和 seam-dispatch buckets 预留容量；
4. 成功后一次性发布 bucket counts、bitmaps、rule count 和 attachment link。

任何 allocation/validation 失败都必须保留旧 plan 与旧 extension set。禁止出现 extension
已进入 block list、但 inline rule 或 trigger 只注册一部分的半附着状态。
失败的 reserve 可以留下更大的私有 capacity，但不得改变任何可观察 count、binding、
bitmap 或 extension membership；capacity 不是 grammar state。

同一个 descriptor 的重复 attach 必须明确拒绝或幂等；不得重复执行 handler 或
postprocess。

Parser 第一次 feed 后 grammar configuration 冻结。若未来需要 mid-feed attachment，必须以
新的完整 configuration generation 原子替换 plan；不得在正在使用的 plan 上增量打补丁。

## 4. Arena 与 record 表示

### 4.1 Contiguous relocatable arena

Delimiter records 存在几何增长的 contiguous vector 中。Vector growth 使用 parser
allocator 的 realloc 语义；所有 record 间引用都是 handle，因此 relocation 不需要修复
pointer graph。

不得按完整 input size 预分配“每 byte 一个 record”，也不得先扫描一次输入计算 delimiter
数量。统一采用 amortized geometric growth；capacity 是存储实现细节，不影响 grammar。

概念上的 record 包含：

```text
global_previous / global_next       mutable source-order chain
rule_previous / rule_next           mutable live per-rule chain
push_previous_rule                  immutable state before this push
scope_before                        immutable phase-one scope state before this push
text_node                           delimiter literal node
rule_slot                           parser-local owner/rule binding
source_start / source_end           consumed source span
original_run_length                 immutable pairing length
remaining_run_length                reducer progress
opener_order                        semantic opener order, when applicable
can_open / can_close
active
```

`original_run_length` 和 `remaining_run_length` 必须分开。CommonMark rule-of-three 使用
前者；reducer 消耗和 progress check 使用后者。

### 4.2 为什么不用 raw pointer

Raw `delimiter *` 会阻止 vector relocation，迫使实现选择以下较差方案之一：

- 按输入上界一次性过度分配；
- 每条 delimiter 单独 allocation；
- chunk pointer graph；
- realloc 后修复所有外部 pointer。

现有 raw delimiter struct 位于 core-private extension SPI：

- installed facade 只安装 `packages/markdown-core/include/markdown_core.h`；
- SwiftPM 的 `publicHeadersPath` 也是该 include 目录；
- shared facade export allowlists 不包含 delimiter/extension parser symbols。

因此 refactor 可以把 delimiter 变成 opaque handle，而不改变受支持的 C facade ABI 或任何
platform API。Bundled extensions 使用 internal handle/accessor API。不得为保留无支持承诺的
raw prefix 而牺牲 arena 数据模型。

### 4.3 Handle 生命周期

- Push 返回的 handle 在其 arena suffix truncate 前有效。
- Reducer 可以在调用期间解析 handle；phase two 禁止 push，因此 reducer 内部不会遇到
  arena relocation。
- Reducer、finalizer、AST node 和 extension opaque payload 不得保存 handle。
- Truncate 后的 index 可以被后续 source scan 复用；旧 handle 立即失效。
- Debug/test build 应对 stale、inactive、wrong-owner 和 below-mark handle 做 invariant
  assertion。

## 5. Push 与索引不变量

Push 是 delimiter identity 的唯一建立点：

1. handler 已经持有 parser-local rule binding；
2. arena 追加 record；
3. record 绑定 rule slot 和 owner；
4. record 链接到 global tail；
5. record 链接到该 rule 的 live tail；
6. 若 rule 参与 shared-close scope，更新该 rule 的 unmatched semantic opener state；
7. 成功后才发布新的 tails。

Arena growth 或 record 初始化失败时：

- 原 vector、global chain、per-rule chain 和 scope state 不变；
- staged delimiter literal node 由调用者释放，cursor 与 AST 都不提交；
- finish 不得返回该 lossy tree。

每个 active record 必须同时满足：

- global prev/next 双向一致；
- per-rule prev/next 双向一致；
- 相邻 per-rule records 的 rule slot 相同；
- owner 与 rule binding 从 push 到 truncate 不变；
- source/order 单调；
- record 位于当前 arena count 内且高于所属 process boundary。

## 6. Mark、process 与 truncate

### 6.1 Boundary snapshot

Core bracket record 在 opener 被扫描时保存 delimiter mark。该 mark 是“进入 bracket 前的
delimiter stack”，等价于 CommonMark algorithm 的 `stack_bottom` pointer，但不依赖可搬迁
地址或 source position。

Nested bracket marks 严格 LIFO。Inner process/truncate 不得修改 outer mark 以下的 record
或 state。

### 6.2 Process 范围

Delimiter processing：

- 从 mark 的 global tail 之后第一个 active record 开始；
- 按 global source order 选择 closer；
- opener search 只走 closer 所属 rule 的 per-rule previous chain；
- reducer 只能影响 mark 之后的 delimiter records 和对应 inline subtree；
- process 完成后，mark 之后不得留下 live delimiter。

Source position 仍用于 AST scope 和 body slicing，但不再定义 stack ownership boundary。

### 6.3 Truncate

Process 无论成功、literal fallback、OOM 还是 internal error，最后都执行 infallible
`truncate(mark)`：

1. 从 arena tail 逆序访问 mark 之后的 physical records；
2. 对每条出现的 rule，借助 immutable `push_previous_rule` 与 `scope_before` 恢复到最早
   suffix record push 之前的 tail/scope state；
3. 恢复 mark 保存的 global tail，并将其 `next` 清零；
4. 将 arena count 回退到 mark count；
5. 保留 capacity 供后续 source scan 复用。

Reverse restore 正确性的原因是：同一 rule 的 records 按 push order 出现在 arena 中；逆序
赋值最后落在该 rule 最早 suffix record 的 pre-push state，恰好是 boundary state。

Mark 以下的 records 从未被 reducer 修改，因此不需要重建或重扫 prefix。

Inner truncate 可以复用 outer mark 以上的 indices，因为：

- outer mark 只引用 mark 以下的 stable prefix；
- inner callback 不得保存 suffix handle；
- 后续 push 使用已经恢复的 per-rule state；
- outer truncate 最终只处理当前有效 suffix。

Shared-close claim order 不随 truncate 回退。它不是 opener topology，而是一次 inline parse
内的单调 source event id；保留 watermark 才能保证复用 arena index 后仍不会产生相等或倒退
的 claim。

这个 truncate 是 CommonMark lifecycle 的直接表达：每次 `process_emphasis` 本来就必须丢弃
`stack_bottom` 以上的全部 delimiter。它不是针对 link-heavy benchmark 的特殊回收路径。

## 7. Per-rule opener search

### 7.1 Search classes

每条 rule 声明有限的 closer search class：

- exact/nearest rule：1 个 class；
- CommonMark rule-of-three：3 个 class，由 immutable
  `original_closer_length % 3` 决定。

每次 delimiter process 为 `(rule slot, class)` 维护一个 exhaustion floor。Floor 是
delimiter order/ordinal，不是 record pointer。尚未在本次 process 访问的 floor 逻辑上等于
mark；实现可使用 generation tag 延迟初始化，避免每个 link boundary 清空完整矩阵。

不得用 source byte 索引 floor。Rule slot 才是 identity；因此不存在 128/256 byte
array boundary，也不存在两个 owner 因相同 byte 共享 exhaustion state。

### 7.2 Search

对于一个 closer：

1. 从 `closer.rule_previous` 开始；
2. 跳过不能 open 的同-rule records；
3. 到达 mark 或该 `(rule,class)` floor 时停止；
4. 对 candidate 应用该 rule 的纯 pairing predicate；
5. 第一个 eligible candidate 是唯一 opener。

失败后，将该 `(rule,class)` floor 前移到当前 closer 的 order。之后同 rule、同 class 的
closer 不得再次访问 floor 之前的 candidates。

成功后，reducer 处理 nearest eligible pair。它不得改为搜索一个“更容易物化”的较早
opener。

### 7.3 CommonMark rule-of-three

`*` 和 `_` 必须保留 CommonMark predicate：

```text
not (closer.can_open or opener.can_close)
or closer.original_run_length % 3 == 0
or (opener.original_run_length + closer.original_run_length) % 3 != 0
```

关键不变量：

- 使用 delimiter run 的 immutable original length；
- endpoint literal 被 strong/emphasis reduction 消耗后，original length 不变；
- floor class 也使用 original closer length；
- `*` 与 `_` 是不同 rule，不共享 chain 或 floor；
- strong/emphasis 每次消耗 2 或 1 个 remaining characters；
- 返回同一 residual closer 时，remaining length 必须严格下降。

只有语义确实采用 rule-of-three 的 grammar rule 才声明该 search policy。CrossLink、Embed、
directive label 和 formula 不因为共享 engine 而被隐式套用 CommonMark modulo policy。

## 8. Shared close trigger arbitration

### 8.1 语义

当多个 syntax 使用同一 close trigger 时，owner 是：

> 在当前位置能够词法消费该 trigger 的 handlers 中，拥有最新 unmatched semantic opener
> 的 handler。

这是 source nesting 语义，不是 extension priority 或 attachment order。

Core Link/Image bracket 与 extension delimiter scope 使用同一 monotonic `opener_order`。
每个 shared-close handler 声明：

- claim rule；
- close trigger；
- 一个无 allocation、无状态修改的纯 lexical probe，返回精确 close marker byte 长度。

### 8.2 `]` 的例子

`dispatch[']']` 包含 Link/Image、directive label、CrossLink 与 Embed candidates：

- CrossLink/Embed probe 只有在当前位置为 `]]` 时为真；
- directive label probe 返回 `]` 以及完整合法 attributes wrapper 的总长度；
- Link/Image 在 bracket 存在时能消费 `]`，即使最终只产生 literal fallback。

Dispatcher 一次扫描 bucket：

1. 对 probe 为真的 handler 读取其最新 unmatched opener；
2. 比较统一 opener order；
3. 选择唯一的最新 claimant；相等 order 是 invariant failure；
4. extension 胜出时由 core 一次性 stage Text、push closer、提交 source cursor；bracket
   胜出时进入既有 core close-bracket transaction。

如果最新 CrossLink opener 面对单个 `]`，其 probe 为假；opener 保持 unmatched，较旧的
directive/link candidate 可以接管当前位置。这不是 registration-order fallback。

Probe 不获得 parser、AST 或 mutable extension state，也不会被当作 matcher 再调用。
胜出者一旦确定就没有“未处理”、retry 或 fallback 路径；allocation 失败设置 sticky OOM，
contract 破坏设置 internal error，二者都不会尝试另一语法掩盖失败。除 bracket 这个
`]` 的 built-in pseudo-claimant 外，close trigger 本身完全由 rule descriptor 声明。

### 8.3 Phase-one scope state

只有声明 shared-close claim 的 rule 才维护 phase-one unmatched semantic opener。该状态服务
trigger ownership，不替代 phase-two CommonMark pairing。

当前 CrossLink、Embed 和 directive label 都有明确的 opener-only/closer-only roles。若未来
shared-close rule 允许同一 delimiter 同时 open/close，descriptor 必须提供纯、与最终语义一致
的 phase-one pairing policy；不得沿用“看到 `can_close` 就盲目 pop”的缓存算法。

## 9. Reduction 与 finalization

### 9.1 Reducer 边界

Engine 选择 opener/closer；owner reducer 不重新搜索 owner 或 pair。Reducer 负责 AST
transformation，并返回 `OK`、`OOM` 或 `INVALID`。Engine 根据 immutable rule descriptor
中的 `RANGE`、`ENDPOINTS` 或 `RUN` policy 更新 delimiter topology。

Delimiter record 的 unlink/consume 生命周期由 engine 持有。Reducer 只接收 immutable
match snapshot，不直接 free、保存或拼接 raw delimiter objects，也不选择下一 closer。
Snapshot 包含 rule-local kind、两个 marker nodes、精确 source bounds、original/remaining
run lengths，以及 `RUN` reduction 本次使用的 1 或 2 bytes。

Reducer 的 AST mutation 也有明确的 transaction boundary：全部可能失败的 allocation 与
semantic validation 必须发生在第一次 tree mutation 之前；一旦修改 AST，callback 必须返回
`OK`。Engine 负责回滚 delimiter topology，但不会复制整棵 AST 来掩盖违反此契约的 reducer。

### 9.2 Progress

Reducer 返回 `OK` 后，engine 的三种 topology policy 都内建 progress：

- `RANGE` 删除完整 matched range；
- `ENDPOINTS` 删除两个 endpoint；
- `RUN` 消耗 1 或 2 个 marker bytes，空 run 被删除，非空 run 的 remaining length
  严格下降。

Reducer 返回 `OOM` 或 `INVALID` 时 process 立即停止，并仍然无 allocation 地 truncate 到
mark。未知 result value 被归类为 `INVALID`，不是普通 parse fallback。

### 9.3 Literal-but-consumed

结构 pair 与最终 AST materialization 是两个不同问题。例如：

- 空或跨行 CrossLink/Embed；
- 不满足 wrapper 约束的 formula；
- opener/closer literal length 不相容的 strikethrough。

若现有 grammar 规定最近结构 pair 被消费但 AST 保持 literal，reducer 必须返回
`OK` 且不物化新 AST；descriptor 的 topology policy 仍消费该结构 pair。它不能把“无法物化”
改成“继续搜索更早 opener”，否则会改变后续 closer ownership 与 canonical AST。

### 9.4 Surviving-node finalization

Opaque syntax 的 reducer 应优先保存 owner inline source 中的 borrowed span；只有最终在 AST
存活的 node 才在 inline-owner local finalization/postprocess 阶段 materialize owned payload。

因此：

- nested candidate 不为随后被 outer opaque node 吞掉的 payload 重复 allocation/copy；
- finalizer 只遍历当前 inline ownership domain 的 surviving nodes；
- 每个 surviving payload 最多 materialize 一次；
- borrowed source 在 finalization 完成前由 inline owner backing buffer 保活；
- finalizer allocation 失败设置 parser sticky OOM。

Finalizer 不参与 delimiter pairing，不得访问 delimiter handle，也不得改变 registration
precedence。

## 10. OOM 与 invariant failure

### 10.1 可失败操作

以下操作可以报告 OOM：

- attachment/grammar-plan transaction；
- delimiter arena initial allocation 或 growth；
- delimiter literal/semantic AST node allocation；
- reducer 的 AST transformation；
- surviving payload finalization。

### 10.2 失败契约

- OOM flag 是 sticky；
- 失败操作不得部分发布新的 chain tail、rule binding 或 dispatch plan；
- parser finish 返回失败，不得返回 silently truncated/lossy AST；
- delimiter truncate、state restore 和 arena disposal 必须无 allocation、不可失败；
- OOM cleanup 不调用 alternate grammar handler；
- custom allocator injection 必须覆盖每个 allocation boundary。

### 10.3 Internal error

以下属于 internal error，而不是 OOM 或 ordinary literal fallback：

- stale/wrong-owner delimiter handle；
- reducer 删除 mark 以下 record；
- reducer 在 phase two push delimiter；
- reducer 返回未知 result；
- global/per-rule chain 不一致；
- truncate 后 tail/scope state 未回到 mark；
- shared-close probe 返回越界长度或两个 claimant 拥有相同 order；
- source consume 非前进、越界或 matcher 静默移动 cursor；
- descriptor/rule contract 绕过 attachment validation。

Release build 必须以现有 parser internal-error channel 失败；debug/test build 同时 assertion。

## 11. 摊还复杂度

设：

- `B` 为 inline source bytes；
- `D` 为实际 push 的 delimiter records；
- `D_r` 为 rule `r` 的 delimiter records；
- `C_r` 为 rule `r` 的有限 search class 数；
- `H_c` 为 source trigger `c` 的 attached handlers；
- `S` 为 final AST 中 surviving opaque payload bytes。

### Arena

Geometric growth 的总搬迁/初始化成本为 amortized `O(D)`。每次 process 后按 semantic mark
truncate 并复用 capacity；空间为 `O(max live delimiter suffix)`。

### Dispatch

普通文本 run 每个 source byte 至多被 special-character scan 访问一次。Trigger dispatch
成本为实际 bucket 工作 `O(H_c)`；shared-close arbitration 扫描 bucket，而不扫描 delimiter
depth 或全部 extensions。

### Opener search

失败搜索后 floor 前移，所以同一 `(rule,class)` 的 candidate 不会重复访问。成功搜索跨过的
records 随 matched range 被 unlink；residual endpoint 每次至少消费一个 source character。

因此 candidate visits 为：

```text
O(sum over r of C_r * D_r + B)
```

Bundled grammar 中 `C_r` 是语义常数（exact 为 1，CommonMark 为 3），所以为 `O(B + D)`。
无关 rule 不进入同一 search chain。

### Reduction 与 finalization

每个 delimiter record 至多 push 一次，并在所在 suffix truncate 前 unlink/consume；每个
endpoint character 至多被消费一次。Finalizer 只 materialize surviving payload，所以总 copy
为 `O(S)`，不是所有 nested candidate spans 的总和。

### Truncate

一次 truncate 访问被回收 suffix 的 physical records。Record 被回退后可复用；在一段 source
scan 中，每次 push 对应至多一次 reverse restore，因此全部 truncate work 为 amortized
`O(D)`。

### 总界

在固定 grammar plan 下：

```text
time  = O(B + D + S + actual output work)
space = O(max live delimiter suffix + rule bindings + output)
```

不得增加 `count == 1`、small input、single extension 或 benchmark-observed shape 分支来获得
局部数字。Exact 与 rule-of-three 的不同 search class 是文档化 grammar semantics，不是
cardinality special case。

## 12. 必须保持的可观察语义

- Closer 按 source order 处理。
- 同 rule 内选择 nearest eligible opener。
- CommonMark rule-of-three 与 original-run-length 语义不变。
- Strong/emphasis residual delimiter 可继续参与当前 closer processing。
- Link/image active/deactivation、nested-link 禁止规则和 footnote behavior 不变。
- Process 不能越过 bracket/footnote mark；结束后 mark 以上无 live delimiter。
- Shared close trigger 由最新、当前位置可词法闭合的 semantic opener 所有。
- Handler attach permutation 不改变 AST。
- Invalid opaque pair 保留既有 literal-vs-consumed semantics。
- CrossLink/Embed body 保持 opaque、source-faithful，并只对 surviving node materialize。
- Source scope 与 body slice 使用明确的 start/end，不再依赖含糊的 `position` 字段。
- Parser-local plan 支持不同 parser 同时启用不同 extension sets。
- OOM 不返回部分结果；internal invariant failure 不伪装成 OOM。

## 13. 验证契约

### 13.1 Correctness

至少覆盖：

- 完整 CommonMark emphasis/strong spec corpus；
- `*`/`_` original length、rule-of-three 三个 modulo classes 和 residual runs；
- smart quotes 与 emphasis adjacency；
- link/image/footnote delimiter boundary；
- Link/Image、directive label、CrossLink、Embed 深度交错；
- `[[` 面对单 `]` 时 decline、较旧 claimant 接管、CrossLink opener 保持；
- 所有 delimiter extension attachment permutations/反序产生相同 canonical AST；
- duplicate attachment 与 attachment OOM 原子性；
- literal-but-consumed nearest-pair cases；
- nested opaque syntax只 materialize final survivors；
- parser renew 后 grammar plan 与 parser-owned transient arena capacity 正确重置/保留；
- allocator injection 覆盖 arena growth、reducer 与 finalizer。

### 13.2 Deterministic complexity counters

Test build 暴露内部只读 counters：

- arena pushes、peak live records 与 geometric growths；
- process calls 与 per-rule opener candidate visits；
- reducer calls；
- delimiter unlinks 与 run bytes consumed；
- truncate visits 与 reclaimed records。

Adversarial tests 必须断言一般工作上界，而不只比较 wall clock：

- balanced nearest-pair ranges；
- CommonMark rule-of-three 的 modulo 0/1/2 classes 与 residual runs；
- unrelated rules 交错，确保 opener search 不访问其他 rule；
- nested mark/truncate 与 arena index reuse；
- geometric growth、growth OOM、跨 unit lane growth/reuse、invalid push 和 reducer failure
  的事务边界。

独立 engine runner 当前固定 12 个 deterministic cases：

```text
balanced_nearest_ranges
commonmark_modulo_one_floor
commonmark_modulo_two_floor
commonmark_modulo_zero_pairs
per_rule_isolation
mark_restore_and_reuse
residual_run_progress
geometric_arena_growth
arena_growth_oom_transaction
unit_lane_growth_and_reuse
reducer_failure_is_terminal
invalid_push_is_transactional
```

此外，extension-order runner 枚举五个 delimiter extensions 的全部 `5! = 120`
attachment permutations；complexity runner 使用 balanced dollar/backslash Formula
adversarial inputs 验证 4 KiB 到 64 KiB 的 normalized scaling。

Wall-clock complexity runner 保留为二级 regression gate，继续使用 normalized endpoint
scaling；它不能替代 deterministic operation invariant。

## 14. 实施映射

- `core/delimiter.c` 与 `core/delimiter.h`：compiled attachment buckets、dense bindings、
  relocatable arena、per-rule lanes、marks、typed processing 与 diagnostics。
- `core/inlines.c`：fixed core routing、统一 shared-close arbitration、transactional source
  consumption，以及 phase-two 调度。inline unit 只 begin/reset parser-owned delimiter
  scratch；lane/record capacity 跨 unit 和 parser renew 保留，拓扑、claim clock 与 marks
  每个 unit 独立。
- `core/markdown-core-extension-api.h`：immutable rule descriptors、pure close probes、
  typed reducers、source span/consume APIs。
- `extensions/`：Strikethrough、Formula、Directive、CrossLink 与 Embed 全部使用同一
  delimiter engine；Autolink 使用同一 transactional source cursor 与显式 seam metadata。
- `tests/runners/delimiter_engine_runner.c`：数据结构、事务性与 deterministic work invariants。
- `tests/runners/extension_order_runner.c`：全部 bundled delimiter-extension attach permutations。

旧 pointer stack、sentinel delimiter kinds、phase-two owner lookup、raw cursor setter、
extension-owned close consumption 与 temporary special-character mutation 均已删除。仓库只保留
一个 active delimiter algorithm。
