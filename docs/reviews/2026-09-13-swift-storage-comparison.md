# Swift ownership 与 swift-markdown 对照

对照版本：swiftlang/swift-markdown `75e3df1d7b664ef3c96595de36c243b98599b3bc`
（2026-09-08 的 main），swift-cmark `7898f1b3e4befeecee56cb4a3bc8eebd2cb63219`。
本仓库实现为 `ccda8bd4`。实验在 macOS arm64，Swift 6.3.3，优化构建下执行。

## 定义与职责

swift-markdown 同样将公开节点包装与内部存储分开：

- [RawMarkupData](https://github.com/swiftlang/swift-markdown/blob/75e3df1d7b664ef3c96595de36c243b98599b3bc/Sources/Markdown/Base/RawMarkup.swift#L18)
  用 enum 表达各种节点的 payload；`RawMarkup` 用 `ManagedBuffer` 持有 header
  与尾部分配的子节点强引用。
- [MarkupChildren](https://github.com/swiftlang/swift-markdown/blob/75e3df1d7b664ef3c96595de36c243b98599b3bc/Sources/Markdown/Base/MarkupChildren.swift#L16)
  是公开惰性 `Sequence`。`makeMarkup` 也有完整的 kind → 公开类型 switch。
- [_MarkupData](https://github.com/swiftlang/swift-markdown/blob/75e3df1d7b664ef3c96595de36c243b98599b3bc/Sources/Markdown/Base/MarkupData.swift#L112)
  还记录 parent 和 occurrence identity，服务父节点导航及持久化编辑。
  根 identity 使用原子计数器；这是其编辑/身份模型的机制，不是迭代释放的必要条件。

因此，collection view、payload enum 与公开类型包装的组合有合理用途。
本仓库 `Fields` 保存实际字段；公开类型提供字段访问，它们没有同时保存两份
独立、需要同步的字段值。内部整数索引的 kind 必须验证，公开字段仍有静态类型。
前一次回复将这两类结构本身直接判为过度设计、将内部检查笼统归为公共类型保证
变弱，说得过于绝对。

`MarkupCollection` 使取得子关系、count 和下标访问保持 O(1)，不为每次读取
分配子数组。`StoredMarkup` 让平坦数组能够存储异构 payload，同时禁止容器 view
重新进入 record 并形成递归拥有关系。其名称与逐类型访问代码可以继续改善；
这些可读性问题本身不足以推翻所有权模型。

集合本身直接保存在所属节点的 `Fields` 中：普通关系为 `[Int]`，
`Definition.content` 为 `[[Int]]`。后者通过 `MarkupGroups` 按需返回内层
`MarkupCollection`，取得视图及下标访问均为 O(1)，没有逐组物化。已删除
原先把 body 集合作为独立 record 的设计；分组不再进入存储枚举或构建队列。

## 字段查询与节点定位

`MarkupStore` 统一持有 records，负责类型化字段查询、节点投影和集合构造。
29 个容器或 scoped value 都通过 `@Stored var fields: Fields` 声明所需字段；
节点类型不再直接访问 records、匹配存储 enum 或重复编写错误检查。
`Stored` 是普通的泛型 property wrapper，保存 store 和私有 index，
没有 payload 副本、缓存、额外堆对象或宏依赖。`$fields` 提供关系查询所需的 store。

index 的职责是定位一个 store 内的具体 occurrence。同一个 store 可以包含
多个 Paragraph，不同 store 也可以在相同位置保存不同节点，因此类型本身
不足以查询出正确字段。定位信息集中在引用实现中，业务节点不依赖数组下标。

对比实验以 `72f14045` 的分散访问器为基线，在 arm64、Apple Swift 6.3.3 下
使用相同优化 C 对象及 Swift `-O -whole-module-optimization` 构建：

| 诊断项 | 原访问器 | 集中查询 |
| --- | ---: | ---: |
| TableCaption / Paragraph / Document stride | 各 16 bytes | 各 16 bytes |
| 4,000 个富文本段落，单次读取 | 2.917 ms | 2.848 ms |
| 1,000 组 callout/directive/table，单次读取 | 0.340 ms | 0.338 ms |
| 同一富文本输入，parse + release | 9.521 ms | 9.445 ms |

读取实验访问顶层 block 的 scope 及 paragraph 的直接 inline 子节点，未遍历
所有后代。两种程序顺序运行，每项为五个样本的中位数；这些差异不能作为稳定
加速比，但未观察到集中查询引入读取退化。优化 LLVM IR 中，Paragraph 和
TableCaption 的 scope getter 均没有动态 cast 或堆分配；节点投影中的分配
调用点也与基线相同。类型化查询被特化为对应的 case 检查，没有擦除 Fields
再装箱的中间步骤。

此前将完整 Fields 直接存进节点的实验仍被排除：TableCaption 从 16 增至
64 bytes，投影时新增一个 80-byte 堆对象。这个结论只适用于内联 payload，
不能据此否定把查询职责集中进 store。当前实现保留小型引用并消除分散的访问器。

## 上游的释放修复与剩余边界

[PR #276](https://github.com/swiftlang/swift-markdown/pull/276) 于 2026-06-22 合并。
当前 [RawMarkup.deinit](https://github.com/swiftlang/swift-markdown/blob/75e3df1d7b664ef3c96595de36c243b98599b3bc/Sources/Markdown/Base/RawMarkup.swift#L164)
用工作栈保留 children，反初始化子引用，并在确认子节点唯一持有后继续拆除其
children，再将 childCount 置零。这个算法已修复 RawMarkup 向下的递归释放。

使用真实上游库、公开 API 和独立进程实验：

| 实验 | 30,000 层 | 65,536 层 |
| --- | --- | --- |
| 循环构造 `BlockQuote([node])`，正常释放根 | 成功 | 成功 |
| `Document(parsing:)` 解析重复 `> `，正常释放根 | 成功 | 成功 |
| 构造后逐层 `child(at: 0)`，保留最深 Text，再正常释放 | 成功 | SIGSEGV |
| 同一最深 Text 实验，打印后 `_exit(0)` 跳过释放 | 未执行 | 成功 |
| 每次下降立即 `detachedFromParent`，最后正常释放 | 未执行 | 成功 |

失败程序已输出到达 65,537 层、literal 为 `leaf`、即将释放。主机 LLDB 显示
栈反复经过 `destroyGenericBox` 与 `_swift_release_dealloc`，在压栈时触发
`EXC_BAD_ACCESS`。结合 `_MarkupData.parent: Markup?` 强引用以及逐步 detach
对照，证据指向公开节点的 parent existential 所有权链。RawMarkup 自身的迭代
析构没有覆盖这条向上的链。具体崩溃深度受编译器、线程栈等条件影响。

本仓库用 65,536 层列表，逐层获取最深 Paragraph 并放弃 Document，经过
131,073 次父子移动后，正常释放成功。原有根/独立容器释放回归也通过。
这比较的是生命周期边界，不是两种输入的性能排名。

可复现实验骨架（上游公开 API）：

```swift
@inline(never) func build(_ depth: Int) -> any BlockMarkup {
    var node: any BlockMarkup = Paragraph(Text("leaf"))
    for _ in 0..<depth { node = BlockQuote([node]) }
    return node
}

@inline(never) func exercise(_ depth: Int) {
    var node: any Markup = build(depth)
    while let child = node.child(at: 0) { node = child }
    withExtendedLifetime(node) {
        print("before release")
        fflush(nil)
    }
}

exercise(65_536)
print("released")
```

## 替代方案评估与决定

| 方案 | 收益 | 代价或不满足的边界 |
| --- | --- | --- |
| 恢复递归 `[Markup]`，只在 Document 结束时清理 | 公开数组 API 简单 | 独立持有的子树晚于 Document 释放时，仍会递归 ARC 释放。 |
| 平坦存储，但每次 getter 返回物化数组 | 保留数组类型 | 每次访问复制 O(k) 个子值并分配数组；缓存会引入额外状态和所有权问题。 |
| 借鉴上游 ManagedBuffer，去掉本库不需要的 parent/编辑机制 | 可按子树保有内存，尾部分配可改善局部性 | 是可行替代，但仍需要 payload 与子关系 view；增加手动初始化/反初始化、唯一引用判定和析构工作栈。移植后还需证明具名关系与 Sendable 边界。 |
| 当前不可变平坦 Swift 存储 | 无手动析构、跨 parse 状态或锁；类型经 Swift Sendable 检查；释放栈有界 | 容器子树或子关系会保留整个 Swift store，公开子关系类型变更为 collection view。 |

目前没有证据表明替代方案同时更简单并满足全部约束，建议保留当前平坦存储。
这不是因为上游也有类似类型，而是本库只读、无需 parent 导航或编辑的模型，
可以用不可变整数边直接消除递归所有权。若后续真实编辑器使用方式表明
“长期只保留小子树却保留整个文档”成为主要内存成本，应重新评估按子树拥有的
存储；不应先为这种尚未测出的负载引入自定义析构。

本轮没有修改生产实现。完整程序、构建日志、退出状态和调用栈保存在本地被忽略的
`build/review/`：`upstream-release.swift`、`upstream-release-results.json`、
`upstream-descendant-backtrace.log`、`upstream-detached-each-step.log` 和
`our-descendant-release.log`。对照不涉及采用上游的 source range 转换；本库的
cmark UTF-8 editor scope 契约保持原样。
