# Pandoc 差异用例逐项说明（原 34 项，现存 33 项）

这里的 34 项是差异登记表中的 **34 个输入用例**，不是 34 个未实现的
feature，也不是 34 个需要追加支持的 Pandoc extension。同一条规则可以
产生多个用例。以下 Core 指 Markdown Core；Pandoc 指固定的 3.11 oracle，
每例使用 corpus 指定的 `markdown_strict` 加选定扩展配置。

验收范围是用户要求的 feature 及现有规范；Pandoc 提供行为证据，不扩大
功能范围。parser 保留语法与语义事实，编号、坐标展开和排版仍由 consumer
完成。差异仍须逐项解释，不能因为登记过就掩盖功能缺失。

原始输入和 reader 配置见 [corpus.json](corpus.json)，
精确结果摘要和说明见 [deltas.json](deltas.json)。
以下保留原始审计编号；第 21 项已修复并退出差异登记。历史 `projection` 标签不表示所有条目都只是
AST 表示不同。

2026-09-12 的 [O/I/P 来源审计](../../../docs/plans/2026-09-12-oip-oracle-drift-audit.md)
发现的空上标、author-tail 引用分组和畸形组回退已按用户要求修复。
第 1、10、20 项仅保留各自尚存的独立差异；第 21 项现在是无豁免的一致用例。
第 7 项仍需区分 reader 配置与失败回退的差异。

| 编号 | 用例 ID／主题 | Core 与 Pandoc 的具体差异 |
| --- | --- | --- |
| 1 | `empty-superscript-and-subscript`：空上下标 | `^^` 已在两边生成空 Superscript。剩余差异仅是 `~~`：Core 的双波浪线用于删除线，未配对时为文本；此例 Pandoc reader 生成空 Subscript。 |
| 2 | `pandoc-reference-attribute-merge`：引用属性合并 | Core 按“定义处、使用处”的顺序保留类名和属性记录，包括重复项；Pandoc 去重、覆盖并调整顺序。例如 `k=def ... k=occ` 在 Core 中都保留，Pandoc 只保留使用处的 `k=occ`。 |
| 3 | `multiline-table`：多行表格列宽 | Core 保存源码列宽占总列宽的比例；Pandoc 把分隔部分计入宽度，再相对于默认页面宽度计算。差异是列宽数值，表格内容已经实现并一致。 |
| 4 | `grid-table-block-cells`：块内容网格表的列宽 | 与第 3 项同属列宽语义差异。源码内部列宽为 15、15、20 时，Core 为 `0.3, 0.3, 0.4`，Pandoc 为 `16/72, 16/72, 21/72`。单元格中的段落、列表等块内容一致。 |
| 5 | `grid-table-row-and-column-spans`：跨行跨列表的列宽 | 仍是相同的列宽差异；此例的跨行、跨列几何和单元格内容一致，不是缺少 rowspan/colspan。 |
| 6 | `example-lists-and-reference`：例句定义和引用模型 | Core 在文档中按源码顺序保存 Specimen 定义，以准确的 ID 保存引用；Pandoc 输出带例句编号的 List，并把引用替换为编号文本。Core 将编号交给 consumer。 |
| 7 | `p6-escaped-space`：上下标中的转义空格 | 对 `^a\ b^` 和 `~*c\ d*~`，Core 生成上下标并把其中的转义空格解码为 NBSP；此例 Pandoc reader 不生成上下标。Core 在候选失败或上下标外保留原来的反斜线与空白，不做全局替换。 |
| 8 | `p6-whitespace-recovery`：Unicode 空白与上下标 | Core 把源码中的所有 Unicode White_Space 都视为上下标配对边界；Pandoc 接受含 U+2003 EM SPACE 的 `^a b^`，Core 保留为文本。此例前面的 ASCII 空格失败恢复，两边一致。 |
| 9 | `p6-entity-space`：实体解码出的空白 | `~a&Tab;b~` 中，Core 保留解码出的 TAB，Pandoc 将其归一化为空格。实体解码出的空白不等同于源码中的原始空白分界。 |
| 10 | `p6-pairing-and-tilde-runs`：空配对和连续波浪线 | `^^` 已一致。`^^^^` 在 Core 中保留两个有独立 scope 的空 Superscript，Pandoc 合并相邻同类节点。Core 将三个以上连续波浪线及未配对的双波浪线保留为文本；Pandoc 可生成空下标，并把 `~~~c~~~` 解析成下标 `c`。 |
| 11 | `p6-opaque-tokens`：上下标内的代码跨度 | 对包含代码 `x y` 的外层上下标候选，Core 先识别完整代码跨度，其内部空格不打断外层上下标；Pandoc 不识别该外层上下标。独立代码内部的 `^`、`~` 在两边都保持代码文本。 |
| 12 | `anchor-global-reservation`：后置标题显式 ID | Core 先预留全篇显式 ID，再生成自动 ID：前面的 `# x` 在后面存在 `{#x}` 时取得 `x-1`。Pandoc 先生成前面的 `x`，后面显式 ID 也为 `x`。 |
| 13 | `anchor-permitted-scalars`：emoji 与锚点 | Core 按字符规则过滤，不把 emoji 转成名字；Pandoc 的 GFM 锚点算法把 😀 转成 `grinning`。本例锚点分别为 `a‿b--` 与 `a‿b--grinning`。 |
| 14 | `anchor-simple-lowercase`：Unicode 小写转换 | Core 使用 Unicode 17 simple lowercase，将 `İ` 转为单个 `i`；Pandoc 的 full lowercase 转为 `i` 加 U+0307 组合点，锚点的字符序列不同。 |
| 15 | `anchor-whitespace-scalars`：锚点中的 Unicode 空白 | Core 把 U+0085、U+2028 等 Unicode White_Space 转成连字符；Pandoc 在此例中删除这两个字符，因而分别得到 `a-b-c` 与 `abc`。 |
| 16 | `anchor-inline-code-reservation`：代码的显式 ID 预留 | 与第 12 项同一条全局预留规则，显式 `{#x}` 这次附在行内代码上。Core 前面的标题取得 `x-1`；Pandoc 标题仍为 `x`。 |
| 17 | `implicit-reference-adjacency`：引用方括号是否必须相邻 | Core 沿用 CommonMark 的相邻要求，将 `[Foo] [Foo][] [go][Foo]` 分别解析成三个标题链接；Pandoc 跨过空格合并前两个方括号组，并留下不同的文本和链接结构。 |
| 18 | `p5-heading-span-reservation`：Span 的显式 ID 预留 | 与第 12 项同一规则，显式 ID 这次来自 `[owner]{#taken}`。Core 前面的 `# Taken` 为 `taken-1`，Pandoc 为 `taken`。 |
| 19 | `p7-tail-and-nesting`：引用前后缀的边缘空白 | Core 去除 citation prefix/suffix 源码两端的空白；Pandoc 在嵌套方括号之前保留 suffix 的前导空格。嵌套内容仍逐项比较。 |
| 20 | `p7-tail-later-author`：作者式引用尾部的另一个引用 | 对 `@a [@b [p. 7]]`，两边现在都将 `@b` 保留为外层 Cite 的 normal item。仅剩前后缀空白规则：Core 的 suffix 为 `[p. 7]`，Pandoc 保留前导空格。 |
| 21 | `p7-malformed-group`：畸形 citation 组的回退 | **已修复并退出登记。** 外层组失败后继续普通行内解析，内部合法 key 成为 authorInText citation，合法 tail 也保留；原用例已与 Pandoc 一致。 |
| 22 | `p7-heading-projection`：citation 参与标题锚点生成 | 对 `## [pre @a suffix; @b]`，Core 从存储的引用内容生成 `preasuffixb`；Pandoc 从保留分隔空格的原始显示文本生成 `pre-a-suffix-b`。 |
| 23 | `p7-unicode-boundary`：下划线之后的引用起点 | `_@a` 在 Core 中不能开始 citation，因为起点前不能是下划线、Unicode 字母或数字；Pandoc 接受下划线之后的 citation。 |
| 24 | `p9a-nested-start`：非 1 起始的嵌套列表候选 | 在父列表段落后，缩进的 `2. ordinary` 不能启动 Core 的嵌套列表，按 lazy continuation 留在原段落；Pandoc 将这行放到另一个 Plain 块。随后 `1. nested` 在两边都成为嵌套列表。 |
| 25 | `p9b-resolution`：前向、重复、匿名例句与起始编号 | 与第 6 项同一模型差异，覆盖前向引用、显式起始编号 7、重复 label、匿名定义。Core 保留所有定义、源码 start 和引用 ID；Pandoc 把引用渲染成 `7` 等文本，并把定义放入例句 List。 |
| 26 | `p9b-heading`：标题中的例句引用 | `## @label` 的 Core 标题内容仍是 specimen citation；Pandoc 标题内容已替换为文本 `1`。**本例两边的锚点都是 `label`**，差异是标题内容及定义模型，不是锚点值。 |
| 27 | `p7-unresolved-reference-tail`：无法解析的链接引用尾部 | 对 `[@a][missing]`，Core 允许前面的完整 citation 组成立；Pandoc 保留第一组方括号为文本，并把其中的 `@a` 解析成 authorInText，引用模式和结构不同。 |
| 28 | `p7-explicit-citation-shortcut`：citation 与引用定义的优先级 | 出现 `[@a]: /u` 时，Core 仍将该行注册为链接引用定义，但正文完整的 `[@a]` citation 优先于快捷链接；Pandoc 将定义形状的行也解析成 citation 加后续文本。 |
| 29 | `p9a-start-always-authored`：未启用 startnum 的对照配置 | `h. eight` 在 Core 中始终保存 start=8；此例故意未启用 Pandoc 的 `startnum`，所以 Pandoc 保存 1。正常选定 feature 映射已经将 `fancy_lists` 与 `startnum` 一起启用；这不是缺少起始编号功能。 |
| 30 | `p8-closer-width`：容器结束围栏的长度 | Core 要求结束冒号串至少与开始围栏一样长；Pandoc 接受任意不少于三个冒号。因此四冒号开始的容器遇到三个冒号时，两边结束位置不同。 |
| 31 | `p8-explicit-anchor-reservation`：容器的显式 ID 预留 | 与第 12 项同一规则，后置显式 ID 这次属于 fenced div。Core 前面的 `# Reserved` 取得 `reserved-1`；Pandoc 仍为 `reserved`。 |
| 32 | `p10-nested-and-lazy`：嵌套定义体的 compact 信息 | Core 单独保存 term 与 body 间源码空行所决定的 compact 布尔值；Pandoc 通过 Plain/Para 表达紧凑性。首个 body 为嵌套 DefinitionList 时，没有对应标志可供读取，比较值为 null；term 与 body 内容一致。 |
| 33 | `p10-padding-and-tabs`：代码定义体的 compact 信息 | 与第 32 项同一 AST 信息差异，发生于首个 body 为 CodeBlock 的情况。差异不是 TAB 或代码内容解析错误，而是 Pandoc 不保留对应 compact 标志。 |
| 34 | `p10-empty-bodies`：空定义体的 compact 信息 | 与第 32 项同一 AST 信息差异，发生于首个 body 为空的情况。空 body 及后续 body 都保留并比较；差异只有 Core 明确保留的 compact 信息。 |

最明显的重复关系是：

- 3–5：同一列宽语义，在三种表格用例中出现。
- 6、25、26：同一例句定义／引用模型，在普通、前向引用及标题中出现。
- 12、16、18、31：同一全局显式 ID 预留规则，覆盖四种声明位置。
- 32–34：同一 compact 信息差异，覆盖嵌套块、代码块和空 body。

这些记录分别由现有模块约束：[上下标](../../../docs/specs/dialect/superscript-and-subscript.md)、
[属性](../../../docs/specs/dialect/attributes.md)、[表格](../../../docs/specs/dialect/tables.md)、
[例句](../../../docs/specs/dialect/specimens.md)、[锚点](../../../docs/specs/dialect/anchors.md)、
[链接](../../../docs/specs/dialect/links-and-images.md)、[文献引用](../../../docs/specs/dialect/citations.md)、
[列表](../../../docs/specs/dialect/lists.md)、[容器](../../../docs/specs/dialect/directives.md)、
[定义列表](../../../docs/specs/dialect/definition-lists.md)。是否存在实现缺口，应据对应模块
的功能、边界和组合测试判断，不能用差异条目数量代替。
