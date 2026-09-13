# #244、#247、#273 修复与验证

对照基线为 PR #275 的首个提交 `84efce32`；本轮只比较新增修复。
原始数据在 [results.json](../../experiments/inline-performance/results.json)，
属性识别差分记录在 [attributes-differential.json](../../experiments/inline-performance/attributes-differential.json)。

## 实现

- **#244**：inline state 持有只前进的 source-mark 游标，回查 opener 时使用二分查找。
  每个端点只定位一次，Text 直接复用定位出的 mark 区间。未改变的字节由统一的
  source Text 构造器保留指针身份；实体和 escape 仍保留解码后的 authored extent。
- **#247**：分发表同时保留每个 owner 的 dispatch / terminator 角色，按原有优先级
  仲裁 `can_start`。不同谓词按 OR 接受，缺省谓词始终接受。删除线和下标共用 run
  分类，同时保留 exact-run 消耗规则、词内空格规则和 `~` 的 flanking transparency。
  构建索引只遍历实际声明的字节。
- **#273**：整段 reverse DP 改为按需的前向成员后缀、裸值续接和 value join facts。
  不再为每个源字节分配两个 offset；ASCII 不再反复 UTF-8 解码。Directive 的
  scan/apply 共用识别所有者；definition-list 的引用探测只识别、不解码值。
  Class/record 向量及共享 radix index 均从一个元素开始几何增长。

[属性识别架构](../architecture/attribute-recognition.md)给出了所有权、累计工作证明和
EOF 依赖。不同长度的物理行与组装段落不共享失败事实；新模型消除了对未查询段落的
重复整段索引。生产 append、版本化 source 和 scanner continuation 仍由增量计划 I52
推进，本轮没有新增 streaming API。

## 测量范围

macOS arm64 Release，同机对照完整 C parse + free，两个版本使用相同 ctypes 调用。
每个输入先预热，再交错采样七次；短样本按预热耗时校准批次数以降低时钟噪声。
计时不含 canonical dump 或分配跟踪。69 个输入均比较完整 canonical dump，包括
原有 26 个独立 benchmark 样本以及 CI 的 `representative_large v1`。

| 输入 | 原耗时 (ms) | 新耗时 (ms) | 加速比 |
| --- | ---: | ---: | ---: |
| Emphasis，2,000 行 | 4.615 | 3.344 | 1.38× |
| 普通文本，2,000 行 | 1.701 | 0.541 | 3.14× |
| 普通文本中的 `!`，2,000 行 | 5.964 | 0.607 | 9.83× |
| 普通文本中的 `~`，2,000 行 | 5.621 | 0.711 | 7.91× |
| 段末稀疏属性，256 KiB | 3.282 | 0.240 | 13.68× |
| 密集属性，约 256 KiB | 9.134 | 7.031 | 1.30× |
| CI `representative_large v1`，158,000 B | 4.232 | 4.059 | 1.04× |

普通文本的分配次数从 16,053 降至 4,050；段末稀疏属性的峰值从 2,894,570 B
降至 794,506 B，识别工作计数从 262,160 降至 13。密集属性的峰值从
15,753,880 B 降至 13,237,912 B，但分配次数从 107,972 增至 108,000。
最慢的相对变化是无属性 64 KiB 文本耗时增加约 2.2%；原有短样本中，
`inline-escape.md` 增加约 1.3%。综合样本在本机仅有约 4% 收益，不能用
专项输入的加速比推断它在 Linux CI 上的表现。

这些数据是该机器上的诊断证据，不是跨平台的最优性证明。某些短样本仍可能有数个百分点
的变化；属性密集场景的分配次数也未必下降，稀疏 facts 的收益主要来自更少的扫描和
更小的申请/常驻容量。JSON 保留每次计时、分配次数、累计申请字节、峰值、工作计数及
源文件和库的哈希，避免只报告最快的形状。

## 首批 map 改动与 CI 回退

[首批 PR benchmark](https://github.com/nouprax/markdown-core/pull/275#issuecomment-5653459611)
报告 `84efce32` 相对 `5eca3bc1` 从 7.550 ms 增至 10.585 ms，即 +40.2%。
这是整个首批提交的 parse-only 结果，不能单独归因于 map。两边使用相同 Ubuntu
image 和 Clang 18.1.3，但来自不同的 hosted-runner 作业：
[base](https://github.com/nouprax/markdown-core/actions/runs/34753250736/job/103713256047)
在 10:59 UTC、northcentralus，
[head](https://github.com/nouprax/markdown-core/actions/runs/34758835041/job/103727913549)
在 13:05 UTC、westus3。日志没有 CPU 型号或逐次耗时，无法拆分硬件、负载和代码影响。

为隔离代码因素，四个独立源码快照使用相同编译器构建，原样运行 CI 的
`bench_runner`。16 轮轮换进程顺序，每个进程 warmup 2 次、计时 9 次；下表为
进程中位数的中位数。此实验在 macOS arm64 上运行，完整 canonical dump 相同。

| 版本 | Parse (ms) | 相对 base |
| --- | ---: | ---: |
| 原始 base | 3.806 | — |
| 仅替换 `map.c` / `map.h` | 3.938 | +3.5% |
| 首批的其余修复，保留旧 map | 3.651 | -4.1% |
| 首批完整提交 | 3.795 | -0.3% |

Radix 的确定性工作界不意味着普通查找更快；本机隔离结果显示该综合输入有常数成本。
可能的来源包括依赖前一次加载的分支遍历、插入时的再次下降及记录布局，仍需 profile
区分。该结果既不能复现 Linux 的 +40.2%，也不能证明 Linux 没有回退。
后续 benchmark 必须在同一 runner 构建并交错运行 base/head，保留原始耗时和环境元数据；
不能用碰撞输入的收益抵消普通输入回退，也不能先把差异归为噪声。

[隔离脚本](../../experiments/baseline-performance/map-attribution.py)与
[原始数据](../../experiments/baseline-performance/map-attribution.json)可复现本机结论：

```sh
python3 experiments/baseline-performance/map-attribution.py --cc clang --output /tmp/map-attribution.json
```

## 正确性与边界

- 新增 native gates：source-order 前向定位与反向 opener 查询；来源切片与解码列宽；
  共享谓词 OR 与 dispatch 优先级；已判定为文本的符号和普通文本拥有相同分配次数。
- 属性 gates 同时约束扫描、累计申请和峰值；覆盖段首/段末稀疏成功与失败、重叠裸值、
  长共享成员后缀以及前向/反向/置换查询。重复查询不扫描、不分配。
- 旧 reverse DP 与新识别器比较 35,010 个输入、1,533,102 次查询，包含 Unicode、
  CR/LF/CRLF、转义、未闭合引号回退、重复查询和越界起点，全部相同。
- Release、ASan、UBSan 各 88 项 C 测试，包括 OOM 故障注入；Swift 46 项测试；
  8 项 C benchmark suites，全部通过。
  位置、containment、引用顺序、source lists、AST projection、parser boundaries、
  public surface 与 repository audits 一并验证。

## 复现

在仓库根目录执行；baseline 目录是独立的临时源码快照。

```sh
baseline_dir=$(mktemp -d)
git archive 84efce3220e6e88fee1e6d5954b88eca82960924 | tar -x -C "$baseline_dir"
cmake -S "$baseline_dir" -B "$baseline_dir/build/benchmark" -DCMAKE_BUILD_TYPE=Release -DMARKDOWN_CORE_BENCHMARKS=ON
cmake --build "$baseline_dir/build/benchmark" --target libmarkdown-core-public-static libmarkdown-core-elements --parallel 6
cmake --preset benchmark
cmake --build --preset benchmark --parallel 6
python3 experiments/inline-performance/run.py --baseline-source "$baseline_dir" --baseline-revision 84efce3220e6e88fee1e6d5954b88eca82960924 --output /tmp/inline-results.json
python3 experiments/inline-performance/attributes-differential.py --output /tmp/attributes-differential.json
```

计时脚本只消费既有解析器。旧语法实现仅在 differential 脚本的临时目录中编译，
不会成为产品中的第二条解析路径。Khash / stb_ds 的候选评估见
[key index 文档](../architecture/key-index.md#third-party-hash-table-candidates)；本轮没有 vendor 或宣称它们更快。
