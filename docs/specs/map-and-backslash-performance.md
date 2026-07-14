# Map 与连续反斜杠性能设计

状态：待 review  
日期：2026-07-14  
范围：C parser core、directive attribute normalization、complexity gate

## 结论

本次性能修复包含两个相互独立、但由同一组端到端测试暴露的问题：

1. 将继承自 cmark-gfm 的 reference/footnote map 从首次查询时全量
   `qsort`、随后 `bsearch`，迁移到共享的 byte-key open-addressing hash index；directive
   attribute 去重复用同一 index。
2. 当没有 extension 接管 `\\` 时，将连续的 escaped-backslash pairs 一次解码成一个 Text
   node，避免先创建数千万个临时节点、再在 parser finish 阶段合并。

两项修改都不改变可观察语义。map 保留 reference/footnote 的 first-definition-wins、directive
的 first-position/last-value-wins；backslash 修改保留 CommonMark escape 结果、source scope 和
extension dispatch 优先级。

## 背景与边界

### Map 的来源和使用者

`markdown_core_map` 随 2026-07-11 baseline 从 cmark-gfm 继承，并非为了 directive 或
formula 新建。改造前后的直接使用者都是：

- reference definitions；
- footnote definitions。

formula 不使用该 map。directive 也不采用 reference label normalization 或 expansion budget
等 `markdown_core_map` 语义；它只复用本次从 map 中抽出的通用 `markdown_core_key_index`。

因此这里有意分成两层：

- `markdown_core_key_index`：不拥有 key/value 的 byte-key 索引原语；
- `markdown_core_map`：在索引之上保留 reference/footnote 的 label normalization、重复定义
  规则与 expansion accounting。

这不是公共 C API。类型和函数声明位于 core-private `map.h`，不进入安装头文件或 platform
binding。

### 非目标

- 不提供通用容器库或任意类型 dictionary。
- 不更改 reference label 的 Unicode case fold、首尾空白删除和内部空白折叠。
- 不用 hash iteration order 定义输出顺序。
- 不恢复 directive 的 HTML-style `#id`、`.class` 或 id/class 特殊合并语义。
- 不更改 extension 对反斜杠特殊字符的优先处理权。

## 1. 通用 byte-key index

### 改造前

继承实现将 definitions 保存在单向链表中。第一次 lookup 时：

1. 为全部 entry 分配 pointer array；
2. 按 normalized label 和 source age 执行 `qsort`；
3. 原地压缩重复 label；
4. 每次 lookup 使用 `bsearch`。

这使准备阶段为 O(n log n)。同样地，早期 directive attribute 去重会为全部属性建立 pointer
array 并排序。在 4 KiB → 128 MiB 的 duplicate-attribute endpoint test 中，旧路径的归一化
slowdown 实测为 4.442。

### 数据结构

`markdown_core_key_index` 是 power-of-two capacity 的开放寻址表：

```c
typedef struct markdown_core_key_index_slot {
    uint64_t hash;
    const unsigned char *key;
    bufsize_t key_len;
    void *value;
} markdown_core_key_index_slot;
```

关键约束：

- 最小 capacity 为 16；
- capacity 始终为 2 的幂，因此 bucket 选择和环绕可用 bit mask；
- load factor 不超过 0.5；
- linear probing 每次操作最多检查 64 个 slot；
- hash 为逐字节 64-bit FNV-1a，再执行 avalanche mixing；零 hash 被映射为 1；
- key/value 均为 borrowed pointer，由调用方保证在 index 生命周期内有效；
- key equality 同时比较 hash、长度和 bytes，不以 hash 相等代替内容相等。

目标不是提供针对 hostile input 的密码学 hash，而是让正常 lookup/insertion 为 expected O(1)，
同时对不利 collision 设置明确的工作上界。

### 扩容与失败原子性

插入使 load factor 超过 0.5 时，capacity 翻倍。扩容先分配新表并完整 rehash；只有全部 entry
成功迁移后才替换旧表。因此 allocation、overflow 或 probe-limit 失败不会留下部分迁移的
index。

index API 以返回 0 报告初始化、插入或扩容失败。它不尝试吞掉错误后继续使用不完整表。

### Collision 与降级路径

64-probe limit 避免构造 collision 将 linear probing 直接退化成无界 O(n²)。如果 map 建索引
时任一 entry 超出该限制，整个 index 被释放，map 回退到继承的 pointer `qsort`/`bsearch`
路径。因此：

- 正常输入：expected O(n) prepare，expected O(1) lookup；
- probe/allocation fallback：O(n log n) prepare，O(log n) lookup；
- 不存在“前半使用 hash、后半使用不完整 hash”的混合状态。

保留 `qsort` 是有意的故障隔离，不是普通输入的数据路径。`blocks.c` 中另一个按 footnote
source index 排序的 `qsort` 只决定最终输出顺序，与 map lookup/duplicate normalization 无关。

### Reference 与 footnote 重复语义

definitions 链表为 newest-first，但 Markdown 语义要求同名 reference 的第一个 source
definition 生效。建索引时从 newest 遍历到 oldest，并允许同 key replace；最后留在 slot
中的正是 oldest，即 source 中最先出现的 definition。

map 仍在每次 lookup 后执行原有 expansion limit accounting；hash index 不绕过
`max_ref_size` / `ref_size` 防护。

footnote 的 lookup 使用相同规则；需要按 source index 输出时显式收集并排序，不能依赖 hash
slot order。

## 2. Directive attribute 去重

directive attributes 是普通 key/value metadata。`id` 和 `class` 与其他 key 相同；HTML-style
`#id` 和 `.class` shortcut 被拒绝。

重复 key 的契约为：

- 保留第一次出现的位置；
- 使用最后一次出现的 value；
- 最终 JSON 只包含一个 key。

实现分两遍：第一遍将每个 key 的 first attribute 放入 index；第二遍再次按 source order
扫描，将后续 value 交换到 first attribute，并将重复节点标记为 inactive。这既不依赖 hash
iteration order，也不需要为输出重新排序。

如果 index 初始化或插入失败，directive 回退到 pointer sort。fallback 仍显式把最后一个
value 移到第一个 attribute，并关闭其余重复项，因此两条路径具有相同语义。

### 初始容量采样

unique-heavy 输入希望按总 attribute count 预分配，duplicate-heavy 输入则不应按数百万个
source occurrences 浪费空间。count 大于 1024 时先采样最多 1024 个 key：

- 样本 unique ratio 大于 0.5：按总 count 初始化；
- 否则：按样本 unique count 初始化，后续按需渐进扩容。

采样只影响初始 capacity，不影响最终 key 集合或重复语义。采样 index 自身失败时按总 count
初始化，保证它只是优化提示而不是 correctness 前提。

## 3. 连续反斜杠批处理

### 触发路径

问题由 malformed/unclosed directive 输入暴露，例如：

```markdown
:x{key="\\\\\\\\\\\\...
```

directive scanner 因缺少 closing quote/brace 不接管该片段，输入按普通 CommonMark inline
继续解析。对于连续 `\\`，原 `handle_backslash` 每次把一对 source bytes 解码为一个 literal
backslash，并创建一个 Text node。parser finish 最终会合并相邻 Text node，所以输出正确，
但大输入会在合并前持有与 pair 数量同阶的临时节点、allocation 和 linked-list 操作。

128 MiB case 因而曾在远端得到 9.850 的 normalized slowdown，本机约耗时 2.49 秒。

### 新路径

当且仅当以下条件全部满足时启用批处理：

1. 当前字符为 `\\`；
2. 下一个字符也是可 escape 的 `\\`；
3. 没有 extension 注册并接管 `\\` special char；
4. 连续 run 至少包含两对 backslash。

实现一次扫描完整的 pair run，分配最终长度的一块 buffer，填入 `run_bytes / 2` 个 literal
backslash，并创建一个 Text node。单 pair、奇数结尾、line ending、非 punctuation 和 allocation
失败仍走原有逐项路径。

### 语义等价性

对长度为 `2k` 的连续 backslash run，旧路径产生 k 个相邻 Text node，每个 literal 为一个
backslash；parser finish 将它们合并为长度 k 的 Text node。新路径直接产生同一个最终节点。

保持不变的边界包括：

- decoded literal bytes 相同；
- node scope 仍覆盖整个 consumed source run；
- 偶数 pair run 后的下一个字符仍由下一次 inline dispatch 处理；
- 奇数 run 的最后一个 backslash 仍按其后字符决定 escape、hard break 或 literal；
- extension hook 先于 core backslash handler，且注册 `\\` extension 时批处理明确禁用；
- formula 的 delimiter/escape 规则未被本修改重新定义。

这项优化位于通用 inline core，是因为性能成本发生在 directive 回退后的普通 Markdown
解析中；把它放进 directive scanner 会漏掉其他产生相同 backslash run 的输入。

## 4. 复杂度验证

complexity runner 对每个 case 测量 4 KiB 与 128 MiB 两个 endpoint。输入跨度为 32768，判定
值为：

```text
normalized slowdown = large_time / small_time / 32768
```

当前覆盖八类输入：

| 类别 | Case |
| --- | --- |
| directive scanner | valid long quoted value |
| directive/backslash | valid consecutive backslashes |
| scanner fallback | unclosed long quoted value |
| core backslash fallback | unclosed backslash value |
| directive map | many unique attributes |
| directive map | many duplicate attributes |
| inherited map | many unique references |
| inherited map | many duplicate references |

最新本机 normalized slowdown 为 0.663–1.164。128 MiB unclosed-backslash 从约 2.49 秒降至
0.189 秒，修复后为 0.979×。

wall-clock threshold 为 4.0，而不是把 2.0 当作 n log n 的数学判别线。原因是 128 MiB
解析会创建数百万个对象并跨越 4 KiB 样本没有覆盖的 allocator/cache regime；远端 macOS
上 expected-linear unique-attribute hash path 曾测得 2.753–3.318。4.0 仍低于已测旧 sort
路径的 4.442，并能拒绝旧 backslash 路径的 9.850。

timing gate 负责捕获真实端到端退化，但不单独证明算法复杂度。结构性保证来自：

- 0.5 load factor；
- 64-probe hard limit；
- transactional growth；
- 50,000-entry constructed reference-collision pathological test；
- unique/duplicate map endpoint tests；
- CommonMark correctness、directive fixtures 和 sanitizer suites。

## 5. Review checklist

Reviewer 应重点确认：

- `markdown_core_key_index` 的 key/value lifetime 是否始终长于 index；
- capacity arithmetic、翻倍和 allocation size 是否完整检查 overflow；
- growth 失败是否保持旧 index 不变；
- 64 probes 失败后是否完整回退，且 fallback 与 hash path 重复语义一致；
- reference 的 first-definition-wins 是否因 newest-first 链表遍历方向得到保留；
- directive 是否确实保持 first-position/last-value-wins；
- 任意输出是否错误依赖 hash slot order；
- backslash fast path 是否只在没有 extension owner 时进入；
- odd/even runs、单 pair、allocation failure 与 source scope 是否保持旧行为；
- performance gate 是否继续同时覆盖正常、重复、malformed 和 collision 输入。

## 6. 相关实现与测试

- `packages/markdown-core/core/map.c` / `map.h`：共享 index 与 inherited map adapter；
- `packages/markdown-core/core/references.c`：reference entry ownership；
- `packages/markdown-core/core/footnotes.c`、`blocks.c`：footnote lookup 与显式输出排序；
- `packages/markdown-core/extensions/directive.c`：attribute normalization 与容量采样；
- `packages/markdown-core/core/inlines.c`：连续 backslash pair fast path；
- `packages/markdown-core/tests/runners/complexity_runner.c`：4 KiB → 128 MiB endpoint gate；
- `packages/markdown-core/tests/runners/pathological_runner.c`：constructed collision correctness test。

