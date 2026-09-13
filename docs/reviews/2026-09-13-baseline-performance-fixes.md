# Baseline 性能修复：第一批

完成 #243、#248、#272 的本地代码与验证，GitHub issues 尚未关闭。完整 32 项的推进状态见
[task list](../plans/2026-09-13-baseline-performance.md)。其余 29 项（含汇总 #271）仍待完成，
当前结果不代表 baseline 已达到最佳性能。

## 改动及约束

### #243：嵌套列表中的表格探测

普通文本也可以是 simple table 的表头，因此“只有 `|` / `+` / `-` 可以开始表格”不成立。
实际可收紧的语义是：只有完整 dash separator 可以抢在已识别的列表或 thematic break 之前打开表格。
普通表头仍在常规 block start 排除后进入原有完整 grammar。

共享 scanner 一旦在当前行的某个字节拒绝 separator，该字节以前的所有 suffix 都复用这次拒绝。
`- ` 的深层嵌套因此不会反复扫描剩余行、建立 ancestor chain 和探测下一行。
lookahead resume cache 同时保留 `first_nonspace` 及列坐标，避免重新扫描已知的缩进。
没有深度阈值，也没有关闭表格语法。

复杂度测试现在计入 ancestor chain 的计数、保存、恢复三遍访问；旧 metric 只计访问行和消耗的
prefix bytes，漏掉了本 issue 的主要成本。原 comment lookahead 测试的线性界随计数口径更新，
新增列表单行、列表续行、quote 续行三种形状，各自覆盖深度 16–8192，并检查保留的容器数与段落续行。
链本身仍按正常 begin/end 生命周期创建和恢复；本次消除的是每个嵌套列表 marker 的无效事务，
未额外引入一种 persistent chain 机制。

### #272：无界线性探测

以统一压缩 radix tree 替换所有 key index 的 hash/linear probing。详见
[key index 架构](../architecture/key-index.md)。每个字节包含 presence bit 与八个 value bits，
可区分前缀和 NUL extension；分支在字节或位上严格前进。查找最多测试 `9 * query_bytes + 1` 个分支，
再比较一个 leaf。扩容只搬移 dense records，不重新插入。

保留 borrowed key、pointer/counter value、entry/commit、重复定义 source-order winner、OOM sticky
和共享 resource 生存期。arm64 首次分配由 512 B 降为 384 B，无每次 parse 的随机 seed。

原 pathological case 生成的是 cmark 旧 hash 的碰撞；现已改成真正 `5eca3bc1` hash 的 bucket flood。
API tests 验证 binary keys、empty/prefix keys、三种插入顺序、duplicate/replace 和每条 branch 的
严格位序。实验另对 references、heading anchors、footnotes、specimens 全部回放并比较完整 AST dump。

### #248：普通文本的空白边界

一个普通 text slice 内没有新的 delimiter/token，所有 whitespace barrier 最终会合并到最后一个。
现在从尾部找到这个 boundary 即停止，ASCII 字节直接分类，只有非 ASCII scalar 需要 UTF-8 解码。
继续使用方言既有的 Zs + TAB/LF/CR/FF 集合；没有换成含 VT 的 C `isspace`。

测试覆盖所有现有 whitespace scalars 及 VT、NEL、ZWSP、Unicode line/paragraph separator、emoji
等非空白邻居，验证最后一个 boundary、单个 delimiter summary 和只访问尾部的确定性工作界。

## 同机对照

环境：macOS arm64、Apple clang 21、CMake Release `-O3 -DNDEBUG`。对照为预先保存的
`5eca3bc1598b0313a0ac6a9e69f078a309feb3e1` dylib。两边均测 public C parse + free，
一次 warmup、七个 raw samples、交替执行顺序。以下为中位数，单位 ms。

| 工作负载 | baseline | 修复后 | 加速比 |
| --- | ---: | ---: | ---: |
| 1024 层列表 + 缩进续行 | 9.460 | 0.346 | 27.4× |
| 2048 层列表 + 缩进续行 | 44.180 | 0.814 | 54.3× |
| 4096 层列表 + 缩进续行 | 218.748 | 1.918 | 114.1× |
| 2048 个碰撞 key，references | 4.354 | 2.318 | 1.88× |
| 2048 个碰撞 key，headings | 2.565 | 1.474 | 1.74× |
| 2048 个碰撞 key，footnotes | 7.305 | 4.323 | 1.69× |
| 2048 个碰撞 key，specimens | 5.723 | 3.775 | 1.52× |
| 2000 行散文单段 | 1.214 | 0.704 | 1.72× |
| 1 MiB 无空白文本 | 3.410 | 2.695 | 1.27× |
| Unicode 散文 | 0.802 | 0.716 | 1.12× |
| 2000 行强调单段 | 4.208 | 4.120 | 1.02× |

强调长段的改善有限，仍需要 #244 等后续工作。时间不能证明线性；复杂度结论由结构不变量和工作
计数支持。52 个测量 case 中，46 个非深层 case 的完整 canonical dump 与 baseline 一致；
深层 case 不生成本身具有二次输出大小的树形 dump，其语义由 C 测试覆盖。

[复现命令](../../experiments/baseline-performance/README.md) ·
[原始 samples / source SHA-256 / library SHA-256](../../experiments/baseline-performance/results.json)

## 验证

- C Release：88/88；ASan：88/88；UBSan（halt-on-error）：88/88，含 OOM injection。
- Swift：46 tests / 10 suites。
- 位置审计：15072 个节点，无新增差异。
- containment 审计：16206 个关系，原登记的 30 项保持不变。
- reference-order independence、AST projection、source lists、element/parser boundary audits 通过。
- Python 对照工具检查四个 index 消费者与 26 个原始 tracked samples 的完整 canonical 输出；
  原增量 PoC 的 baseline 结果保留，probe 已适配新 index。

本批没有修改 public API、binding value model、源码语法或 CI benchmark contract；
ES/Kotlin runtime 与 TSan 本次未重跑。后续节点存储、分发、位置映射、绑定解码和基准工程
继续按 task list 推进。
