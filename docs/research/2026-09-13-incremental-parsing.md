# 增量 Markdown 解析调研（2026-09-13）

目标是完整保留 markdown-core 当前方言和 canonical AST，同时改善 LLM
streaming 与 editor edits。以下结论来自官方文档或项目源码；没有对这些外部
解析器运行横向性能测试，不能据此给出吞吐量排名。

## 方案比较

| 方案 | 可以借鉴 | 对本项目的限制与决定 |
| --- | --- | --- |
| cmark feed/finish | 输入分块与解析器生命周期分离 | feed 接收字节，finish 才返回树；接口本身不提供每个前缀的 immutable snapshot 或任意编辑。不能把恢复类似接口当作完成增量化。 |
| Lezer Markdown | 手写 block parser、上下文约束下的 tree fragment reuse、紧凑树 | 官方明确不验证 reference 是否定义；不能直接替换语义完整的 parser。借鉴复用条件，不引入另一套方言。 |
| Tree-sitter | 编辑映射、旧树共享、分段读取、多解析区域 | Markdown grammar 分 block/inline 两次解析；当前扩展语法、资源、锚点与 AST 投影仍需实现。替换引擎的兼容成本高于给当前引擎增量化。 |
| micromark | token events、分离 preprocessing/parse/postprocess/compile | 支持流式输入，但输出编译前会缓冲事件；这与每 chunk 发布可修正的完整 AST 是不同能力。 |
| streaming-markdown | 随 chunk 输出、提前展示未闭合内容的 UX | 明确采用 optimistic rendering，且强调已输出 DOM 不修改；不能作为本项目 exact-prefix semantic oracle。 |
| VS Code piece tree | 字节内容与行索引分离、平衡树定位、避免整串复制 | 它解决文本存储，不解决 Markdown 的引用和前瞻依赖；需要与解析 checkpoint、映射和版本共同设计。 |
| Roslyn immutable syntax | 不可变快照、线程间读取、共享未改变部分 | 思路可借鉴，但本项目还必须处理非局部 Markdown 语义和多语言 ABI；不能直接照搬 C# 的节点布局。 |

cmark 的官方头文件给出 feed 多次、finish 返回树的使用顺序。本项目 HEAD
已经只有 one-shot 操作，不能假定上游 feed 仍在当前实现中可用。
[cmark API](https://raw.githubusercontent.com/commonmark/cmark/master/src/cmark.h)

Lezer 的 README 说明它以手写 Markdown parser 产生 Lezer-style trees，并消费
tree fragments；`[a][b]` 即使没有定义也可能按 link 解析。源码的
`reuseFragment` 先检查 fragment 位置和 block context hash，再接入旧节点。
我们需要更严格的状态/依赖等价，hash 只能用于快速排除，不能充当唯一正确性依据。
GitHub 仓库已标为 archive 并指向新托管地址；本次直接读取的是仍可访问的
GitHub 源码，新地址与部分 Lezer 文档请求失败，不声称验证了新托管代码。
[Lezer README](https://github.com/lezer-parser/markdown),
[Markdown parser 源码](https://raw.githubusercontent.com/lezer-parser/markdown/main/src/markdown.ts)

Tree-sitter 的官方顺序是先用 `TSInputEdit` 调整旧树，再把旧树传回 parse，
新树内部共享旧结构；位置缓存和版本必须一起处理。它的 Markdown grammar
要求先 block parse，再用 included ranges 做 inline parse。对本项目的启示是
复用应跨越 source、syntax、projection，而不是仅保留某个 C parser 指针。
[Tree-sitter incremental API](https://tree-sitter.github.io/tree-sitter/using-parsers/3-advanced-parsing.html),
[Markdown grammar](https://github.com/tree-sitter-grammars/tree-sitter-markdown)

micromark 区分 streaming input 与最终 compilation；README 明确说明 compilation
前缓冲 events。streaming-markdown 则在尚未闭合代码时就改变样式。二者分别
展示了“接收增量输入”和“乐观呈现”，都不自动满足我们的 exact-prefix AST。
[micromark 架构](https://github.com/micromark/micromark#architecture),
[streaming-markdown 行为](https://github.com/thetarnav/streaming-markdown#usage)

VS Code 的文本缓冲重构围绕 piece table/red-black tree、行信息与内存组织展开；
其经验支持给 source 单独建立带字节/行摘要的平衡树。Roslyn 的 immutable trees
说明快照式读取和局部结构共享可以共存。采用这些思路是本项目的设计判断，
并非来源对 markdown-core 性能的保证。
[VS Code text buffer](https://code.visualstudio.com/blogs/2018/03/23/text-buffer-reimplementation),
[Roslyn syntax model](https://learn.microsoft.com/en-us/dotnet/csharp/roslyn-sdk/work-with-syntax)

## 选择

采用当前 C 元素语法之上的 **可恢复 block/inline 解析 + 显式依赖失效 + 不可变
分段快照**。Append 是 EOF 处的 edit；两者共用 source、状态、语义解析和发布
算法。保留 baseline 作为迁移期间的 differential oracle；迁移结束后移除旧的
独立驱动，而不是维护两个正式解析器。

没有选取“遇空行永久冻结”或“永远只解析最后一块”。本仓库的
[16 个可执行反例](../../experiments/incremental/results.json)覆盖引用、脚注、
锚点保留、注释、表格和 caption、setext、definition list、list tightness、
block identifier、Properties 和容器闭合。它们在 baseline 上展示：文本追加
可能改变旧节点的 kind、字段、拥有关系或 source scope。

完整方案与复杂度边界见 [设计与架构](../architecture/incremental-parsing.md)。
