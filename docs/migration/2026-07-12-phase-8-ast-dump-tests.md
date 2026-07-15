# Phase 8:将所有 parser tests 迁移到 AST dump

状态:已完成。所有保留的 parser/spec/API/fuzz tests 不再调用任何 renderer;
断言对象只有 canonical AST dump 与 typed accessors。

> Phase 18 只迁移跨产品 canonical conformance goldens 到 root shared manifest；
> 本阶段的 C-owned CommonMark/extension/regression correctness fixtures 与 AST-dump
> 断言仍位于 C package，未进入 binding correctness discovery。

## 1. Render-dependent assertions 分类(基于 Phase 7 inventory)

| 类别 | 处置 |
| --- | --- |
| spec/extension fixtures(12 个文件、801 例) | expected block 由 HTML/XML 全部迁移为 canonical AST dump |
| `spec_roundtrip_commonmark`、`extensions_roundtrip_gfm` | 删除(commonmark-writer roundtrip 只有 renderer 价值);AST determinism 改由 `spec_runner` 每例双 dump 与 `fuzz_smoke` 覆盖 |
| `extensions_option_table_style`、`extensions_option_full_info_string` | 保留为 AST 套件:alignment 与完整 info string 是 AST 语义(dump 恒输出),roundtrip 载体删除 |
| `spec_entities`(HTML substring) | facade 解析后对 Text literal 断言;HTML 转义 exceptions(quot/nvlt/nvgt)整表删除——AST literal 即原始 UTF-8 expansion |
| `pathological_*`(HTML regex ×30) | facade 结构断言:node-kind 计数、Text literal 拼接(等于原输入)、typed property probe;迭代遍历避免 50000 深度递归 |
| `pathological_complexity_*`(HTML 转换计时) | facade parse 计时,相对比率断言不变 |
| `api_engine`(61 处 renderer 引用) | 见 §2 |
| `fuzz_smoke` | Phase 7 已是 parse/traverse/dump/free,无变化 |
| libFuzzer campaign harnesses(`markdown-core-fuzz.c`、`fuzz_quadratic*.c`) | render 调用替换为 iterator 全树遍历 + accessor 触达 |
| `facade_dump_cli` | 本就基于 `-t ast`,无变化 |
| spec.txt 的 `tagfilter` 例子 | 删除(tagfilter 只改 HTML 输出中的 `<`,无 AST 语义,不伪装为 AST test) |

## 2. `api_engine` 迁移明细

删除的纯 renderer 套件:`render_html`、`render_xml`、`render_man`、
`render_latex`、`render_commonmark`、`render_plaintext`、`test_safe`
(OPT_SAFE/UNSAFE 只是 renderer 策略)、`custom_nodes`(CUSTOM_INLINE/BLOCK 的
on_enter/on_exit 是 renderer payload)。

改为 AST/accessor 断言:

- `accessors`:整篇 rendered-HTML 校验改为逐 setter 的 getter 回读断言。
- `iterator_delete`/`create_tree`:HTML 校验改为树结构与 literal 断言。
- `utf8`/`numeric_entities`/`parser`:`test_md_to_html` helper 替换为
  `test_md_paragraph_text`(单段落 Text literal 拼接比较,raw bytes 无 HTML
  转义)。
- `line_endings`:列表/代码块结构断言;其中 `OPT_HARDBREAKS`/`OPT_NOBREAKS`
  两个断言删除——它们只改 renderer 输出,AST 中 CRLF 恒为 SoftBreak(保留该
  parser 级断言)。
- `source_pos`、`source_pos_inlines`、`ref_source_pos`、`autolink_source_pos`:
  sourcepos XML 断言改为 facade parse + canonical dump byte 比较(dump 对每个
  node 输出 `scope=`,覆盖同一坐标语义);新增 `test_facade_dump` helper。
- `directive_extension_accessors`:commonmark/XML/HTML transport 断言删除,
  归一化 JSON 断言改为直接 accessor 比较。
- `test_pathological_regressions`:改为 parse+free 计时,不再渲染。
- C++ consumer(`cplusplus.cpp`):markdown_to_html 断言改为节点类型 + literal。

结果:650 断言全过(迁移前 649,断言数净持平——删除的 renderer 断言由新的
结构断言抵消)。

## 3. Fixture 迁移与人工审查

生成方式:`spec_runner --rewrite`(新增的显式维护模式)按套件的
`ParseOptions` 组合重新生成 expected;markdown 块与 tags 逐字节不变。

审查记录(防自证链条):

1. **Witness**:Phase 8 未触碰任何 parser/facade 源码;同一工作树上
   Phase 7 结束时 HTML byte-exact 套件 57/57 全绿(见 Phase 7 报告)。因此
   dump 编码的正是通过 CommonMark/GFM HTML 验证的同一批 AST。
2. **结构校验**:脚本比对迁移前后每个 fixture 的 example 数、tags、markdown
   字节:除 spec.txt 删除 1 个 tagfilter 例子外全部一致。
3. **语义抽查**:逐 kind 检查 dump(table alignments/header row、footnote
   id、formula mode standalone/embedded、directive attributes JSON 与
   label=null/label=N、full info string(含 NUL→U+FFFD)、smart punct、
   autolink 的空前导 Text 与 title=null/"" 区分、scope 坐标与原 XML sourcepos
   一致)。
4. **异常扫描**:801 个启用例子的 expected block 全部以 `Document scope=`
   开头,无残留 HTML/XML。
5. 原 `<IGNORE>` 例子(autolink crash 回归)已改为真实 dump 断言。

选项映射说明:legacy「extension 挂载但 option 关闭」的 option-gates 套件在
facade 中等价于对应 `ParseOptions` 字段为 false;`-e footnotes`→`footnotes`
字段;tagfilter 不进入 `ParseOptions`(无 AST 语义)。

## 4. 测试基础设施变化

- `test_support` 删除全部 render 路径(HTML/XML/commonmark 转换、roundtrip
  空白折叠 canonicalization),新增 facade 层:`ts_ast_options_none`/
  `ts_ast_enable`(fixture tag → typed options)、`ts_ast_parse`、迭代
  `ts_ast_walk`/`ts_ast_count_kinds`/`ts_ast_concat_text`。
- `spec_runner` 仅 AST 模式;每例双 dump 断言确定性;`--rewrite` 见上。
- CTest 套件数 57 → 55(删除 2 个 roundtrip 套件),labels 不变。
- 比较策略中不再存在任何 canonicalization 层(契约 §6 已更新)。

## 5. 验证

- `ctest --preset correctness`:55/55;`ctest --preset benchmark`:6/6。
- ASan correctness 55/55;`swift test` 4 suites/10 tests;`pnpm verify` 全链
  路通过(见 Phase 8 结束时验证)。
- 全仓测试树 renderer 调用审计:`grep render_|markdown_to_html` 在
  `packages/markdown-core/tests/` 与 `fuzz/` 下无命中(注释除外)；Phase 15 已删除
  原根级 `tests/`。
- Renderer 本体与 CLI `-t html|xml|man|latex|commonmark|plaintext` 仍存在,
  由 Phase 9 删除;Phase 8 之后已无任何测试依赖它们。
