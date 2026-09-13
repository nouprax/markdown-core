# Performance review completion: #243, #248, #258

本轮以前一提交 `4ca15dedd35d677ee0834eed97183f6f544742cc` 为对照，补全曾被过早标记完成的工作。
实现和本地验证完成后才恢复关闭引用；本文件不表示已合并，也不表示全部 baseline 优化完成。

- [x] #243：普通文本表头必须有紧邻的 dash separator。表格、定义列表、块标识符共享一次 next-line peek，
  不再为没有分隔行的普通段落建立表格工作区。缓存只存在于当前 block-start 仲裁，以实际父容器为 key，
  新行和新容器开始前失效。保留 pure-text header、grid、multiline、pipe、caption、容器前缀和 mapped cell 语义。
- [x] #248：delimiter 的 ASCII 邻居不调用 UTF-8 解码，每侧的 whitespace/punctuation 分类只计算一次。
  保留 Zs + TAB/LF/CR/FF，VT/NEL/Unicode line separator 仍不是空白。移除无调用者的旧扫描接口。
- [x] #258：正式构建默认不包含诊断字段、局部累计变量和更新操作。内部复杂度测试与 probe 链接
  同源码的独立诊断 archive；编译定义传递至其消费者，禁止与正式库的私有结构布局混用。
- [x] #250 的重复行探测和无 marker 时的引用定义识别也随共享数据流消除；未增加引用定义结果的跨阶段缓存。

## 验证

- Release、ASan、UBSan：各 94 项 CTest 全部通过，包含 strict OOM、确定性复杂度、所有表格方言和 public facade。
- delimiter 测试覆盖全部 ASCII 类别、Unicode 类别边界及其两两组合，实际调用注册的 delimiter owner。
- 普通列表、独立段落、blockquote、链接行各按 128–8192 倍增：段落数不变，表格工作区增长、separator
  扫描与 geometry 分配均为零；共享 lookahead 工作不超过 `12N + 16`。原深度 16–8192 的三种嵌套形状继续通过。
- 5,069 个固定与 seed `258243248` 的混合语法输入：前一提交、当前正式库、当前诊断库的完整 canonical dump 一致。
- 以实际 compile commands 预处理全部 59 个正式编译单元，均不包含诊断字段或计数操作。
- private adapter 的 8 项构建选择和失败边界测试通过；源列表、测试拓扑、CI policy 审计通过。

## 复现

```sh
cmake --preset default
cmake --build --preset default --parallel
ctest --test-dir build/cmake --output-on-failure
cmake --preset asan
cmake --build --preset asan --parallel
ctest --test-dir build/asan --output-on-failure
cmake --preset ubsan
cmake --build --preset ubsan --parallel
ctest --test-dir build/ubsan --output-on-failure
python3 -m unittest discover -s experiments/incremental -p 'test_*.py'
cmake --build build/cmake --target incremental_probe
```

性能比较使用[配对 benchmark pipeline](../architecture/pr-benchmark.md)，base 设为上述精确提交，
head 设为本轮最终提交。计时使用不含诊断计数的正式库；诊断构建用于复杂度和分配观察，其 parser
尺寸包含计数字段，不能把它的分配统计当成正式库的尺寸。
