# Markdown Core 需求文档

## 1. 文档状态

本文档是 `Markdown Core` 项目的实施需求和新 session 交接文档。

已确定的产品、架构和命名决策使用“必须”或直接陈述；若实施中发现新的未决事项，必须显式记录并在对应实现开始前确认，不得默认扩大范围。

## 2. 项目定位

`Markdown Core` 是一个跨平台 Markdown 解析基础设施项目。

它在同一个 monorepo 中永久共同维护：

- C Markdown parser engine。
- 只读 C AST API。
- Swift immutable AST binding。
- Kotlin immutable AST binding。
- ECMAScript/TypeScript immutable AST binding。
- 三端共享的语义一致性测试。

本项目是下游渲染器和应用的统一 Markdown AST 上游，不是一个临时 binding 仓库。

## 3. 项目身份与演进来源

### 3.1 新项目身份

- 仓库名：`nouprax/markdown-core`。
- 项目名：`Markdown Core`。
- GitHub organization `nouprax` 已由项目所有者控制。
- npm organization `nouprax` 已创建，并用于持有 `@nouprax` scope。
- `nouprax.com` 已由项目所有者控制，Maven Central `com.nouprax` namespace 的 DNS 所有权验证已完成。
- 本项目拥有独立于 `cmark-gfm` 的命名、包坐标、公共 API 和版本线。
- 新的公共产品名称不得继续使用 `cmark-gfm`。
- 新的公共 C API 不得新增 `cmark_*` 命名的 symbol。
- 初始版本发布前，除法律/历史归属文本和 Git history 外，项目不得保留任何 `cmark`、`cmark-gfm`、`CMARK_*` 或 `cmark_*` 产品/实现命名。

### 3.2 演进来源

Markdown Core 的 C engine 起源于 [DongyuZhao/cmark-gfm](https://github.com/DongyuZhao/cmark-gfm)，以下 commit 是新项目建立时的基线：

```text
711032b2a16cf25c3df75033833eba086b17ca6a
```

该 fork 已经与原 `cmark-gfm` 项目实质性 diverge，不计划重新合并回 `cmark-gfm`。从 Markdown Core 建立起：

- C engine 是 Markdown Core 自身的一级源码，不是 vendored third-party dependency。
- Swift、Kotlin 和 ES binding 与 C engine 在同一 repo 中永久共同维护。
- 项目不再跟随 `cmark-gfm` 的命名和版本。
- 不再存在“将 C 依赖从 master commit 升级到上游 release tag”的产品计划。

### 3.3 历史与许可证

- 必须完整保留 cmark/cmark-gfm 原项目适用的 copyright 和 license notices，不得因重命名或重写而删除。
- 根 `LICENSE` 与必要的 `THIRD_PARTY_NOTICES`/`COPYING` 必须包含适用许可证文本和归属。
- 必须在 `README.md` 和 `UPSTREAM.md` 中明确说明：Markdown Core 继承自 cmark/cmark-gfm，在继承代码的基础上已重写部分代码，并已形成不计划合回上游的独立项目。
- `UPSTREAM.md` 必须记录 fork 来源、基线 commit、diverge 说明、主要重写/删除范围和许可证继承关系。
- 可以保留原 Git commit history。
- 新项目应建立独立 release lineage，不应让原项目的旧 SemVer tags 参与新 SwiftPM package 的版本解析。

### 3.4 内部旧命名的迁移

为降低大规模机械重命名对成熟 C 代码带来的回归风险，重命名必须作为独立阶段执行，不与 parser 语义修改混合。但初始发布前必须完成全量 rebrand：

- 不得保留 `cmark_*` functions、types、globals 或内部 symbols。
- 不得保留 `CMARK_*` macros、enum constants 或 include guards。
- 不得保留带 `cmark`/`cmark-gfm` 的源文件、headers、目录、CMake targets、library names、pkg-config files、CLI binary names、test names、scripts 或 package metadata。
- 不得保留运行时错误消息、help text 或生成产物中的旧品牌名。
- 新公共与内部 C 命名必须使用 `markdown_core_*` / `MARKDOWN_CORE_*` 或另一个文档化的 Markdown Core 内部前缀。
- Renderer 代码只能在 AST dump 和测试迁移完成前临时保留；初始版本发布前必须从源码、构建、CLI、公共 headers 和导出 symbols 中删除。
- 新项目不保留 AST mutation public API。

`cmark` 名称只允许出现在以下历史/法律上下文：

- `LICENSE`、`COPYING` 或 `THIRD_PARTY_NOTICES` 中的原始许可证和归属。
- `README.md` 和 `UPSTREAM.md` 的来源及重写声明。
- `docs/` 中明确标记为 migration/history 的文档。
- Git commit history。

## 4. 项目目标

1. 在同一 monorepo 中建立 SwiftPM、Kotlin/Gradle 和 npm 三个 platform packages。
2. 三个 packages 编译并调用同一 commit 中的 C engine。
3. 三端对外提供语义一致的 AST 类型、字段和树结构。
4. 每个平台只提供 `Document.parse` 这一个公共解析入口。
5. 对外只提供 immutable AST，不提供 AST mutation API。
6. 建立统一的跨端 AST conformance test suite。
7. 建立统一版本和协调发布流程。
8. 仓库的多包管理、根级命令和工具链 setup 参考 `/Users/donz/Repos/GitHub/code-block`。
9. 在建立新项目时完成 C engine 的历史目录清理，使 core、extensions、tests 和 platform packages 的归属清晰。
10. 删除所有 Microsoft/MS 私有 Markdown extensions 及其选项、测试和生成代码。
11. 提供原生 C AST dump 能力，并将 parser/spec tests 的预期结果从 render output 迁移为 AST dump。
12. 在测试迁移完成后删除所有 HTML、XML、CommonMark、LaTeX、man 和 plaintext render 能力。
13. 完成全仓库 rebrand，除必要法律/历史归属外不保留任何 cmark 命名。

## 5. 非目标

新的 platform packages 不包含：

- Markdown 到 HTML 的公共转换 API。
- Markdown 到其他文本、视图或渲染格式的公共转换 API。
- HTML、XML、CommonMark、LaTeX、man 或 plaintext renderer 实现。
- UI 渲染器、SwiftUI/Compose/React 组件或样式系统。
- 针对 AST 的编辑、插入、删除、替换或重排 API。
- 将 AST 序列化为 JSON 作为 C 与 platform binding 之间的生产交互协议。
- 将三端分别实现为三套独立 parser。
- 与原 `cmark-gfm` 继续保持 source-level 或 release-level 同步。
- 为旧 `cmark_*` C ABI、旧 library/CLI 名称或旧 package coordinates 提供兼容别名。

AST dump 是公开的诊断用树结构表示，同时作为测试 expected contract；它不是 Markdown content renderer、持久化格式或 C-to-binding 生产传输协议。

## 6. 发行命名

### 6.1 命名家族

三个 platform packages 使用带生态前缀的对称命名：

| 生态 | Package / artifact 名 | 公共 module/package |
| --- | --- | --- |
| SwiftPM | manifest package `swift-markdown-core` | product/module `MarkdownCore` |
| Kotlin/Maven Central | `com.nouprax:kotlin-markdown-core` | `com.nouprax.markdown.core` |
| npm | `@nouprax/es-markdown-core` | ESM exports |
| C | `markdown-core` | `markdown_core_*` |

生态前缀是发行身份的一部分，不应进入 AST 类型名称。例如，Swift 中使用 `Document`，不使用 `SwiftDocument`。

### 6.2 SwiftPM 命名与组织边界

- SwiftPM 不使用 npm/Maven 式 organization scope。
- 组织身份由 package URL `https://github.com/nouprax/markdown-core` 表达。
- 根 `Package.swift` 的 manifest package name 为 `swift-markdown-core`。
- SwiftPM consumer 通过 repo URL 引用时，package identity 依 repo basename 为 `markdown-core`；在需要显式填写 dependency package identity 的位置使用 `markdown-core`。
- 对外 product 和 importable module 统一为 `MarkdownCore`。
- 不使用 `NoupraxMarkdownCore`、`NoupraxSwiftMarkdownCore` 或 `nouprax-swift-markdown-core`；`nouprax` 不进入 Swift 源码中的 AST 类型名。

### 6.3 Maven Central 命名与所有权

- Maven Central namespace/groupId 为 `com.nouprax`。
- `com.nouprax` 的所有权已通过 `nouprax.com` DNS 验证，不依赖 GitHub organization 名。
- Maven artifactId 为 `kotlin-markdown-core`。
- KMP umbrella/root publication coordinate 为 `com.nouprax:kotlin-markdown-core:<version>`；同一 release 还包含 Kotlin Multiplatform plugin 生成的 target-specific publications。
- Kotlin package namespace 为 `com.nouprax.markdown.core`。
- 不使用 `io.github.nouprax` 或原个人 namespace。

### 6.4 npm 命名与所有权

- npm organization/scope 为 `nouprax` / `@nouprax`。
- npm organization 已创建，GitHub organization 不作为 npm scope 所有权的替代。
- 完整 npm package 名为 `@nouprax/es-markdown-core`。
- 首次对外发布必须是 public scoped package。

### 6.5 ES 命名含义

`es-markdown-core` 中的 `es` 表示 ECMAScript 运行时产物：

- package 对 JavaScript 和 TypeScript 消费者同时可用。
- 运行时交付 JavaScript/ESM 和 WebAssembly。
- package 必须包含 TypeScript declarations。
- 不使用 `ts-markdown-core`，因为 TypeScript 是类型和源码层，不是独立运行时平台。

### 6.6 使用示例

Swift：

```swift
import MarkdownCore

let document = try Document.parse(markdown)
```

Kotlin：

```kotlin
import com.nouprax.markdown.core.Document

val document = Document.parse(markdown)
```

ES/TypeScript：

```typescript
import { Document } from "@nouprax/es-markdown-core";

const document = Document.parse(markdown);
```

## 7. 版本与发布

### 7.1 新版本线

- Markdown Core 使用自己的 SemVer 版本线。
- 新版本不继承 `cmark-gfm` 的版本含义。
- 新项目的首个 release 为 `1.0.0`。
- 新 repo 不迁移、不保留、不归档原 `cmark-gfm` 的旧 tags。
- 新 repo 的 tag namespace 只包含 Markdown Core 自身的 releases，从 `1.0.0` 开始。

### 7.2 统一发布

C engine、C AST API 和三个 platform packages 必须在同一 repo commit 上完成验证。

默认发布策略是使用同一个项目版本：

```text
Markdown Core X.Y.Z
├── C engine / facade source X.Y.Z
├── swift-markdown-core X.Y.Z
├── kotlin-markdown-core X.Y.Z
└── @nouprax/es-markdown-core X.Y.Z
```

一个解析行为、AST 契约或 C facade 变更必须在三端共同验证后发布。

### 7.3 发布认证与 secrets

所有对外发布必须由 GitHub Actions 中的受保护 `release` environment 执行，并满足：

- 只允许受保护 release tag 或经确认的手动 workflow 触发。
- 发布 job 必须在全部 build/test/conformance/consumer checks 通过后运行。
- `release` environment 应配置 required reviewer，并在可用时禁止 self-review。
- Registry credentials 和 signing private key 只能保存为 GitHub environment secrets，不得保存在 repo、workflow 明文、Gradle properties、`.npmrc`、shell history 或构建产物中。
- Pull request、fork build 和普通 CI job 不得获取 release secrets。
- Workflow 必须使用最小 `permissions`，对不需要的 GitHub token permissions 显式关闭。
- 不得在 logs 中打印 token、private key、passphrase 或完整认证命令。
- 必须在 `docs/releasing.md` 记录 credential 创建、GitHub secret 名称、用途、权限、到期时间、轮换、撤销和泄漏处置方法，但不得记录 secret value。

#### 7.3.1 npm

- `@nouprax/es-markdown-core` 必须优先使用 npm Trusted Publisher 与 GitHub Actions OIDC 发布，不在 GitHub 中长期保存 npm write token。
- npm trusted publisher 必须绑定到 organization `nouprax`、repo `markdown-core`、精确 workflow filename 和 `release` environment。
- npm publish job 必须使用 GitHub-hosted runner，并只授予 `contents: read` 和 OIDC 所需的 `id-token: write`。
- 必须使用满足 npm trusted publishing 要求的 Node/npm 版本，且 `package.json` 中 `repository.url` 必须精确指向 `nouprax/markdown-core`。
- 通过 OIDC 发布时必须保留 npm provenance attestation。
- 若首次 bootstrap publish 无法直接配置 trusted publisher，只允许使用短期、最小权限的 granular token 或经 2FA 的手工首次发布；trusted publisher 验证成功后必须立即撤销 bootstrap token。
- Trusted publishing 稳定后，npm package 应要求 2FA 并禁止传统 token publishing。

#### 7.3.2 Maven Central

- Maven Central 必须使用 Central Publisher Portal 生成的有过期时间 user token，不得使用账户密码。
- Portal token 的 username/password 必须分别保存为 `release` environment secrets：`MAVEN_CENTRAL_USERNAME` 和 `MAVEN_CENTRAL_PASSWORD`。
- Maven artifacts 必须使用 PGP/GPG 签名，每个 Central 要求的 artifact 必须包含对应 `.asc` signature。
- ASCII-armored private signing key 和 passphrase 必须分别保存为 `MAVEN_SIGNING_KEY` 和 `MAVEN_SIGNING_PASSWORD`。
- Public signing key 必须发布到 Maven Central 支持的 public key server，并在发布前验证可检索。
- Signing private key 必须有 passphrase、到期/转换计划和离线备份；CI secret 不得成为唯一副本。
- Portal token 必须设置合理到期时间，在人员、workflow 或发布工具变更后轮换，发生泄漏时立即撤销。

#### 7.3.3 SwiftPM、C artifacts 与 GitHub Release

- SwiftPM release 使用 repo 的 SemVer Git tag，不需要独立 Swift registry token。
- GitHub Release、source archive、checksums 和必要 C artifacts 必须优先使用 workflow-provided `GITHUB_TOKEN`。
- 只有需要创建 GitHub Release 或上传 artifacts 的 job 可获得 `contents: write`；其他 jobs 保持 `contents: read`。
- 不得默认创建长期 PAT。若 GitHub 能力缺口确实需要 PAT，必须使用 fine-grained、最小 repo 范围、有过期时间的 token，并记录原因。

## 8. 总体架构

```text
Markdown UTF-8 source
          │
          ▼
 Markdown Core C engine
          │
          ▼
 read-only markdown_core C AST API
       ┌──┼──┐
       ▼  ▼  ▼
    Swift Kotlin ECMAScript/WASM
       │  │  │
       ▼  ▼  ▼
 immutable platform AST
```

共享 C 层负责：

- 执行 Markdown 解析。
- 统一注册和启用项目支持的 syntax extensions。
- 将 core 节点和 extension 节点规范化为同一套只读 C AST API。
- 定义清晰的内存所有权和字符串生命周期。
- 保证三个 platform bindings 使用相同的 parser behavior。

Platform binding 负责：

- 调用共享 C parse API。
- 从只读 C AST 构建平台原生 immutable AST。
- 平台 AST 构建完成后释放临时 C document。
- 确保公共 AST 不依赖裸指针或手动释放。
- 在不改变语义的前提下使用平台原生类型系统表达 AST。

## 9. C AST API 需求

### 9.1 API 性质

C 层必须提供直接的 AST parse/traversal API，不得通过 JSON 或其他文本序列化格式在 C 层和 platform binding 之间传递 AST。

新公共 C 头文件应只暴露：

- 解析 document。
- 销毁 document。
- 读取 root node。
- 读取 node kind。
- 遍历 child/sibling。
- 读取当前 node kind 对应的只读属性。
- 读取 source position/range。
- 获取解析失败信息。

新公共 C 头文件不得暴露：

- 修改 node 属性的 API。
- append、prepend、insert、unlink、replace 或其他 mutation API。
- HTML 或其他格式的 renderer API。
- 要求 platform consumer 直接理解的内部 `cmark_node` implementation details。

### 9.2 概念 API

以下是需求层面的 API 轮廓，具体类型和函数名在实现设计时确定：

```c
typedef struct markdown_core_document markdown_core_document;
typedef struct markdown_core_node markdown_core_node;
typedef struct markdown_core_error markdown_core_error;
typedef struct markdown_core_parse_options markdown_core_parse_options;
typedef struct markdown_core_scope markdown_core_scope;

typedef enum markdown_core_node_kind markdown_core_node_kind;

markdown_core_document *markdown_core_document_parse(
    const uint8_t *source,
    size_t length,
    const markdown_core_parse_options *options,
    markdown_core_error **error
);

const markdown_core_node *markdown_core_document_root(
    const markdown_core_document *document
);

void markdown_core_document_free(markdown_core_document *document);

markdown_core_node_kind markdown_core_node_get_kind(
    const markdown_core_node *node
);

const markdown_core_node *markdown_core_node_first_child(
    const markdown_core_node *node
);

const markdown_core_node *markdown_core_node_next_sibling(
    const markdown_core_node *node
);

markdown_core_scope markdown_core_node_get_scope(
    const markdown_core_node *node
);
```

`markdown_core_parse_options` 必须由 facade 提供显式默认初始化函数或常量；不得要求 consumer 复制内部 parser bit flags，也不得用未初始化 struct 表示默认值。`markdown_core_scope` 在正式 header 中必须以明确的 public value types 表达 start/end position。

### 9.3 所有权

- `Document.parse` 输入按 UTF-8 bytes 及其明确长度传入 C，不依赖 NUL termination。
- C document 拥有解析期间产生的 C AST 及其字符串内存。
- C string view 必须是 `{ const uint8_t *data; size_t length; }` 等价的显式 UTF-8 byte view，不承诺 NUL termination。
- Node handle 和 borrowed string view 的生命周期不得超过所属 C document。
- Platform AST 构建完成后必须释放 C document。
- 返回给用户的 platform AST 不得包含需要用户释放的 C pointer。
- 所有失败路径不得泄漏 parser、document、node、string 或 error memory。

### 9.4 Error model

- C facade 使用显式 status/error model，不使用 sentinel 空 document 表示成功。
- Error 至少包含稳定于当前 release 的 error code 和 UTF-8 diagnostic message；若错误与输入位置相关，还必须包含 `Scope` 或 `Position`。
- C 分配的 error 必须由专用 `markdown_core_error_free` 释放，并明确允许为 null。
- Swift 将失败映射为 typed thrown error，Kotlin 映射为 typed exception/error，ES 同步抛出 typed `Error`。
- Error codes/messages 不承诺跨 release ABI compatibility，但三端在同一 release 中必须语义一致。

### 9.5 ABI 边界

- Platform bindings 只依赖新的 `markdown_core_*` 只读 facade。
- 历史 `cmark_*` ABI 必须在全量 rebrand 阶段重命名或删除，不得在初始发布产物中存在。
- C node kind 和属性访问器必须覆盖 core 与自定义 extension nodes。
- 公共 ABI 变更必须与三端 binding 变更位于同一 commit。
- Markdown Core 不承诺跨 release 的 C binary ABI compatibility。
- 每个 release 必须从 clean checkout 重新编译 C engine 与所有 bindings，不支持将新 binding 与旧 C binary 混用。
- C API 变更不添加 compatibility shim、deprecated alias 或 ABI version bridge；三端 bindings 必须在同一 release 中同步更新。

### 9.6 原生 AST dump

- 实现前必须先审计 C engine 是否已经存在不依赖 renderer 的原生 AST dump API。
- `cmark_render_xml` 或 CLI `-t xml` 不视为满足该需求，因为它们属于待删除 renderer 体系。
- 若无独立 AST dump API，必须在 `markdown_core_*` C 边界中添加。
- AST dump 的首要用途是发现 parser behavior drift：相同 Markdown、相同 `ParseOptions` 和相同 canonical schema 下，只要公共 AST 的节点类型、层级、顺序、属性、nullability、文本内容或 source range 发生变化，dump 就必须产生可见 diff。
- Dump 必须覆盖 canonical AST 中每一个 behavior-bearing field，包括 core/extension node kind、原始 children 顺序、所有公开节点属性和完整 `Scope`。不得为了让 golden 更稳定而省略、折叠或重新排序语义数据。
- 即使属性等于默认值也必须输出；optional 属性必须输出其字段名和 `null`，不得用“字段缺失”代替 null。
- Dump 格式必须明确区分 `null`、空字符串 `""`、空 children (`children=0`)、`false`、数值 `0` 和非空值。
- Dump 中的字符串转义、Unicode、数字、枚举、布尔值和换行必须跨平台稳定。
- AST dump 必须可从 C tests 和 test CLI 调用，不依赖 Swift、Kotlin 或 JavaScript。
- AST dump 使用类似文件树的 canonical UTF-8 tree text，不使用 HTML/XML renderer，也不使用 JSON；每个 AST 节点只占一行，通过 `├──`、`└──`、`│   ` 和四个空格表达真实 parent/child 层级，而不是使用括号嵌套或平铺的 node/event 列表。
- Root node 行不带 connector。其余节点根据是否为同级最后一个 child 使用 `├──` 或 `└──`；祖先仍有后续 sibling 时使用 `│   ` 延续竖线，否则使用四个空格。
- 每个节点行的固定输出顺序是：concrete node kind、`scope=<range>`、该 kind 在 canonical AST schema 中声明的全部语义属性、`children=<count>`。字段之间使用单个 ASCII space。
- `children=<count>` 必须始终输出，使用无前导零的十进制数；随后嵌套的直接 child 行数量及顺序必须与该值一致。`children=0` 明确表示 leaf/空 children。
- Children 必须保持 parser 原始顺序；dump 不得按 kind、scope 或属性重新排序 siblings。
- 属性顺序不得由 hash/map iteration、注册顺序或内存布局决定；每个 node kind 的字段顺序必须在 schema 文档和 golden tests 中明确固定。
- `scope` 规范写作 `startLine:startColumn..endLine:endColumn`，四个 integer 坐标及其语义均原样继承同一 Markdown Core release 的 native C parser。
- 所有实际节点都必须有非 optional scope，不允许使用 `null` 或继承 parent scope 作为 fallback；C facade、dump 和三端 binding 不得扫描 source、修正、展开、拒绝或另行解释特定坐标组合。
- 字符串使用 JSON string escaping rules，以明确表达引号、反斜杠、控制字符和换行；非 ASCII Unicode 直接保留为 UTF-8。
- 枚举使用 canonical schema 中固定的 lowercase symbolic name；布尔值只使用 `true`/`false`；整数使用无前导零的十进制；optional value 使用 `null`。
- Dump 使用上述 Unicode file-tree connectors、LF 换行和单一 final newline。除字符串内容外不得输出 trailing whitespace、环境相关路径、pointer/address、locale、平台换行或非确定性 ID。

规范化树示例：

```text
Document scope=1:1..2:5 children=2
├── Heading scope=1:1..1:7 level=1 children=1
│   └── Text scope=1:3..1:7 literal="Hello" children=0
└── Paragraph scope=2:1..2:5 children=1
    └── Text scope=2:1..2:5 literal="world" children=0
```

- 所有语义属性以内联 `name=value` 形式位于 `scope` 与 `children` 之间；示例未列出的节点字段必须在 canonical schema 中逐类型定义，不能由实现自行省略。
- 属性名不得包含空白；string value 必须带双引号，因此即使内容包含空格也不会破坏字段边界。复合值的精确表示必须由 canonical schema 定义并保持单行，不能在属性中插入实际换行。
- Spec/fixture harness 必须把 Markdown source、明确的 `ParseOptions` 和 expected tree dump 作为一个测试用例管理。Options 不需要写入 AST tree，但不得依赖进程全局默认配置来解释 golden。
- Golden comparison 必须是 byte-for-byte comparison，并在失败时提供结构化/逐行 diff，以便定位 drift 到具体 node、field 或 scope。
- 必须用有针对性的 fixtures 证明每个 node kind、每个属性的 null/empty/non-default 状态、每个 extension、Unicode 和 range 边界都会改变 dump；不得仅依赖由当前 parser 自动生成的 goldens 来证明 dump 完整性。
- AST dump API 是三端 package 的公开诊断能力，但其文本不是持久化/传输 ABI。在 canonical AST 未发生有意变更时，dump schema、字段顺序和规范化规则不得漂移。
- 有意的 parser 行为变化必须在 reviewed commit 中附带对应 golden diff 和变更说明；有意的 dump schema 变化还必须同步更新 schema 文档、dump implementation、完整性测试和所有 goldens。不得提供隐藏 drift 的 compatibility/normalization mode，也不得未经人工审查批量覆盖 goldens。
- AST dump 的内存所有权、错误处理和 free API 必须在 C header 中明确。

## 10. 公共 AST 模型

### 10.1 模型来源

AST 定义参考：

```text
/Users/donz/Repos/GitHub/markdown-renderer/
packages/swift-markdown-render/Sources/SwiftMarkdownRender/Tree
```

参考模型中的 `MarkdownElement` 在本项目中重命名为 `Markup`。

Markdown Core 中的 AST 只包含解析语义，不包含 `markdown-renderer` 的 render/presentation-specific state。

### 10.2 命名规则

- 抽象 AST 节点类型命名为 `Markup`。
- 具体类型移除 `MarkdownElement` 前缀。
- 具体类型不添加通用的 `Element`、`Node` 或 `Markup` 后缀。
- 三端使用相同的概念命名。
- C façade kind 与 native node type 使用相同的概念名：非 block 节点使用基础名，block 节点使用 `_BLOCK` 后缀。
- 不提供继承自 cmark 的兼容别名或无命名空间 short-name 宏。
- Native extension 通过注册独立 node type 扩展模型，不保留 generic `CUSTOM` / `CUSTOM_BLOCK` 节点。
- Swift 的 `Code`/`CodeBlock`、`Formula`/`FormulaBlock`、`HTML`/`HTMLBlock`、`Directive`/`DirectiveBlock` 分别放在独立同名文件中；共享 native decoding helper 也单独成文件。
- 生态发行前缀不进入 AST 类型名。
- 保留具体类型名 `List`，不为 Kotlin 改名为 `ListBlock`、`MarkupList` 或其他平台特有名称。
- Kotlin 通过全限定名 `com.nouprax.markdown.core.List` 或 import alias 解决与 `kotlin.collections.List` 的冲突；库实现内对集合类型显式使用 `kotlin.collections.List`。

示例：

| 参考类型 | Markdown Core 类型 |
| --- | --- |
| `MarkdownElement` | `Markup` |
| `MarkdownDocument` | `Document` |
| `MarkdownElementParagraph` | `Paragraph` |
| `MarkdownElementHeading` | `Heading` |
| `MarkdownElementText` | `Text` |
| `MarkdownElementCodeBlock` | `CodeBlock` |
| `MarkdownElementInlineCode` | `Code` |
| `MarkdownElementLink` | `Link` |
| `MarkdownElementImage` | `Image` |

### 10.3 节点范围

初始 AST 必须覆盖参考 `MarkdownElement` 模型中的节点语义：

- `Document`
- `BlockQuote`
- `Paragraph`
- `Heading`
- `ThematicBreak`
- `List` / `ListItem`
- `CodeBlock`
- `HTMLBlock`
- `FormulaBlock`
- `Table` / `TableRow` / `TableCell`
- `DirectiveBlock`
- `FootnoteDefinition`
- `Text`
- `SoftBreak`
- `LineBreak`
- `Code`
- `HTML`
- `Formula`
- `Emphasis`
- `Strong`
- `Strikethrough`
- `Link`
- `Image`
- `Directive`
- `FootnoteReference`

辅助值类型必须覆盖参考模型所需的概念：

- source position/range
- list flavor、start、tight 和 task checked state
- table alignment
- formula mode
- directive attributes
- link/image destination 与 title
- code block info/language、literal 和 fence closed state

`TableRow` 和 `TableCell` 是带完整 `Scope` 的 `Markup`，分别通过
`Table.header`/`Table.rows` 和 `TableRow.cells` 强类型边拥有；它们参与
Visitor dispatch 和 Walker callback，但不要求 public generic `children`。
Directive label 不引入 synthetic `DirectiveLabel` Markup；
`Directive`/`DirectiveBlock` 通过 typed、optional `label` Markup collection
保留它，并区分 label 缺失与显式空 label。标准 Walker 负责穿过这些 typed
properties，consumer 不需要检查 node kind 来发现结构。

Directive attributes 的 Markdown source grammar 遵循 generic directive attribute-list
语义：`{key=value}`、bare attributes、single/double quoted values、`#id` 和 `.class`
shortcuts 均有效。所有解析结果都是 string-to-string mapping；例如
`{id=123 muted=true title="My Video"}` 对 consumer 表示为
`{"id":"123","muted":"true","title":"My Video"}`，其中 `true` 仍是字符串。
重复普通 key 取最后一个值，重复 id 取最后一个值，class values 按出现顺序合并。

跨 C API 和各平台 binding 的表示是 optional normalized JSON string，不是源文本，
也不是跨端 `JSONValue` tree。JSON object 只包含 string key 和 string value；不得产生
number、boolean、null、nested object 或 array。consumer decode 该 JSON string 后得到
typed string map。attribute names（包括 `id`、`class` 和 `data-*`）不携带 HTML
渲染语义，core 不得将它们投影到 HTML attributes。C API setter 接受同一
string-map JSON schema，并在成功后返回规范化 JSON；失败必须保持 node 不变。

Directive、formula 和 code 节点使用统一的 `embedded`/`standalone`
placement mode；mode 属于 directive/formula/code 本身，不属于 label。
为保持 typed dispatch 和合法结构，inline 与 block concrete kinds 仍然分离。

### 10.4 不可变性

- Swift AST 对外属性必须只读，优先使用 value types、`let` 和 `Sendable`。
- Kotlin AST 必须通过 sealed types 和只读 `val` 属性表达，不得将内部 mutable collection 泄漏给用户。
- ES/TypeScript AST 必须在类型层面递归使用 `readonly`，公共 API 不得提供 mutation 入口。
- ES/TypeScript 不执行 `Object.freeze`、recursive freeze 或 Proxy-based runtime immutability；不可变性是类型/API 契约，不是 JavaScript runtime 强制约束。
- 任何平台都不得公开 setter、mutable children 或 native mutation handle。
- 任何平台都不得要求用户为 AST 手动管理 C/WASM memory。

### 10.4.1 Binding source ownership

- 三个平台的 public model 都按 AST concept 拆分。Swift value type 通过对应文件
  中的 extension 直接读取 C accessor；Kotlin serialized bridge 与 ES WASM pointer
  boundary 则分别由单一 exhaustive decoder 解释并直接构造 public model，model
  文件不依赖 private bridge。
- `Code`/`CodeBlock`、`Formula`/`FormulaBlock`、`HTML`/`HTMLBlock`、
  `Directive`/`DirectiveBlock` 必须分别落在独立文件；紧密耦合的
  `List`/`ListItem` 必须共同位于 `List` 文件，
  `FootnoteDefinition`/`FootnoteReference` 必须共同位于 `Footnote` 文件，
  `Table`/`TableRow`/`TableCell` 必须共同位于 `Table` 文件。
- 每个平台保留 exhaustive native-kind router，以便新增 kind 时产生编译或测试
  失败。Swift router 只做分派；Kotlin 和 ES decoder 同时是各自 private boundary
  的唯一 schema owner。
- Kotlin private bridge 必须按 kind 直接解码为 immutable public model；不得
  建立 `Any`、generic `WireNode`、universal field-slot record 或第二棵中间 AST；
  model 文件不得依赖 `WireReader` 或解释 bridge layout。
- ES 必须从 WASM node pointer 直接解码为 readonly discriminated-union model，
  不得建立 generic wire tree 或把 `NativeExports`/`DecodeContext` 泄漏进 model。
  stateful decoder 复用 parse-lifetime scratch memory，严格校验 raw kind、enum 和
  boolean 值。维护源码必须是单一 TypeScript source graph，runtime JavaScript 与
  recursive readonly declarations 都由 `tsc` 生成，不得手写平行的 `.js` +
  `.d.ts`；root entry 只做 public barrel。

### 10.5 `Scope` 与完整 source range

- 每个 `Markup` 必须拥有非 optional `scope: Scope`。
- `Scope` 必须包含 `start: Position` 和 `end: Position`，不只保留 start position。
- `Position` 至少包含 integer `line` 和 `column`。
- `Position` 的数值和语义均原样继承同一 Markdown Core release 的 native C parser；公共 AST 不额外定义特定坐标组合的语义。
- C facade 和三端 binding 必须直接复制坐标，不扫描 source、不修正、不展开、不拒绝特定组合，也不各自建立解释层。
- AST dump 必须对每个 node 输出 scope，且三端 conformance tests 必须比较 start/end。

### 10.6 Visitor/Walker

- Swift、Kotlin 和 ES/TypeScript 必须公开 visitor API 和 walker API。
- Visitor 必须能对每个具体 `Markup` 类型进行 typed dispatch。
- Visitor 必须 exhaustive：每个 concrete `Markup` handler 都是 required，
  不得提供 `defaultVisit`、optional handler 或 protocol/default implementation
  形式的 catch-all fallback；新增 kind 必须使未更新 visitor 编译失败。
- Walker 必须提供标准 depth-first traversal 和 entering/exiting event，不要求 consumer 自行实现每种 container node 的 children 访问规则。
- Visitor/Walker 只读 AST，不提供 replace/remove/mutate callback。
- 三端 traversal order 必须一致，并由各 package 的同构 focused conformance case 验证。
- 三端必须公开 `TreeDumper.dump(markup)` 和 `Markup.dump()`；任意 Markup 可作为
  subtree root，输出同一 canonical grammar。实现必须复用 exhaustive Visitor 与
  Walker，不调用 C dump；文本只用于 diagnostics/log/snapshot，不是持久化或传输格式。

Phase 5 冻结的完整 node/field/nullability/ownership、ParseOptions、Visitor/
Walker 契约见 `docs/specs/canonical-ast.md`；确定性 file-tree dump grammar、
字段顺序与 escaping 见 `docs/specs/canonical-ast-dump.md`。

## 11. 解析 API

### 11.1 单一入口

三个平台只公开以下概念 API：

```text
Document.parse(markdownSource, options = ParseOptions.default)
```

不对外公开：

- 第二套 parser object。
- 同义的 parse 入口。
- HTML/render API。
- C parser/node handle。

### 11.2 输入和输出

- 输入是平台原生字符串。
- Platform binding 必须以标准 UTF-8 将输入传递给 C API。
- 解析成功时返回 immutable `Document`。
- 三端在相同输入下必须生成语义等价的 AST。
- 解析或 binding 构建失败时必须显式报错，不得将失败静默替换为空 `Document`。

### 11.3 解析配置

- Consumer 可以在每次 `Document.parse` 调用时传入 immutable `ParseOptions`。
- `ParseOptions` 在三端必须具有相同字段、默认值和语义，不得存在 platform-specific parser option。
- Platform bindings 只将 typed `ParseOptions` 映射到 C facade；真正的 parser flags 和 extension attachment 必须由 C 层统一实施。
- `Document.parse(source)` 等价于 `Document.parse(source, ParseOptions.default)`，不新增第二个 parse 入口。
- `scope` 是所有 `Markup` 的必需属性，因此 source-position tracking 始终启用，不作为 consumer 可关闭选项。

`ParseOptions.default` 默认启用所有 Markdown Core 支持的通用 AST 语法：

- smart punctuation
- footnotes
- strip HTML comments
- table
- strikethrough
- autolink
- task list
- formula extension
- dollar formula delimiters
- LaTeX formula delimiters
- directive extension

Consumer 可以通过 `ParseOptions` 分别启用或关闭上述 AST-affecting 能力。任何 options 组合都必须在三端产生相同 AST。

以下历史 options 不进入新 `ParseOptions`：

- `unsafe`：只影响旧 renderer。Markdown Core AST 始终保留解析得到的 raw HTML、URL 和其他原始数据，安全策略由下游 consumer/renderer 决定。
- `github-pre-lang`：只影响旧 HTML renderer 的 `<pre>`/`<code>` 输出。
- `full-info-string`：只影响旧 HTML renderer。Markdown Core AST 的 `CodeBlock` 始终保留完整 raw info string，无需选项开关。
- 任何已删除 renderer-specific option。

### 11.4 同步解析与调度责任

- Swift、Kotlin 和 ES/TypeScript 的 `Document.parse` 都是同步 API。
- ES/TypeScript `Document.parse` 返回 `Document`，不返回 `Promise<Document>`。
- WASM 必须在 `Document.parse` 可被调用前完成初始化；可以通过 ESM initialization/top-level await、内嵌 WASM 或其他不引入第二个公共 parser API 的方式实现。
- 是否在异步 task、background queue、coroutine 或 Web Worker 中调用 parse，由 consumer 决定。
- Markdown Core 不公开 `parseAsync`、callback parse 或内部调度器。

### 11.5 必须删除的 MS 私有语法

新 Markdown Core 不支持任何 Microsoft/MS Copilot 私有 Markdown 语法。已知删除范围至少包括：

- `ms_copilot_accordion`
- `ms_copilot_annotation`
- `ms_copilot_citation`
- `CMARK_OPT_MS_COPILOT_ACCORDION`
- `CMARK_OPT_MS_COPILOT_ANNOTATION`
- `CMARK_OPT_MS_COPILOT_CITATION`
- `CMARK_OPT_MS_FORMULA_DELIMITERS`
- MS single-backslash formula delimiter scanners 及 formula parser 分支
- 对应 node kinds、accessors、setters、registration、CLI flags、headers、CMake/Make inputs、generated scanners、tests、fixtures 和文档

删除不是“默认关闭”：上述代码和公共符号必须从项目中完全移除。完成后必须使用 case-insensitive source audit 确认没有遗留的 MS-specific parser surface。

## 12. Platform package 需求

### 12.1 `swift-markdown-core`

- 通过 Swift Package Manager 构建和发布。
- SwiftPM source URL 为 `https://github.com/nouprax/markdown-core`。
- Manifest package name 为 `swift-markdown-core`；repo-derived package identity 为 `markdown-core`。
- Swift product/module 名为 `MarkdownCore`。
- SwiftPM 产物和 module 不添加 `Nouprax` 前缀。
- Swift target 依赖同 repo 内的共享 C target。
- 公共 API 只暴露 Swift AST 和 `Document.parse`。
- 不要求消费者直接 import C target。
- 需要独立 consumer package test 验证 release 的实际导入方式。
- Apple platform support policy 为仅支持最新两个已正式发布的 iOS 和 macOS major generations。
- `1.0.0` 初始 minimum deployment targets 为 iOS 18 与 macOS 15，从而覆盖 iOS 18/26 和 macOS 15/26 两个世代。
- 每年新 major OS 正式发布且 CI/toolchain 可用后，提高 minimum deployment target 以继续保持最新两个 major generations；不维护更旧 OS 的 compatibility branch。

### 12.2 `kotlin-markdown-core`

- 通过 Maven Central 发布。
- Maven Central namespace/groupId 为 `com.nouprax`，已由 `nouprax.com` DNS 所有权验证。
- Maven base artifact 名为 `kotlin-markdown-core`。
- KMP umbrella/root publication 的完整 coordinate 为 `com.nouprax:kotlin-markdown-core:<version>`。
- Kotlin package namespace 为 `com.nouprax.markdown.core`。
- 交付形式是 Kotlin Multiplatform library，不是 Android-only AAR。
- Gradle 是 Kotlin/KMP 模块的唯一 build、test、variant modeling 和 publication orchestration system；Maven Central 是承载 Maven-format publications 的 registry，不替代 Gradle build。
- 同一个 release 必须发布一组协调 artifacts：KMP root metadata publication、JVM JAR、Android AAR 和已声明 Kotlin/Native targets 的 artifacts。AAR 只服务 Android，JVM JAR 只服务 JVM，二者都不是单独的“全平台通用包”。
- Kotlin Multiplatform Gradle plugin 必须生成 root publication 与 target-specific publications。默认坐标规则应保持 plugin convention，例如 JVM target 为 `com.nouprax:kotlin-markdown-core-jvm:<version>`；Android 和 Native 的最终 target suffix 必须在 Phase 12 通过 `publishToMavenLocal` 产物审计固化并记录，不手写与 plugin metadata 不一致的坐标。
- Gradle/KMP consumer 应依赖 root coordinate，由 Gradle Module Metadata 选择合适 variant；发布不得删除或损坏 `.module` metadata。另以 repo-owned Maven Wrapper 运行真实 Maven consumer，直接通过 Maven POM 依赖和验证 JVM target-specific coordinate、effective model、dependency resolution 与 lifecycle。
- 所有 root/target publications 必须使用相同 group、base artifact identity、version、license、SCM 和 release provenance，并在 Maven Central 同一次 deployment 中完整出现；不得只发布 root metadata 而遗漏其引用的 target artifacts。
- AST types、`ParseOptions`、`Document.parse`、Visitor 和 Walker 必须在 `commonMain` 公开。
- 第一版必须包含 Android target 和通用 JVM target；JVM 产物必须为所支持的 desktop OS/architecture 打包可加载的 native library。
- Kotlin/Native targets 属于 KMP 交付范围，通过 cinterop 直接调用同一 C facade；精确 target/architecture matrix 必须在实现计划中与 CI 矩阵一起固化。
- Kotlin/JS 和 Kotlin/Wasm 不是 Kotlin package 的第一版必需 target；Web/Node 交付由 `@nouprax/es-markdown-core` 覆盖。
- Kotlin 公共 API 只暴露 Kotlin AST 和 `Document.parse`。
- Native C 交互细节不得出现在公共 API。
- Kotlin 到 C 的字符编码路径必须保持标准 UTF-8。
- Android/JNI 实现不得将 JNI modified UTF-8 当作 Markdown 源文本的 UTF-8 表示。
- 需要独立 consumer test 验证发布 artifact 及 native library packaging。
- Consumer tests 至少包含 KMP Gradle、使用 Gradle Module Metadata 的 JVM Gradle、通过 repo-owned Maven Wrapper 运行的真实 JVM Maven，以及 Android Gradle/AAR 四种消费方式，并验证 native library 的 OS/architecture/ABI variant 选择。
- Android AAR 必须在 `jni/<abi>/`/等价标准位置携带所支持 ABI 的 native libraries。通用 JVM publication 必须提供稳定的 OS/architecture runtime selection：可以由主 JVM JAR 依赖带 Gradle attributes 的 platform runtime artifacts，也可以在有充分体积依据时打包全部 supported natives；最终方案必须同时验证 Gradle Module Metadata 与真实 Maven resolution/lifecycle，并验证 unsupported platform 的明确错误。Maven consumer 通过固定版本和 distribution checksum 的 Maven Wrapper 执行，不要求开发机预装全局 `mvn`。

### 12.3 `@nouprax/es-markdown-core`

- 通过 npm 发布。
- 发布到已建立的 npm organization `nouprax` 所持有的 `@nouprax` scope。
- package 对外提供 ESM JavaScript、TypeScript declarations 和可执行 WebAssembly。
- Markdown Core C engine 和只读 C facade 必须由 WebAssembly 执行。
- 公共 API 只暴露 ES/TypeScript AST 和 `Document.parse`。
- `Document.parse` 是同步函数，返回 `Document` 而非 `Promise<Document>`；并发与 worker 调度由 consumer 负责。
- TypeScript AST 使用递归 `readonly` 类型，但不调用 `Object.freeze` 或其他 runtime freeze。
- 不得暴露 WASM pointer、Emscripten runtime 或内部初始化对象。
- 不得用 JSON 在 WASM 和 JavaScript 之间传递 AST。
- 需要 consumer test 验证 package exports、types、Node 环境和目标 browser 环境。

## 13. Monorepo setup

### 13.1 目标结构

新 repo 必须将 C engine 与三个 platform packages 收入统一的 `packages/` 边界。目标结构为：

```text
markdown-core/
├── .editorconfig
├── .gitattributes
├── .gitignore
├── .clang-format
├── .swift-format
├── .swiftlint.yml
├── CMakeLists.txt
├── Package.swift
├── eslint.config.js
├── package.json
├── pnpm-workspace.yaml
├── prettier.config.mjs
├── .prettierignore
├── settings.gradle.kts
├── build.gradle.kts
├── gradle/
│
├── packages/
│   ├── markdown-core/
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── markdown_core.h
│   │   ├── core/
│   │   ├── extensions/
│   │   ├── tests/
│   │   │   ├── core/
│   │   │   ├── extensions/
│   │   │   ├── api/
│   │   │   ├── fixtures/
│   │   │   ├── canonical-ast/
│   │   │   ├── corpora/
│   │   │   └── runners/
│   │   ├── benchmarks/
│   │   └── fuzz/
│   ├── swift-markdown-core/
│   │   ├── Benchmarks/
│   │   ├── Sources/
│   │   └── Tests/
│   ├── kotlin-markdown-core/
│   │   ├── android-runtime/
│   │   ├── consumers/
│   │   ├── contracts/
│   │   └── src/
│   └── es-markdown-core/
│       ├── src/
│       ├── tests/
│       └── scripts/
├── samples/
├── scripts/
├── docs/
├── LICENSE
└── UPSTREAM.md
```

路径约束：

- 现有根级 `src/` 必须迁移为 `packages/markdown-core/core/`。
- 现有根级 `extensions/` 必须迁移为 `packages/markdown-core/extensions/`。
- 现有根级 `test/` 中属于 C engine 的测试必须收入 `packages/markdown-core/tests/`。
- 现有 `api_test/` 必须并入 `packages/markdown-core/tests/api/`。
- 现有 `bench/` 必须迁移并重命名为 `packages/markdown-core/benchmarks/`。
- 现有 `fuzz/` 必须迁移为 `packages/markdown-core/fuzz/`。
- C parser correctness fixtures 由 `packages/markdown-core/tests/` 独占；跨产品
  canonical goldens 与 manifest 仅位于 `specs/canonical-ast/`，且不包含 runner。
  各 binding 的 focused API/correctness cases、consumer tests 和 test runners 仍
  位于各自 package，不得在根级重建共享 `tests/`。
- 新稳定 C facade header 必须位于 `packages/markdown-core/include/markdown_core.h`。
- 历史 C 内部 headers 不得因目录迁移而全部变成 public headers。
- 根目录 CMake 负责编排；C engine 的 target 定义必须由 `packages/markdown-core/CMakeLists.txt` 拥有。

目录迁移必须使用 Git-aware move 保留可追踪历史，并且与 parser 行为修改分开提交。

### 13.2 平台边界

- JNI wrapper 属于 `packages/kotlin-markdown-core/`，不得进入 portable C engine。
- Emscripten/WASM glue 和生成脚本属于 `packages/es-markdown-core/`。
- Swift AST adapter 属于 `packages/swift-markdown-core/`。
- `packages/markdown-core/` 必须保持可独立由 CMake 构建和测试，不得反向依赖 Swift、Kotlin、Node 或 Emscripten-specific glue。

### 13.3 现有包装层

原 `cmark-gfm` repo 中现有的 SwiftPM 和 Android/AAR setup 不是新项目的公共设计基础：

- setup 时可参考其 C source/build integration，但不得直接延续旧 package API、坐标或版本。
- 新 SwiftPM 产物必须为 `swift-markdown-core` / `MarkdownCore`。
- 新 Kotlin 产物必须为 `kotlin-markdown-core`。
- 当新 setup 完成并被消费者测试覆盖后，旧 setup 可删除。

### 13.4 根级工具链

根级命令至少需覆盖：

- C engine/facade build and test
- Swift build/test/consumer test
- Kotlin build/test/consumer test
- ES/WASM build/typecheck/test/consumer test
- 统一 format 和 format check
- 共享 AST fixture/conformance test
- 可一次运行全部验证的根级命令

### 13.5 Repo hygiene、格式化与 lint

Repo setup 必须在任何平台实现开始前建立并提交统一的 hygiene/tooling 配置。配置以各语言官方或主流推荐规则为基线，只覆盖 monorepo 需要明确统一的部分。

#### 13.5.1 通用文本规则

- 根目录必须提供 `root = true` 的 `.editorconfig`，至少设置 UTF-8、LF、final newline、trailing-whitespace policy、`indent_style = space`、`indent_size = 4` 和 `tab_width = 4`。
- 所有人手维护的 C/C++、Swift、Kotlin/Kotlin Script、JavaScript/TypeScript、JSON、YAML、CMake 和 shell 文件一律使用 spaces，基础 indentation width 为 4。
- `Makefile` recipe 是语法要求下的唯一常规 tab 例外；`.editorconfig` 必须为 `Makefile`/`*.mk` 单独声明 tab，并将显示宽度设为 4。不得为了“全仓无 tab”破坏 Make 语法。
- Markdown 若需要保留 hard line break，可关闭 trailing whitespace trimming；该例外必须局限于 `*.md`。
- Generated、vendored、golden、binary 和 package-manager cache 文件不得被 formatter 改写；所有 formatter/linter 的 exclusions 必须一致并有明确路径，不允许用宽泛规则跳过手写 source tree。
- 根目录必须提供 `.gitattributes`，为文本规范 LF，并显式标记常见 binary artifacts，避免 checkout 平台改变 golden、fixture 或 AST dump bytes。

#### 13.5.2 Ignore 与发布内容控制

- 根 `.gitignore` 必须覆盖 CMake/build directories、SwiftPM/Xcode derived data、Gradle/Kotlin/Android native build state、Node/pnpm caches、WASM generated build output、coverage/test artifacts、IDE metadata、OS metadata、`local.properties` 和本地 secret/env files。
- `.gitignore` 不得忽略源代码、测试 fixtures、reviewed AST goldens、Gradle wrapper、工具版本配置或用于 reproducible CI 的 lockfiles。
- `.env.example` 等无 secret 的模板可以纳入版本控制；真实 token、signing key、`.env` 和 machine-local registry credentials 必须忽略并由 secret scanning/CI policy 防止误提交。
- npm 发布内容优先由各 `package.json` 的 `files` allowlist 和 `exports` 控制，并通过 `npm pack --dry-run`/等价 consumer test 审计；不得依赖容易意外扩大包内容的 `.npmignore` 作为唯一边界。
- Maven、SwiftPM、C install 和 GitHub Release artifacts 同样必须使用 allowlist/target ownership 思路，并在 release dry run 中审计文件清单。

#### 13.5.3 Swift

- Swift formatter 使用 Swift toolchain 自带的 Apple/Swift project `swift format`，根目录提交 `.swift-format`。
- `.swift-format` 从所固定 Swift toolchain 的默认/recommended configuration 生成，只做有记录的项目覆盖；`indentation.spaces` 和 `tabWidth` 必须为 4。
- 提供 mutating format 命令与 non-mutating check：`swift format --recursive --in-place ...` 和 `swift format lint --recursive ...`，覆盖 `Package.swift`、Swift sources、tests 和 consumer fixtures。
- SwiftLint 使用根 `.swiftlint.yml`，以其默认 community rules 为起点，仅启用明确适合 library API 的 opt-in rules。
- `swift format` 是 layout/whitespace 的唯一 source of truth；与 formatter 重叠或冲突的 SwiftLint style rules 必须关闭，SwiftLint 主要负责 formatter 不覆盖的 correctness、API hygiene 和 convention checks。
- SwiftLint 及 Swift toolchain 版本必须在 CI/tool version policy 中固定；lint tooling 不得成为发布后 consumer 的 runtime dependency。
- 新 repo 不建立 SwiftLint baseline，不允许全局 `disabled_rules` 掩盖初始违规；必要例外必须按 rule 和最小代码范围记录原因。

#### 13.5.4 Kotlin

- Kotlin style 以 JetBrains Kotlin Coding Conventions 为语义基线：使用 spaces、4-space indentation，不使用 tab。
- Kotlin formatting 与 style lint 统一使用一个固定版本的 `ktlint`，通过支持 Kotlin Multiplatform 的 Gradle plugin 接入；不再叠加第二个会改写同一 Kotlin source 的 formatter。
- `.editorconfig` 的 `[*.{kt,kts}]` 必须设置 `ktlint_code_style = ktlint_official`、`indent_style = space`、`indent_size = 4` 和 `tab_width = 4`。若 ktlint rule 与 Kotlin 官方 convention 冲突，必须记录窄范围例外并以 Kotlin 官方 convention 为准。
- Gradle 必须提供 `ktlintFormat` 和 `ktlintCheck`，覆盖所有 KMP source sets、Gradle Kotlin scripts、tests 和 consumer fixtures。
- ktlint/Gradle plugin 版本必须由 version catalog 或等价单一位置固定；新 repo 不建立 lint baseline，也不默认启用 experimental rules。

#### 13.5.5 ECMAScript/TypeScript 与 C

- ES/TypeScript formatting 使用固定版本 Prettier，设置 `useTabs: false`、`tabWidth: 4`；lint 使用与当前 TypeScript/ESM setup 相容的 ESLint recommended/type-aware configuration。
- Prettier 负责 layout，ESLint 不得重复承担相冲突的 formatting rules；必须提供 `format`/`format:check` 与 `lint:es` 命令。
- C/C++ formatting 使用 repo 固定的 `.clang-format`，从 LLVM style 或项目确认的主流基线开始，至少覆盖 `UseTab: Never`、`IndentWidth: 4`、`TabWidth: 4`。
- C lint 至少包含项目支持编译器的 warnings-as-errors configuration；可复现的 `clang-tidy` 检查应在 target/toolchain 可用时纳入，但不得以平台相关误报阻断未支持的 toolchain。
- Generated scanners、vendored code 和机械生成的 WASM/JNI glue 只能在明确路径下排除；手写 C facade、JNI、cinterop 和 WASM glue 必须接受对应 formatter/linter。

#### 13.5.6 根命令与 CI

- 根级必须提供统一的 `format`、`format:check` 和 `lint` 命令，并提供 `format:swift`、`format:kotlin`、`format:es`、`format:c` 及对应 check/lint 子命令便于局部运行。
- `format` 可以修改工作树；`format:check` 和 `lint` 必须只读且在不合规时 non-zero exit。
- CI 只运行 non-mutating checks，不得自动格式化后继续成功，也不得自动提交修复。
- Formatter、linter、compiler/toolchain 和 package-manager versions 必须固定或由 lock/version catalog 管理；升级必须作为可评审变更并伴随全仓 format diff。
- 每种语言只能有一个 layout formatter。Lint 工具必须与 formatter 分工，避免开发机、IDE、Gradle、SwiftPM 和 CI 产生互相改写的循环。
- 对继承 C engine 的首次四空格格式迁移必须在目录 Git-aware moves 完成后，以独立、纯机械 commit 执行；不得与目录移动、symbol rename、parser behavior 修改或 renderer 删除混合，以保留可审查历史。

### 13.6 Gradle/KMP 工程规范

#### 13.6.1 版本与构建模型

- Gradle build scripts 必须使用 Kotlin DSL：`settings.gradle.kts`、`build.gradle.kts` 和必要的 convention plugins。
- 必须提交 Gradle Wrapper 的 scripts、JAR 和 properties，固定一个 stable Gradle release，并设置官方 distribution SHA-256。所有本地、IDE 和 CI 命令都通过 `./gradlew`，不依赖机器全局安装的 Gradle。
- 版本选择遵循“最新 stable 且在 Kotlin Multiplatform/Kotlin/AGP/JDK compatibility matrix 中相互支持”，不得分别追逐 Gradle、Kotlin 和 AGP 的绝对最新版本而形成未经支持的组合。
- `gradle/libs.versions.toml` version catalog 是 plugins、Kotlin、AGP、ktlint、publishing plugin 和 Kotlin/JVM dependencies 的单一版本入口。这里使用的 Gradle 术语是 version catalog，不是 version category。
- Version catalog 只集中声明 requested versions，不替代 dependency constraints/locking。Build dependencies 必须避免 dynamic versions 和 changing/SNAPSHOT dependencies；对需要解析稳定性的 configuration 启用并提交 dependency locking。
- 必须提交 `gradle/verification-metadata.xml`，对 plugins 和 dependencies 启用 checksum/signature verification；新增或升级 dependency 时对 verification metadata diff 做人工审查。
- `pluginManagement` 和 `dependencyResolutionManagement` 必须在 `settings.gradle.kts` 集中管理 repositories，并禁止 subproject 任意增加未经审查的 repository。
- 多模块重复逻辑应放入 typed convention plugin/included `build-logic`，不使用大量 `subprojects {}` mutation、`afterEvaluate` 或 configuration-time side effects。

#### 13.6.2 Toolchains 与现代 Gradle 能力

- 必须分别固定 Gradle daemon JVM、Java/Kotlin compilation toolchain 和产物 bytecode target；三者不得隐式取决于开发机当前 `JAVA_HOME`。
- JVM/KMP targets 使用 Gradle/Kotlin toolchain API（包括 `jvmToolchain`/typed compiler options）；Android compile/target/min SDK 和 native ABI matrix 通过 version catalog 或 typed build constants 集中声明。
- Toolchain auto-provisioning 若启用，必须声明受信任的 download repository；CI 和 IDE 使用相同 toolchain resolution policy。
- Build 必须以 configuration cache compatibility 为目标，并在 CI 对主要 build/test/lint/publication-dry-run tasks 运行 configuration-cache check。不得为了启用 cache 隐藏错误；若上游 plugin 暂不兼容，必须记录精确 task/plugin 和移除豁免的条件。
- 启用 local/CI build cache 和 lazy task registration，所有 custom tasks 必须声明 inputs、outputs 和 environment-sensitive properties；不得在 configuration phase 编译 C、探测不可追踪的本机状态或读取 release secrets。
- Incubating Gradle features（例如尚未被所选 Kotlin/AGP plugins 支持的 isolation mode）只能在兼容性验证后启用；“充分利用最新特性”不等于把不稳定 feature flag 直接放入 release build。

#### 13.6.3 IDE import/sync

- Clean checkout 必须能从 repo 根目录直接以 Gradle project 导入当前受支持的 IntelliJ IDEA 和 Android Studio stable versions；不得要求提交 `.idea`、生成 `.iml`、预先运行自定义脚本或手工修改 build files。
- 不需要应用 legacy `idea` plugin 来生成 IDE project files；现代 IDE 使用 Gradle/KMP model import。只有确有无法由标准 model 表达的设置时才允许加入窄范围 IDEA DSL，并须说明原因。
- Gradle sync 在没有 Maven Central credentials、PGP key、release environment 或已构建 native binaries时必须成功；signing、upload 和昂贵 native packaging 只能在对应 execution task 被请求时配置/执行。
- IDE 必须正确显示 `commonMain/commonTest`、Android、JVM 和声明的 Native source sets，不产生重复 content roots、错误 generated-source roots 或 unresolved expect/actual declarations。
- 必须记录支持的 IntelliJ IDEA、Android Studio、JDK、Gradle、Kotlin、KMP plugin 和 AGP compatibility matrix；升级任一项时同时运行 IDE sync/import 回归。
- CI 必须包含 Gradle Tooling API/model-load 或等价的 headless import smoke test、`./gradlew projects`、KMP tooling metadata 和各 target compile tests。Release checklist 还必须在受支持 IntelliJ IDEA 与 Android Studio 各完成一次 clean import/sync smoke test。
- IDE import 不得依赖 machine-local `local.properties` 以外的隐式状态；Android SDK path 可由标准 IDE/`ANDROID_HOME` 提供，其他必需条件必须在 setup 文档中明确。

#### 13.6.4 KMP publication

- 使用 `org.jetbrains.kotlin.multiplatform`、Android KMP library plugin/DSL、`maven-publish`/signing 或当前 Kotlin 官方流程支持的固定版本 publishing plugin，不能用手写 HTTP upload 代替 Gradle publications。
- Android target 必须显式启用 library publication 并生成 AAR；JVM target 生成 JAR；Native targets 生成对应 KLIB/native artifacts；root `kotlinMultiplatform` publication 保留完整 target references 和 Gradle Module Metadata。
- Publication 必须先发布到隔离的 local Maven repository，审计 POM、`.module`、JAR、AAR、KLIB、sources/docs、signatures、checksums、coordinates、variants、native payload 和 root-to-target references，再允许访问 Maven Central。
- 同一 release 的完整 publications 由单一受控 publish job/host 上传，避免多个 host 重复发布同一 coordinate。可使用 matrix jobs 构建和验证 artifacts，但最终 staging/deployment 必须聚合并检查 completeness。
- 发布验证必须证明 root coordinate 可由 Gradle/KMP/Android consumers 正确选择 variant，target-specific JVM coordinate 可由 repo-owned Maven Wrapper 驱动的真实 Maven consumer 仅通过 Maven repository/POM 使用，且所有 consumer 都获得与其 OS/architecture/ABI 匹配的 native library。真实 Maven consumer 是所承诺 Maven effective-model、resolution 和 lifecycle compatibility 的最终 smoke test。

## 14. 跨平台一致性

三端必须对以下内容保持一致：

- 节点 kind 及其命名。
- parent/child 层级和 children 顺序。
- optional 字段的 nullability。
- 字符串内容和 Unicode 语义。
- list start/tight/task state。
- heading level。
- code block info/language/literal/fence state。
- link、image 和 title。
- table header/body/alignment。
- formula、directive 和 footnote 节点。
- source position/range 的坐标含义。
- syntax extensions 和 parser options。
- parse failure behavior。

一致性测试必须覆盖同一组公开行为；四端 conformance target 枚举
`specs/canonical-ast/manifest.json` 的同一组 cases，不建立平台副本或第二份清单。

Root shared spec 独占完整 canonical file-tree `.ast` golden corpus，不再定义第二套
JSON 或其他 tree schema。Swift/Kotlin/ES 必须各自公开
`TreeDumper.dump(markup)` 与 `Markup.dump()`，独立遍历本层 immutable AST，并与
shared `.ast` byte-for-byte 比较；不得调用 C dump、读取另一 binding 输出，或使用
dump text 构建生产 AST。

## 15. 质量与安全性要求

- 公共 C 头文件必须能在 C 和 C++ consumer 中安全 include。
- 所有 C/WASM 字符串边界必须使用明确长度，不依赖输入 NUL termination。
- 必须测试 ASCII、CJK、emoji、supplementary Unicode scalars、combining marks 和 embedded NUL 边界。
- 必须测试空输入、大文档、深度嵌套和 malformed Markdown。
- 必须测试 parse success、parse failure 和 platform object construction failure 路径的资源释放。
- 必须为 C engine/facade 保留 sanitizer-compatible test path。
- AST dump 必须在相同输入、options 和平台时产生 byte-for-byte 稳定的规范化输出。
- Spec tests、extension spec tests、fuzz tests 和原本依赖 render output 的 parser/API tests 必须改为验证 AST dump 或直接 AST accessors。
- 纯 renderer-specific tests 在 renderer 删除后必须删除，不得为了保留旧 expected output 而继续保留 renderer。
- 不得使用用户可见的 undefined behavior 换取 zero-copy。
- 初始版本应优先选择清晰所有权与语义一致性，然后再通过 benchmark 决定是否需要更复杂的跨边界优化。

## 16. 验收标准

初始版本在满足以下条件时可验收：

1. 项目已使用 Markdown Core 新身份，并记录了 fork 来源与许可证信息。
2. C engine、C facade 和三个 platform packages 位于同一 monorepo。
3. `swift-markdown-core`、`kotlin-markdown-core` 和 `@nouprax/es-markdown-core` 都能从 clean checkout 完成构建。
4. 三端都只通过 `Document.parse` 公开 Markdown 解析。
5. 三端解析同一组 CommonMark/GFM/custom-extension fixtures 时得到等价 AST。
6. 生产解析路径不包含 AST JSON 序列化或反序列化。
7. 新公共 C API 只使用 `markdown_core_*` 命名并且不提供 AST mutation。
8. 三端公共 AST API 不提供 mutation。
9. 三端公共 API 不提供 HTML 或其他 renderer。
10. Unicode 在三端不发生损坏。
11. 反复 parse/release 和所有已知失败路径没有明显内存泄漏或悬空指针。
12. SwiftPM、Maven/Gradle 和 npm 各有独立 consumer-level test。
13. 根级全量验证命令能检查 C、Swift、Kotlin、ES/WASM 和 conformance tests。
14. 项目版本、三端 package 版本与 C engine/facade source 使用协调的统一 release；这不构成跨 release binary ABI 承诺。
15. C engine 已位于 `packages/markdown-core/{core,extensions,tests}` 边界内，旧根级 `src/`、`extensions/`、`test/` 不再是生产构建输入。
16. MS Copilot accordion、annotation、citation 和 MS formula delimiters 的源码、options、symbols、CLI flags、tests 和文档已完全删除。
17. C 层提供独立于 renderer 的原生、确定性 AST dump API 和 test CLI 入口。
18. CommonMark spec tests、extension spec tests 及其他 parser tests 不再使用 HTML/XML/CommonMark 等 render output 作为 expected result，而是验证 AST dump 或直接 AST accessors。
19. HTML、XML、CommonMark、LaTeX、man 和 plaintext renderer 的实现、公共函数、extension callbacks、CLI formats、构建输入和专属测试已删除。
20. 除 `LICENSE`/`COPYING`/`THIRD_PARTY_NOTICES`、README/UPSTREAM 来源声明、明确的历史迁移文档和 Git history 外，case-insensitive 全仓库/产物审计不存在 `cmark` 命名。
21. `README.md` 和 `UPSTREAM.md` 明确说明项目继承自 cmark/cmark-gfm、已重写部分代码，并完整保留适用 license/copyright notices。
22. SwiftPM 通过 `https://github.com/nouprax/markdown-core` 发布，导出 `MarkdownCore` product/module，不添加 `Nouprax` module 前缀。
23. Maven Central 已完成 `nouprax.com` DNS 所有权验证，并以 `com.nouprax:kotlin-markdown-core:<version>` 发布 KMP root publication 及其引用的完整 target-specific publications。
24. npm package 由 npm organization `nouprax` 以 public `@nouprax/es-markdown-core` package 发布。
25. npm 已配置绑定 `nouprax/markdown-core` 精确 workflow/environment 的 OIDC trusted publisher，不存在未使用的长期 publish token。
26. Maven Central Portal token 与 PGP signing secrets 只存在受保护 `release` environment，其权限、到期、转换和撤销流程已验证。
27. SwiftPM/GitHub Release 使用 SemVer tag 与最小权限 `GITHUB_TOKEN`，不依赖无过期时间的 PAT。
28. `docs/releasing.md` 完整记录发布步骤、secret names、权限、bootstrap、dry run、rotation、revocation 和泄漏响应，不包含任何 secret value。
29. 首次正式版本为 `1.0.0`，新 repo 未迁移、保留或归档旧 tags。
30. Swift `1.0.0` 支持 iOS 18/26 与 macOS 15/26，并有对应 CI；后续按“仅最新两个 major generations”滚动提高 minimum deployment targets。
31. Kotlin 以 KMP library 交付，公共 API 位于 `commonMain`；首版至少验证 Android、通用 JVM 与已声明的 Kotlin/Native target matrix，而非仅交付 Android AAR。
32. 三端 `Document.parse` 均为同步 API；ES 在调用前已完成 WASM 初始化，且不公开 `parseAsync` 或第二个 parser 入口。
33. Kotlin 保留 AST 类型名 `List`，并通过全限定名/import alias 的 consumer 编译测试证明可与 `kotlin.collections.List` 共存。
34. 每个 `Markup` 都公开非 optional `Scope(start, end)`，其数值和语义原样继承 native C parser；AST dump 与三端 conformance tests 均验证坐标未被 binding 改写。
35. 三端公开语义一致的 `ParseOptions`、typed Visitor 和带 entering/exiting event 的只读 depth-first Walker；默认 options 与逐项关闭行为均由各 package 的 focused cases 覆盖。
36. TypeScript AST 递归使用 `readonly`，运行时不调用 `Object.freeze`、recursive freeze 或 Proxy 强制不可变。
37. C facade 的 length-delimited UTF-8 string view、显式 error/free model 与 clean-rebuild ABI policy 已由 C/C++ consumer 和失败路径测试覆盖。
38. Canonical AST dump 使用文档规定的 UTF-8 file-tree 结构和 connectors，不包含 property/index edge label；每个节点占一行并包含完整 behavior-bearing fields、固定字段顺序、严格 scope、child count、escaping、LF 和 final newline。四端对 root shared manifest/goldens 的 byte-for-byte diff 能定位公共 AST behavior drift；三端公开 `TreeDumper.dump(markup)` 和便利的 `Markup.dump()`，但不将文本定义为序列化 API。
39. Repo 根目录提供并验证 `.editorconfig`、`.gitattributes`、`.gitignore`、`.clang-format`、`.swift-format`、`.swiftlint.yml`、Prettier 和 ESLint 配置；所有手写语言默认使用 spaces 与 4-space indentation，只有 Make recipe 等语法必需位置使用 tab width 4。
40. Swift 通过 `swift format` 与 SwiftLint 分工；Kotlin 通过单一 ktlint 同时提供 format/check；ES 通过 Prettier/ESLint 分工；C 通过 clang-format 与 compiler lint。各工具版本固定且不存在两个 formatter 争用同一 source。
41. 根级 `format:check` 和 `lint` 在 clean checkout 只读通过，CI 不修改文件；ignore/package allowlists 经 consumer/release dry run 证明既不遗漏必需产物，也不包含 cache、local state 或 secrets。
42. Kotlin 使用 Gradle/KMP 构建并向 Maven Central 发布完整的 root/target publication set；root `.module` metadata、JVM JAR、Android AAR 和声明的 Native artifacts 相互引用正确、版本一致且没有缺失 publication。
43. Clean checkout 可被声明支持的 IntelliJ IDEA 和 Android Studio 从根目录直接导入并完成 Gradle sync，不需要 release secrets、已生成 `.idea`/`.iml` 或预先执行 native build。
44. Gradle Wrapper/checksum、version catalog、JVM/toolchain policy、dependency verification/locking、configuration-cache checks 和 centralized repositories 均已启用并由 CI 验证；采用的是最新 stable compatible matrix，而不是未经支持的独立最新版组合。
45. KMP Gradle、使用 Gradle Module Metadata 的 JVM Gradle、真实 JVM Maven 和 Android Gradle consumer tests 均能从 local/staged Maven repository 解析正确 publication/variant 并加载目标 native library；Maven 由 repo-owned Wrapper 固定并启动。

## 17. Task 计划

以下计划是 setup 后新 session 的默认实施顺序。每个 phase 都使用 flat `Tasks` 与 `Acceptance` lists，每个 task 和 acceptance criterion 各占一个物理行，不使用续行或嵌套 list。任何阶段都必须在 Acceptance list 满足后才能继续依赖它的后续阶段。其中 Phase 6–9 的顺序是强制的：必须先建立 AST dump，再冻结全仓统一测试架构并完成 CTest 侧迁移、迁移 parser assertions，最后删除 renderer。

### Phase 0：建立新项目基线

Tasks：

- [x] 建立 `nouprax/markdown-core` repo 和新 release lineage。
- [x] 以 commit `711032b2a16cf25c3df75033833eba086b17ca6a` 为 C engine 基线。
- [x] 不迁移或保留任何旧 tags，确保新 repo 的首个版本为 `1.0.0`。
- [x] 保留 license/copyright notices。
- [x] 创建 `UPSTREAM.md`，记录 fork 来源、基线 commit 和 diverge 决策。
- [x] 记录迁移前 CMake、Make、tests、sanitizers 和 benchmarks 的可运行基线。
- [x] 记录当前 MS-specific source surface、render functions、render-dependent tests 和 CLI output formats，作为后续删除 checklist。

验证命令、结果和删除 inventory 见 `docs/migration/2026-07-11-phase-0-baseline.md`。

Acceptance：

- [x] 新 repo 在未改变 parser 行为的情况下能执行已记录的 C 基线验证。
- [x] MS-specific 与 renderer 待清理清单完整且可追踪。

### Phase 1：纯目录与构建重构

Tasks：

- [x] 建立 `packages/markdown-core/`。
- [x] 使用 Git-aware moves 将 `src/` 迁移到 `packages/markdown-core/core/`。
- [x] 将 `extensions/` 迁移到 `packages/markdown-core/extensions/`。
- [x] 将 C tests、`api_test/`、`bench/` 和 `fuzz/` 迁移到新归属。
- [x] 更新 CMake、Make、CI、scripts 和 include paths。
- [x] 建立根 CMake 编排与 `packages/markdown-core/CMakeLists.txt` target ownership。
- [x] 创建 `.editorconfig`、`.gitattributes` 和覆盖所有工具链/local state/secrets 的 `.gitignore`，统一 spaces 与 4-space indentation，并保留 Make recipe tab 例外。
- [x] 创建 `.clang-format`、`.swift-format`、`.swiftlint.yml`、Prettier 和 ESLint 配置，固定对应 tool versions。
- [x] 通过 Gradle plugin 接入固定版本 ktlint，设置 `ktlint_official` 与 4-space indentation，提供 `ktlintFormat`/`ktlintCheck`。
- [x] 建立 Gradle Wrapper 并固定 distribution checksum；选择并记录 Gradle/Kotlin/KMP/AGP/JDK 的最新 stable compatible matrix。
- [x] 创建 `gradle/libs.versions.toml`、centralized repositories、dependency locking 和 `gradle/verification-metadata.xml`。
- [x] 配置 daemon/compile toolchains、configuration cache、build cache 与 lazy/custom task inputs/outputs policy。
- [x] 添加不读取 release secrets 的 Gradle Tooling API/model-load headless import smoke test，并记录 IntelliJ IDEA/Android Studio clean-sync checklist。
- [x] 建立根级 `format`、`format:check`、`lint` 及各语言子命令，并把 non-mutating checks 接入 CI。
- [x] 为 generated/vendored/golden files 建立各工具一致的窄范围 exclusions，并验证手写 source 没有被意外排除。
- [x] 为 npm package 建立 `files`/`exports` allowlist，并为其他 release artifacts 建立等价内容审计。
- [x] 在目录迁移 commit 之后，以独立纯格式化 commit 将继承的手写 C/C++ source 迁移为 clang-format 四空格风格。
- [x] 保持 parser behavior、public behavior 和 test fixtures 不变。

Acceptance：

- [x] 迁移前记录的 C build、tests 与 sanitizers 全部恢复通过。
- [x] 根级 format checks 和 lint 在 clean checkout 只读通过。
- [x] 所有手写 source 使用约定的 4-space style，ignore 与 allowlist 审计通过。
- [x] 本阶段不包含有意 parser 语义变更。

实施提交、补充审计、Tooling API model-load 证明和验证结果见
`docs/migration/2026-07-11-phase-1-validation.md`。

### Phase 2：删除 MS 私有 Markdown extensions

Tasks：

- [x] 删除 `ms_copilot_accordion` source、headers、node kinds、registration 和 accessors。
- [x] 删除 `ms_copilot_annotation` source、headers、node kinds、registration 和 accessors。
- [x] 删除 `ms_copilot_citation` source、headers、node kinds、registration 和 accessors。
- [x] 删除 `CMARK_OPT_MS_COPILOT_*` options 及 CLI flags。
- [x] 删除 `CMARK_OPT_MS_FORMULA_DELIMITERS`、MS formula parser branches 和 single-backslash scanners。
- [x] 重生成 scanner artifacts，并从 CMake、Make、fuzz configuration 中删除 MS inputs。
- [x] 删除所有 MS-specific tests、fixtures、scripts 和文档。
- [x] 使用 case-insensitive source audit 查找 `ms_copilot`、`CMARK_OPT_MS_`、`ms-formula` 和其他已知标识符。

Acceptance：

- [x] C build、tests 与 sanitizers 全部通过。
- [x] 已知 MS-specific parser surface 从源码、构建、CLI、导出 symbols、tests 和文档中完全消失。

删除 inventory、man page 同步修复、source/install/AAR 审计和验证结果见
`docs/migration/2026-07-11-phase-2-ms-removal.md`。

### Phase 3：全仓库移除 cmark 命名

Tasks：

- [x] 建立 case-insensitive 命名 inventory，覆盖 filenames、directories、symbols、types、macros、include guards、targets、libraries、CLI、tests、scripts、generated files 和 package metadata。
- [x] 将 `cmark_*` functions/types/globals 重命名为 `markdown_core_*` 或文档化的内部前缀。
- [x] 将 `CMARK_*` macros/enums/include guards 重命名为 `MARKDOWN_CORE_*` 或文档化的内部前缀。
- [x] 重命名含 `cmark`/`cmark-gfm` 的源文件、headers、CMake targets、library/pkg-config names、CLI binary、tests 和 scripts。
- [x] 更新 CMake、Make、CI、include paths、generated files、export maps 和 install/package metadata。
- [x] 删除旧 ABI、library、CLI 和 package names 的 compatibility aliases，不保留双重品牌。
- [x] 保留并核对原 cmark/cmark-gfm license/copyright notices。
- [x] 在 `README.md` 和 `UPSTREAM.md` 加入“继承自 cmark/cmark-gfm，并在此基础上重写了部分代码”的明确声明。
- [x] 对工作树、安装产物、静态/动态库 symbol table、CLI help 和 package archives 运行 case-insensitive `cmark` audit。
- [x] 审查 audit 中的每个剩余命中，只允许许可证、归属、来源/迁移文档或 Git history 例外。

Acceptance：

- [x] 所有 C build、tests 与 sanitizers 全部通过。
- [x] 产物、源码实现和操作文档中不存在 cmark 命名。
- [x] 唯一剩余命中位于许可证、copyright、README/UPSTREAM 来源声明、明确迁移历史或 Git history 中。

验证命令、命名映射、初始 inventory 和剩余例外见
`docs/migration/2026-07-11-phase-3-rebrand.md`。

### Phase 4：修正 directive attribute-list → string-map JSON 语义

Tasks：

- [x] 建立 directive standard behavior inventory，覆盖 bare/key-value attributes、single/double quotes、`#id`、`.class`、duplicate rules、C API、render callbacks、fixtures 和 packaged headers。
- [x] 冻结 source attribute-list → normalized string-map JSON contract，并明确 absent/`{}`、ownership 和 deterministic serialization 规则。
- [x] 冻结 inline、leaf block、container block 在 invalid/truncated attribute list 下的 reviewed fallback fixtures。
- [x] 定义 non-recursive attribute-list scanner/parser 的 length、overflow、transactional failure 和 leak-free resource-safety contract。
- [x] 使用 typed `directive_attribute` payload 保存解析结果，并维护 normalized JSON/XML cache，不保留 Markdown attribute-list 原始文本。
- [x] 实现 bare、unquoted、single/double quoted values、`#id`、`.class`、last-id/last-duplicate 和 ordered class merge 语义。
- [x] 将 inline、leaf block、container block 和 label-associated attributes parser paths 统一迁移到 attribute-list contract。
- [x] 修改 directive attributes getter，使 absent 返回 `NULL`，present 返回 normalized string-map JSON，并记录 lifetime/ownership。
- [x] 修改 directive attributes setter，使其只接受完整 string-map JSON、规范化成功输入，并在失败时保持 node 不变。
- [x] 禁止 directive HTML attribute projection；CommonMark 输出规范化 attribute-list，XML 仅 transport-escape normalized JSON。
- [x] 同步 public、SPM、Android Prefab、install 和 package headers，并审计删除错误的 opaque-source 文案。
- [x] 重写 directive spec fixtures，覆盖所有 directive shapes、absent/`{}`、bare/quoted values、shortcuts、duplicates、Unicode 和 malformed fallback。
- [x] 扩充 C API tests，覆盖 source-to-JSON normalization、transactional failure、ownership、replacement、escaped NUL、duplicate rules 和错误 node kinds。
- [x] 增加 invalid/pathological tests，覆盖 malformed/truncated/unclosed attributes 和 sanitizer failure modes。
- [x] 增加 size-doubling complexity suite，覆盖 valid/unclosed 长值、连续 backslashes、many unique keys 和 many duplicate keys，排除 O(n²) 扫描/去重。
- [x] 更新 C++ consumer tests 和仍直接消费 directive attributes 的 wrappers，验证公开结果为 normalized string-map JSON。
- [x] 修订 Phase 4 migration/validation report，记录 corrected behavior diff、reviewed goldens、C API contract、resource limits、验证命令和 package audit。
- [x] 重新运行 Release C suite、C/C++ consumers、ASan、UBSan、format/lint、source/install/package/AAR audits 和 `git diff --check`。

Acceptance：

- [x] Directive attribute-list → normalized string-map JSON 语义由完整 C spec/API regression suite 固化。
- [x] Size-doubling performance tests 排除 valid、invalid、unclosed 与 many-key adversarial input 的 O(n²) 行为。
- [x] Release、C/C++ consumer、ASan、UBSan、format/lint 和 package-header audit 全部通过。
- [x] Public、installed 与 packaged headers 和实现一致。

实施与验证结果见 `docs/migration/2026-07-11-phase-4-directive-json.md`。

### Phase 5：冻结 canonical AST 契约

Tasks：

- [x] 对照 `markdown-renderer` 枚举所有保留的 `Markup` 节点、字段和 nullability。
- [x] 确认 canonical AST 不包含任何已删除 MS-specific nodes 或 fields。
- [x] 保留具体类型名 `List`，并为 Kotlin 全限定名/import alias 用法添加编译测试。
- [x] 为所有 `Markup` 冻结非 optional `Scope(start, end)` 契约，坐标数值与语义原样继承 native C parser。
- [x] 冻结三端统一 `ParseOptions`，固化默认开启项和 renderer-only 选项删除规则。
- [x] 为三端定义 typed Visitor 与只读 depth-first Walker 契约。
- [x] 逐 node kind 固化 canonical file-tree dump schema、完整 behavior-bearing fields、字段顺序、connectors、child count、escaping 和严格 scope 输出。
- [x] 建立 C package-owned Markdown fixtures 与 canonical file-tree `.ast` goldens；明确 C dump 和三端公开 `TreeDumper` 独立生成同格式 tree text，binding 只比较 package-local focused snapshot，禁止读取 C goldens、调用 C dump 或将 tree text 用作生产传输协议。

Acceptance：

- [x] 每个保留节点都有明确的 kind、字段、所有权与坐标语义。
- [x] 每个保留节点至少有一个 fixture。

冻结契约、C package-owned tree goldens、独立 binding test adapter 规则、fixture coverage、
Phase 6 implementation inputs 和验证结果见
`docs/migration/2026-07-11-phase-5-canonical-ast.md`。

### Phase 6：实现只读 C AST facade 和原生 AST dump

Tasks：

- [x] 审计现有 C API，确认是否存在不依赖 renderer 的 AST dump interface。
- [x] 不将 `cmark_render_xml` 或 CLI `-t xml` 当作新 AST dump interface。
- [x] 创建 `include/markdown_core.h`。
- [x] 实现 typed parse options、默认初始化与 `markdown_core_document_parse/free`。
- [x] 实现 root、kind、child/sibling traversal 和节点属性访问器。
- [x] 实现 error model、UTF-8 string view 和 source location API。
- [x] 若无独立 AST dump，添加原生、确定性 dump API、free API 和 test CLI 输出模式。
- [x] 确保 canonical file-tree dump 以 connectors 表达真实 parent/child 结构，并覆盖全部 core/extension behavior-bearing fields、children 顺序/count、null/empty/default 差异、Unicode 和每个 node 的 `Scope`。
- [x] 为每个 node kind 和字段建立 dump completeness fixtures，证明字段变化必然产生可见 diff。
- [x] 添加 native C dump golden tests、C/C++ consumer tests、ownership tests 和 sanitizer tests；C dump 必须与 Phase 5 的 C package-owned `.ast` goldens byte-for-byte 一致。
- [x] 隐藏不需要的内部 symbols，不向 platform packages 暴露 mutation/render API。

Acceptance：

- [x] C facade 可独立解析全部 C-owned fixtures，并只读遍历完整 AST。
- [x] 原生 AST dump 在不调用任何 renderer 的情况下产生稳定、直观的 file tree。
- [x] Completeness fixtures 证明任一公共 AST behavior-bearing field 的变化都会形成可见 diff。
- [x] 所有路径在 sanitizer 下通过。

实施与验证结果见 `docs/migration/2026-07-11-phase-6-c-ast-facade.md`。

### Phase 7：冻结全仓统一测试架构并收敛 CTest suites

Tasks：

- [x] 删除继承的 Pro Git benchmark 实现及全部痕迹：移除 Makefile 的 `progit` target、runtime `git clone` recipe、`BENCHFILE`/`benchinput.md` generation/dependency/cleanup rules、root checkout/generated input、相关 cache/temp/output、operational docs 和 `.gitignore` entries；在完成此项前禁止执行 inherited `make bench`，完成后 Phase 7 不再引用或特殊处理该语料来源。
- [x] 冻结历史根级 correctness-test 路由：曾以 `pnpm test` 作为全仓 correctness tests 聚合入口，并按固定名称委托语言入口；该历史模型已由本阶段后续的 execution-platform routing 修订取代。
- [x] 根据全仓 audit 修订 test routing contract，使三个验证 task families 直接映射到具名 execution-platform native targets，并将 suite/case discovery 与 filtering ownership 保留在各平台原生 runner。
- [x] 冻结独立 benchmark 路由：`benchmark:<platform>` family 只包含声明支持可信测量环境的 targets，不被 correctness 或普通 `verify` 隐式执行；CI 通过独立 benchmark jobs/schedules 调度，不为未支持平台建立空 target。
- [x] 冻结 runner ownership：C 的事实来源是 CTest，Swift 是 `swift test` + Swift Testing，Kotlin 是 Gradle/KMP test tasks，ES 是 package-native Node/browser/type test runner；不得用 pnpm、shell、Make 或另一平台 runner 重建第二份 suite graph。
- [x] 冻结每个 platform target 的语义：`test:*` 运行完整 correctness，`conformance:*` 运行完整 contract/spec checks，`benchmark:*` 运行完整 performance workloads；三者都不能退化为 build/lint、静默 skip 或空/no-op target。
- [x] 冻结跨端 suite/workload taxonomy 与命名规则。Correctness 类别至少包含 `api`、`ast`、`consumer`、`errors`、`ownership`、`unicode`、`robustness`、`pathological` 和 `packaging`；conformance 独立验证公开 contract/spec/schema mapping；performance 使用独立 `benchmark` workload/label。平台专属 suites 可以扩展，但相同语义不得使用互不相关的名称。
- [x] 实现互斥的 correctness、conformance 与 benchmark 原生 targets，并将 stress-shaped inputs 分别纳入 correctness robustness cases 与 benchmark workloads。
- [x] 冻结 discovery/filter contract：每个平台 runner 必须支持列出 suites/cases、按 suite/name/label/filter 单独运行、机器可读退出状态和可定位 diff；根级文档记录 pnpm、原生 runner、IDE 和 CI 的等价命令映射，使 CI 可按 `api`、`spec`、`extensions`、`pathological`、`benchmark` 等功能/成本分组独立分片。
- [x] 冻结 package-local fixture contract：C package 独占 Markdown parser canonical `.ast` goldens；Swift、Kotlin 和 ES 只使用本 package 的 focused binding cases 验证公开 schema mapping，不读取或复制 C goldens、不调用另一平台 adapter、不通过 dump text 构建 production AST，也不在根目录建立 shared fixture。
- [x] 冻结通用执行策略：统一 UTF-8 byte comparison、LF/final-newline、temporary directory ownership、timeouts、expected failures、process cleanup、serial/resource locks、performance warmup/repeat 和 deterministic diagnostics；具体 helper 使用各平台原生实现，不引入跨语言 test bridge。
- [x] 冻结 IDE/AI/editor contract：提交并验证 CMake/CTest presets、Swift Testing discovery、Gradle test tasks 和 ES test configuration；仓库任务清单能直接映射到 VS Code/CLion、Xcode、IntelliJ/Android Studio 和通用命令行，不依赖个人 IDE state。
- [x] 将现有 `pnpm test:swift` 从 build-only 占位语义迁移为真实 `swift test`；使用 Swift Testing 的 `@Suite`/`@Test` 组织 suites，禁止新增 XCTestCase 作为新架构基础，并保留 `swift build` 为独立 build task。
- [x] Kotlin 使用 Gradle/KMP 具名 platform tasks；ES 使用 Node/browser correctness、独立 conformance 与 benchmark scripts；根级只用对应 `family:<platform>` task 直接委托，不另设 language aggregate。
- [x] `test:c-host`、`conformance:c-host` 与 `benchmark:c-host` 配置/构建同一个 CMake graph，并分别委托互斥 CTest presets；不在 package scripts、Make、CI 或其他 wrapper 中复制测试清单和参数。
- [x] 建立完整 C test inventory，覆盖 native unit、只读 facade、legacy API、C/C++ consumers、CommonMark spec、extension spec、regression、entity、pathological/complexity、CLI、fuzz smoke、sanitizer 和 benchmark tests，并记录每项当前 runner、fixture、timeout、资源和预期结果。
- [x] 为 CTest 建立稳定 suite/label taxonomy，至少包含 `api`、`facade`、`consumer`、`spec`、`extensions`、`regression`、`pathological`、`fuzz` 和 `benchmark`；每个测试必须可由 `ctest -R` 和 `ctest -L` 独立发现、运行和定位失败。
- [x] 将 `packages/markdown-core/tests/` 下全部 Python test runners 和 ctypes/CLI bridge 迁移为由 CMake 构建或调用的统一测试实现；C API/accessor/ownership tests 优先使用原生 C/C++ executables，数据驱动的 spec/fixture orchestration 使用仓库已固定的标准工具链，不引入新的 test framework 或 package dependency。
- [x] 删除 `find_package(Python3)`、`PYTHON_EXECUTABLE`、doctest runner 和“Python 缺失时跳过 spec tests”的分支；`MARKDOWN_CORE_TESTS=ON` 时所声明的 suites 必须全部存在，缺少必需工具应在 configure 阶段明确失败而不是降级跳过。
- [x] 建立共享 test support，统一 binary/library discovery、parse options、fixture loading、UTF-8 byte comparison、canonical diff、timeouts、expected failure、temporary files、process cleanup 和 deterministic diagnostics，禁止每个 suite 复制一套 subprocess/ctypes glue。
- [x] 将当前 C API、spec、extension、entity、regression、pathological、directive complexity、inline delimiter、AST dump CLI 和 C/C++ consumer tests 分别注册为对应 CTest suite；保留测试粒度，不把全部场景折叠为一个不可定位的总脚本。
- [x] 将 fuzz smoke 迁移为 parse/traverse/dump/free 的确定性 CTest suite；长时间 fuzz campaign 保留为显式非默认任务，但复用同一个 harness 和 corpus。
- [x] 冻结 vendored-corpus policy：允许把网络 corpus 通过一次性、显式 maintenance/import workflow 固化到 `tests/corpora/<name>/`，但 correctness、benchmark、CI、IDE 和普通 build/test 命令必须完全离线，绝不在运行时 clone/download/update corpus。
- [x] 每个 vendored corpus 必须提交 manifest，记录 canonical source URL、不可变 commit/tag、imported paths、原始/规范化 SHA-256、byte size、import command/version、更新流程、copyright/attribution、完整 license 文件和 redistribution/package policy；没有明确允许商业使用、修改、仓库再分发和自动化测试且与项目许可政策兼容的许可不得导入。新 benchmark corpus 优先使用 project-authored、CC0/public-domain、MIT、BSD 或 Apache-2.0 内容。
- [x] corpus import workflow 若需要 clone，只能在显式 opt-in maintenance command 的临时目录中使用 pinned revision；完成后仅复制 manifest 声明的文件，删除 `.git`、checkout、download archive、intermediate concatenation、cache 和临时目录。禁止在 repo root 生成或保留 source checkout。
- [x] 添加 Phase 7 benchmark preflight guard：若发现未受管 source checkout、loose generated input、没有 manifest/license/hash 的 corpus、hash 不匹配、release package 包含 corpus，或普通 test/bench 命令将访问网络，立即失败；只允许许可政策接受、manifest 声明且内容校验通过的 `tests/corpora/` snapshot。
- [x] 生成或选择与历史 large-input benchmark 同等规模、允许商业使用/修改/再分发的 corpus；删除 benchmark 对 runtime network fetch、loose generated input、Python statistics scripts 和 Make-only orchestration 的依赖，使用仓库内已审查的 vendored deterministic corpora、现有 samples 与程序内可重复生成的数据集。
- [x] 完成 corpus 迁移后删除所有瞬态本地痕迹与隐藏入口：移除未受管 source checkout/generated input、相关 cache/temp/output、runtime download/generation/cleanup rules，以及 `.gitignore` 中会掩盖这些路径的 entries；最终只允许显式 tracked、licensed、manifested、hashed 的 corpus 文件存在。
- [x] 建立正常但独立调度的 CTest benchmark suite，统一标记 `benchmark`，覆盖 representative documents、large input、deep nesting、extensions 和 adversarial size-doubling cases；固定 warmup/repeat、timeout、串行/resource-lock 和结果格式，只使用稳定的 complexity/relative regression assertions，不使用易波动的绝对 wall-clock 阈值。
- [x] 让 `make test`、`make bench`、sanitizer jobs、CI 和 IDE tasks 委托给相同 CMake graph 与 CTest presets/labels；`make test` 对应 correctness，`make bench` 对应 `benchmark:c-host`/CTest `benchmark` label，Makefile 不再实现第二套 runner。
- [x] 添加 test topology audit，验证 CMake 声明、CTest discovery、pnpm tasks 和 CI 调用链一致；测试目录不存在 `.py` runner，CTest 不存在 disabled/optional/静默 skipped 的必需 suite；Make/CMake/pnpm/CI 不存在 runtime corpus network dependency；工作区/build/package/cache 不存在未受管 checkout/generated input，`.gitignore` 不得隐藏它们；所有 vendored corpus manifest/license/hash 与 package exclusion 必须验证通过。
- [x] 添加全仓 test architecture audit，验证每个已实现 `test:*`/`conformance:*`/`benchmark:*` platform task 直接调用具名原生 target、运行非空 selection、支持独立 discovery/filter；三个 family 互斥，CI 和 IDE mappings 没有遗漏或重复执行。

Acceptance：

- [x] 全仓测试架构、入口命名、runner ownership、suite/workload taxonomy、fixture、filter、timeout、failure 和 IDE discovery contract 已冻结。
- [x] 根级 test routing 只暴露 `test:<platform>`、`conformance:<platform>`、`benchmark:<platform>` 三个并列 task families，platform id 同时标识语言与真实 execution destination。
- [x] 不存在跨 host aggregate、中间 routing layer、language aggregation、suite 级 task、`:full`、公开 `stress` task 或通用 router；显式 maintenance task 不进入 test routing。
- [x] 每个 platform task 直接调用具名原生 target，suite/case discovery/filter 只属于 CTest、SwiftPM/xcodebuild、Gradle/KMP/instrumentation 或 ES package-native runner。
- [x] Correctness、contract/spec conformance 与 benchmark discovery 互斥，benchmark 只由独立 workflow 调度，且所有入口不得 build-only、no-op 或静默 skip。
- [x] Large、deep 与 repeated input 在 correctness 下以 robustness cases 验证结果，在 benchmark 下以独立 workload 测量性能。
- [x] Android emulator 由 Pixel 10 Pro XL Gradle Managed Devices group provision API 36/4 KB 与 API 36/16 KB，本机与 CI 使用相同 platform tasks，GMD 自动管理 lifecycle，清理 task 仅按需执行。
- [x] Linux x64 不提供本机 container/emulation 替代入口，实际运行归 Phase 19 CI 验收。
- [x] CI 显式映射全部声明平台及其适用 conformance target，topology audit 机器校验 task、native target、CI destination、互斥 selection 与非空执行一致，远端绿色证据归 Phase 19。
- [x] C correctness、conformance 与 benchmark 共用同一个 CMake/CTest graph 并由互斥 labels/presets 分离，全部 C suites 可发现且不依赖 Python 或运行时网络。
- [x] 所有外部 corpus 都是 tracked、pinned、licensed、attributed、manifested、hashed 且从 release packages 排除的离线 snapshots，不残留未受管 checkout、archive、loose generated input 或 cache。
- [x] CLI、IDE、AI 和 CI 使用同一套平台任务与原生 discovery，routing、native targets、CI mappings 与 topology audit 实现完成后 Phase 7 关闭。

冻结契约见 `docs/specs/test-architecture.md`；迁移 inventory、关键决策与验证结
果见 `docs/migration/2026-07-11-phase-7-test-architecture.md`。

### Phase 8：将所有 parser tests 迁移到 AST dump

Tasks：

- [x] 使用 Phase 7 inventory 对 render-dependent assertions 分类，覆盖 spec、extension spec、API、fuzz、roundtrip、CLI 和自定义/历史 harness tests。
- [x] 修改 CommonMark spec test harness，使 expected result 为 canonical AST dump，而不是 HTML。
- [x] 修改所有 extension spec tests，使 expected result 为 canonical AST dump。
- [x] 将借助 XML renderer 检查 AST 的 tests 切换为新 dump API 或直接 accessors。
- [x] 将借助 HTML/CommonMark/plaintext 输出检查 parser behavior 的 tests 切换为 AST dump。
- [x] 将 fuzz tests 改为 parse、traverse/dump、free，不再调用 renderer。
- [x] 将 roundtrip tests 改为 AST determinism/fixture tests，或在已无 parser 价值时删除。
- [x] 删除纯 renderer-specific assertions，不将它们伪装为 AST tests。
- [x] 人工审查大规模 expected-output 迁移，不得未审查地使用当前 parser 批量覆盖 golden 而形成自证。

Acceptance：

- [x] 所有保留的 parser、spec、API 与 fuzz tests 在不调用 HTML、XML、CommonMark、LaTeX、man 或 plaintext renderer 的情况下通过。
- [x] 任一未批准的 parser behavior drift 都表现为可定位的 canonical tree golden diff，而不被规范化过程隐藏。

迁移分类、api_engine 明细、fixture 审查记录与验证结果见
`docs/migration/2026-07-12-phase-8-ast-dump-tests.md`。

### Phase 9：删除所有 render 相关函数与实现

Tasks：

- [x] 删除 HTML renderer 及 `cmark_markdown_to_html`。
- [x] 删除 XML renderer，确保 AST dump 不依赖它。
- [x] 删除 CommonMark renderer。
- [x] 删除 LaTeX renderer。
- [x] 删除 man renderer。
- [x] 删除 plaintext renderer。
- [x] 删除通用 render framework、render headers 和无使用 buffer/helpers。
- [x] 删除 syntax extension API 中的 HTML/XML/CommonMark/plaintext render callbacks 及各 extension 实现。
- [x] 删除 CLI `-t/--to` render formats、help text 和 output routing，只保留解析/AST dump 诊断能力。
- [x] 从 CMake、Make、installed headers、export maps、docs、benchmarks 和 tests 中删除 renderer。
- [x] 审计源码和导出 symbol table，确认不再存在 render API。

Acceptance：

- [x] Clean build 不编译任何 content renderer。
- [x] Public headers 与导出 symbols 不包含 render API。
- [x] CLI 不支持 render formats。
- [x] 全部 AST-based tests 仍通过。

删除 inventory、CLI/API 收窄、产物审计和验证结果见
`docs/migration/2026-07-12-phase-9-renderer-removal.md`。

### Phase 10：修复已发现的 C engine/facade 缺陷并冻结并发契约

本阶段是任何 platform AST binding 开始前的阻塞阶段。测试侧 warmup、串行化或
binding 层锁只能作为临时诊断手段，不能成为 C facade 的调用契约，也不能用来
关闭已确认的 C 缺陷。

Tasks：

- [x] 建立 Phase 10 C defect ledger，汇总截至本阶段开始时 migration reports、sanitizer、并发测试和人工审计中所有已确认的 C engine/facade 缺陷；每项记录复现、根因、影响面、回归测试和关闭证据，不以 platform workaround 代替修复。
- [x] 修复并发首次 `markdown_core_document_parse`：当前 `markdown_core_core_extensions_ensure_registered` 的无同步 `static int registered` 会让多个线程同时修改 extension registry、extension node-type counters 和 node flags，并可在 `markdown_core_register_node_flag` 中竞争崩溃。
- [x] 使用适用于项目完整平台矩阵和当前 C language baseline 的进程级 once 机制包住**整个** core-extension 注册事务；不得只给 `markdown_core_register_node_flag` 局部加锁，也不得要求 consumer 显式 warmup。
- [x] 冻结 registry 生命周期：首次成功注册后 extension descriptors 对 facade parse 保持 process-lifetime immutable；处理 `markdown_core_release_plugins` 与 once 状态脱节的问题，禁止出现“已初始化标志仍为真但 registry 已释放”、并发释放/读取或不完整重注册。
- [x] 审计 facade parse 路径的全部 process-global mutable state，包括 node type/flag 分配、syntax-extension registry、默认 allocator 和任何 lazy cache；将只应在启动时变化的状态纳入同一初始化边界，或证明其独立线程安全。
- [x] 在 public C header 中写明线程安全与所有权契约：不同 document 的 parse/traverse/dump/free 可并发；同一 document/node 的只读访问条件、与 free 的互斥责任、error/object ownership 和全局生命周期不得依赖未公开约定。
- [x] 添加无 warmup 的原生 C 并发首次 parse CTest：在全新进程中以 barrier 同时释放多个线程，覆盖 parse、extension attach、traverse、dump 和 free，并验证全部结果一致且无 crash/data race；另加初始化完成后的并发 stress 与生命周期回归测试。
- [x] 增加 ThreadSanitizer 配置/CI 验证（支持的平台），并继续运行 ASan、UBSan、Release、shared/static、C/C++ consumer 与 package builds；TSan 不可用的平台必须至少编译并运行相同原生并发 regression，而非静默跳过整个并发契约。
- [x] C 修复落地后删除 Swift Testing 中的全局 facade warmup，让 Swift 并行测试直接覆盖真实首次调用；不得把 workaround 搬入 Swift、Kotlin 或 ES production binding。
- [x] 对 defect ledger 中其余已确认问题逐项补回归测试并修复；重新审计公开 facade 的失败路径、溢出、资源释放、determinism 和跨平台行为，未关闭的已知 C correctness、memory-safety、thread-safety 或 lifecycle defect 阻塞 Phase 11。
- [x] 新增 Phase 10 migration/validation report，记录线程模型、portable once 设计、registry lifecycle 决策、修复 diff、测试复现方式、sanitizer/TSan 结果、platform/package audit 和所有 workaround 的删除情况。

Acceptance：

- [x] 并发首次和后续 facade parse 不需要预热或外部锁。
- [x] Registry 初始化与释放不存在竞争或状态脱节。
- [x] 原生并发 regression、TSan（支持的平台）、ASan、UBSan、Release、shared/static、consumer 和 package 验证全部通过。
- [x] Phase 10 defect ledger 中截至阶段开始已确认的 C 缺陷全部关闭。
- [x] Swift 测试 warmup 已删除，platform bindings 只依赖公开 C 契约。

defect ledger 见 `docs/migration/2026-07-12-phase-10-defect-ledger.md`;线程模型、
portable once 设计、registry lifecycle 决策、修复内容、缺陷敏感性验证与
sanitizer/TSan 验证结果见 `docs/migration/2026-07-12-phase-10-concurrency-contract.md`。

### Phase 11：实现 `swift-markdown-core`

Tasks：

- [x] 创建 SwiftPM product/module `MarkdownCore`。
- [x] 将 `1.0.0` minimum deployment targets 设为 iOS 18 和 macOS 15，并为 iOS 18/26、macOS 15/26 配置 CI。
- [x] 实现 Swift `Markup` 协议和全部 immutable node types。
- [x] 实现 `Document.parse`。
- [x] 实现 Swift `ParseOptions`、typed Visitor 和只读 Walker。
- [x] 公开 Swift `TreeDumper.dump(markup)`，并让所有 `Markup.dump()` 委托该实现。
- [x] 将 C AST 完整拷贝为 Swift value tree，然后释放 C document。
- [x] 添加 AST、Unicode、failure、ownership 和 `Sendable` tests，并由 Swift test target 以 exhaustive Visitor + Walker 独立遍历 Swift AST，生成 canonical tree text 与 package-local focused snapshot 比较。
- [x] 遵守 Phase 7 测试架构：SwiftPM 分离 `MarkdownCoreTests` 与 `MarkdownCoreConformanceTests` targets；`test:swift-macos`/`test:swift-ios-simulator` 只运行 correctness，配对的 `conformance:swift-*` tasks 只运行 conformance target。
- [x] Swift performance workloads 与 correctness tests 分离，由 `benchmark:swift-macos` 委托原生 benchmark executable，不进入 `test:swift-macos`。
- [x] 添加独立 SwiftPM consumer test。

Acceptance：

- [x] Swift package 仅通过 `import MarkdownCore` 即可调用 `Document.parse`，且不暴露 C handle。
- [x] Swift package-owned tests 全部通过。
- [x] iOS 18/26 与 macOS 15/26 声明矩阵验证通过。

Swift immutable AST 设计、C-to-Swift ownership 边界、公开 API、canonical golden、
consumer、benchmark、deployment matrix 与验收结果见
`docs/migration/2026-07-12-phase-11-swift-binding.md`。

### Phase 12：实现 `kotlin-markdown-core`

Tasks：

- [x] 使用 Kotlin Multiplatform plugin 建立 `commonMain`、Android、通用 JVM 与 Kotlin/Native targets，并固化可发布的 OS/architecture matrix。
- [x] 配置 KMP root publication、JVM JAR publication、Android AAR publication 和每个 Native target publication，保留 Gradle Module Metadata。
- [x] 通过 `publishToMavenLocal` 固化并记录 root/target-specific coordinates、artifact suffix、POM、`.module`、sources/docs 和 native payload。
- [x] 实现 Kotlin sealed immutable AST types。
- [x] 将 Kotlin public model、Visitor/Walker 与 wire codec 分目录；model 文件只拥有 public types，单一 exhaustive decoder 直接把 kind-specific `MKC2` payload 构造成 public model，并删除 `Any`/`WireNode` 中间树。
- [x] 实现使用标准 UTF-8 bytes 的 native bridge 和 `Document.parse`。
- [x] 实现 `commonMain` 的 `ParseOptions`、typed Visitor 和只读 Walker。
- [x] 在 `commonMain` 公开 `TreeDumper.dump(markup)`，并让所有 `Markup.dump()` 委托该实现。
- [x] 确保 JNI wrapper 属于 Kotlin package，不进入 portable C core。
- [x] 添加 AST、Unicode、failure、ownership 和 native packaging tests，并由 Kotlin test source set 以 exhaustive Visitor + Walker 独立遍历 Kotlin AST，生成 canonical tree text 与 package-local focused snapshot 比较。
- [x] 添加 KMP Gradle、JVM Gradle Module Metadata、Android Gradle/AAR consumer tests；真实 JVM Maven consumer 的 required-CI 接入归 Phase 19。
- [x] 遵守 Phase 7 测试架构：按 KMP execution platform 注册配对的具名 Gradle correctness/conformance tasks，例如 `jvmTest`/`jvmConformanceTest` 与 `macosArm64Test`/`macosArm64ConformanceTest`；根级 `test:kotlin-*`/`conformance:kotlin-*` 直接委托。
- [x] Kotlin performance workloads 与 correctness tests 分离，由 `benchmark:kotlin-jvm` 委托 Gradle benchmark task，不进入 Kotlin correctness targets。

Acceptance：

- [x] `commonMain` 公共 API 在 Android、通用 JVM 与已声明的 Kotlin/Native OS/architecture matrix 上发布并验证。
- [x] Root 与所有 target publications 完整且 metadata 一致。
- [x] Gradle/KMP、JVM/Maven 和 Android/AAR consumers 可选择正确 variant、加载 native library、调用 `Document.parse` 并通过 package-owned tests。
- [x] 支持的 IDE 可 clean import/sync。

实现、坐标、toolchain/compatibility policy、CI matrix、consumer、benchmark 与验收结果见
`docs/migration/2026-07-12-phase-12-kotlin-binding.md`。

### Phase 13：实现 `@nouprax/es-markdown-core`

Tasks：

- [x] 实现同步 `Document.parse(...): Document`，并确保 WASM 在该 API 可调用前完成初始化。
- [x] 使用 Emscripten 或等价工具将 C engine/facade 编译为 WebAssembly。
- [x] 通过直接 C/WASM AST traversal 构建 ES objects，不使用 JSON bridge 或 AST dump bridge。
- [x] 将 ES readonly model、WASM runtime、stateful exhaustive decoder 和 Walker 拆分为 TypeScript module tree，由 `tsc` 唯一生成 ESM 与 declarations；model 不解释 WASM boundary，root barrel 不拥有 Markup 字段构造逻辑。
- [x] 实现递归 readonly TypeScript declarations，不实施 `Object.freeze` 或 recursive runtime freeze。
- [x] 实现 TypeScript `ParseOptions`、typed Visitor 和只读 Walker。
- [x] 隐藏 WASM pointer、memory 和 runtime initialization details。
- [x] 在 TypeScript source graph 中公开 `TreeDumper`，为每个 runtime Markup 提供非枚举 `dump()` 方法，并由 ES conformance target 以 exhaustive `visit` + Walker 独立生成 canonical tree text 与 package-local focused snapshot 比较。
- [x] 添加独立 npm consumer test：runtime 与 TypeScript consumer 都安装实际 `npm pack` tarball，类型检查由 NodeNext 通过 package `exports.types` 解析，不使用 `paths` 直连仓库 build output。
- [x] 遵守 Phase 7 测试架构：注册 `test:es-node`/`test:es-browser` correctness targets 与独立 `conformance:es-node` target；suite names、filters、fixtures、timeouts 和 diagnostics 与冻结契约一致。
- [x] ES performance workloads 与 correctness tests 分离，由 `benchmark:es-node` 委托 package-native benchmark script，不进入 correctness targets。

Acceptance：

- [x] npm package 在 Node 和目标 browser 环境完成 WASM 初始化后可同步调用唯一 `Document.parse` 入口。
- [x] npm package 不暴露 WASM 实现且不执行 runtime freeze。
- [x] npm package-owned tests 全部通过。

实现、WASM ABI、公共 API、测试/consumer、benchmark、CI 与验收结果见
`docs/migration/2026-07-12-phase-13-es-binding.md`。

### Phase 14：跨端 conformance 与性能基线

Tasks：

- [x] 建立与 correctness 并列的 `conformance:<platform>` family；每个 task 只委托对应原生 contract/schema target，不被 correctness 隐式包含。
- [x] 比较四端节点顺序、字段、nullability、Unicode 和 source locations。
- [x] 为大文档、深度嵌套和反复 parse/release 增加 correctness robustness cases，并为 large document/deep nesting 建立独立 timed benchmark workloads；不建立公开 `stress` task。
- [x] 记录 C native、Swift copy、JNI 和 WASM 的 parse/binding 性能与内存基线。
- [x] 只根据 benchmark 证据决定是否需要跨边界优化。

Acceptance：

- [x] C 通过 package-owned tests 验证 parser behavior。
- [x] 三端 binding 通过各自 correctness cases 验证代码行为。
- [x] 独立 `conformance` target family 验证公开 schema mapping。
- [x] Large/deep inputs 同时拥有互不混合的 robustness assertions 与 timed benchmark workloads。
- [x] `benchmark` 提供可重复的性能与内存基线。

统一命令、四端独立 canonical traversal、robustness/benchmark 边界、可重复 workload、基线结果与
跨边界优化决策见
`docs/migration/2026-07-12-phase-14-conformance-performance.md`。

### Phase 15：重构测试归属与共享契约布局

目标：测试实现和测试数据都由被测 package 自己拥有，根目录不再充当跨 package 的
测试源码或 fixture 容器。C parser/facade 是语义事实来源；Swift、Kotlin 和 ES tests
只验证各自 binding 对该公开 C schema/行为的映射，不重复承担 parser correctness。

Tasks：

- [x] 清点根目录 `tests/` 下的所有 suites、consumer projects、compile contracts、fixtures 和 support code，并为每项指定唯一 package owner；禁止以“跨端”为理由保留职责不明的 root test runner 或 root fixture。
- [x] 将 canonical Markdown/`.ast` goldens、coverage manifest、CommonMark/extension corpora、pathological/fuzz/robustness inputs 以及 C/C++ consumer tests 全部收敛到 `packages/markdown-core/tests/`；它们只定义和验证 C parser/facade semantics，并由唯一 CMake/CTest graph 发现和运行。
- [x] 将 Swift AST、API、Unicode、failure、ownership、`Sendable`、robustness 和 SwiftPM consumer tests 移入 `packages/swift-markdown-core/` 自己的 test/consumer-test layout，并由该 package 的 SwiftPM manifest 与 Swift Testing suites 独立拥有；独立 consumer package 的 test target 直接依赖公开 `MarkdownCore` product，不创建无实际入口的 executable/`main.swift` 中间层；测试数据应是验证 C-to-Swift field/nullability/scope/error/ownership 映射所需的 package-local 最小 cases，不复制完整 C parser corpus。
- [x] 将 Kotlin common/JVM/Android/Native tests、compile contracts、packaging tests 和 Gradle/Maven/Android consumers 收敛到 `packages/kotlin-markdown-core/`（包括其 nested Android runtime module）下，由 KMP source sets 和 package-local consumer builds 独立拥有；只验证 native/JNI-to-Kotlin mapping 和目标平台交付，不重复验证 Markdown parser 规范。
- [x] 消除顶层 sibling `packages/kotlin-markdown-core-android-native/`：将其迁入 `packages/kotlin-markdown-core/android-runtime/`，作为 `kotlin-markdown-core` 唯一拥有的内部 Android runtime Gradle module；保留独立 module 仅因为 Android-KMP plugin 不支持 `externalNativeBuild`，不得把它描述成第二个 Kotlin product/package。
- [x] 将内部 Android runtime 的 Gradle project path、AAR artifactId、publication metadata 和依赖名从 `android-native` 统一改为 `android-runtime`；验证 KMP Android publication 只把它作为携带四 ABI JNI `.so` 的 runtime dependency，consumer 仍只需声明公开 `com.nouprax:kotlin-markdown-core` root coordinate。
- [x] 确保 ES Node、browser、types、conformance、ownership、robustness、packaging 和 npm consumer checks 全部位于 `packages/es-markdown-core/`；runtime 与 TypeScript consumer 必须安装实际 packed artifact，并通过 package exports 解析公开 surface，只验证 C/WASM-to-ES mapping，不复制 C canonical goldens。
- [x] 重新定义各 binding 的 `conformance` suite：以 C facade 的公开 node-kind/field/nullability/scope/error schema 为覆盖清单，在 package-local focused cases 上证明每种映射可达且正确；不得读取 C package 的 golden dump、调用 C test runner、比较另一 binding 输出，或把完整 parser conformance corpus 复制到 binding package。
- [x] 删除根目录 `tests/`；更新 CMake、SwiftPM resources、Gradle inputs、ES paths、consumer commands、CI、audit scripts、README 和 migration docs 中的全部旧路径。
- [x] 只暴露显式 platform tasks，不建立跨 host aggregate，也不在根目录重建 suite/case 清单、fixture normalization、共享 fixture 或跨语言 test harness。
- [x] 增加布局审计：拒绝根级测试源码/runner/fixture、跨 package test implementation/data imports、binding package 内复制 C parser corpus，以及 package test 绕过本 package 公共 API 读取 C dump 或另一 binding 输出。
- [x] 从 clean checkout 分别运行每个 package 的完整 correctness、consumer、独立 conformance、robustness 和 packaging checks，再运行根级聚合入口，证明移动未改变 native discovery、filter names、diagnostics 或覆盖范围。

Acceptance：

- [x] Phase 15 关闭快照中不存在根目录 `tests/` 或任何 root/shared test fixture，Phase 18 对 canonical AST corpus ownership 的后续转移必须显式 supersede 本结论。
- [x] C package 独占 parser/facade correctness corpora，并在 Phase 18 前独占 canonical goldens。
- [x] 每个 binding package 只用自己的 focused cases 验证公开 binding mapping、ownership 和 packaging。
- [x] 所有 test、runner、consumer 与 compile contract 均由唯一 package 的原生工具链拥有，且没有职责交叉或覆盖退化。

归属清单、迁移后的目录、conformance schema 覆盖、审计规则与验证结果见
`docs/migration/2026-07-12-phase-15-test-ownership.md`。

### Phase 16：删除旧 setup 与收窄公共面

Tasks：

- [x] 在新 consumer tests 覆盖后删除旧 SwiftPM、`spm/`、`swift/` 和 Android/AAR setup。
- [x] 删除或内部化旧 wrappers/bindings。
- [x] 确保 platform packages 只导出 immutable AST、公开 diagnostic `TreeDumper`/`Markup.dump()`，不导出 mutation API、renderer、native handle 或序列化协议。
- [x] 审计导出 symbols、public headers、Swift API、Kotlin API 和 npm exports。
- [x] 清理失效文档、脚本、CI 和 package coordinates。
- [x] 保留所需的 license/attribution 文件。

Acceptance：

- [x] Clean checkout 只依赖新 monorepo setup。
- [x] 旧包装层不再参与构建、测试或发布。

删除清单、私有编译头布局、四端公共面 allowlist、动态符号审计、失效引用清理和
验收结果见 `docs/migration/2026-07-12-phase-16-public-surface.md`。

### Phase 17：修复全仓 audit 发现并冻结单一 C library 交付边界

本阶段是发布准备前的阻塞阶段。Phase 16 后的完整 repo audit 证明目录迁移、四端
correctness、sanitizer、consumer 与大部分 package checks 已通过，但也确认 C install
metadata、CMake consumer、CI enforcement、许可证归属和迁移后配置仍有遗漏。所有问题
必须在本阶段关闭；不得把修复推迟到发布 workflow，也不得通过重新公开继承自
`cmark-gfm` 的 extensions library 来绕过单一 library contract。

Audit 同时发现 Phase 7 的 test routing contract 需要修订；该 task 已写回并在
Phase 7 完成，因为它属于测试架构 ownership，而非 C package remediation。共享
canonical AST contract 的根级收敛归 Phase 18，远端平台执行证据归 Phase 19 CI
验收；两者都不重新打开 Phase 7，也不在本阶段复制 routing task。

Tasks：

- [x] 冻结并实现唯一公共 C library：安装、链接和发现层只存在 `libmarkdown-core`；parser core、GFM/formula/directive extensions 可以使用 private object/static targets 组织编译，但不得单独安装、导出、写入 pkg-config/CMake package，或要求 consumer 链接 `libmarkdown-core-extensions`。
- [x] 修正 `markdown-core.pc`：只链接 `-lmarkdown-core`，把描述收窄为 parser/immutable AST facade，并为 clean shared-only 与 static install 注册独立 pkg-config consumer 编译、链接、运行检查。
- [x] 提供标准、可消费的 CMake config package，只有一个 reviewed public imported target；补齐 install include usage requirements，禁止 CLI executable、private engine/extensions targets 和内部 headers 进入 public export，并用 install-prefix 外的独立 `find_package(... CONFIG REQUIRED)` consumer 验证。
- [x] 扩充 package audit，使其对 shared-only/static install 分别执行真实 pkg-config 与 CMake consumer configure/build/link/run；仅检查文件名或 symbol allowlist 不得再让坏掉的 metadata 通过。
- [x] 修复 `packages/markdown-core/extensions/CMakeLists.txt` 的 cmake-format 失败，并使 C/CMake/Swift/Kotlin/ES formatter 与 linter 的根级只读入口全部通过；最终 clean-checkout 总复验归 Phase 21。
- [x] 将 `audit-public-surface.sh` 和 `audit-package-contents.sh` 直接接入 required CI，继续执行 test-topology audit；确保 CI 的入口与根 `verify` 契约一致，而不是只在本地 script 中声明检查。
- [x] 将 CodeQL language/build matrix 从遗留模板更新为当前 C/C++、Java/Kotlin、JavaScript/TypeScript 和 Swift 产品语言，删除无产品源码的 Python/Ruby jobs，并为 compiled languages 使用可重复的实际 build steps。
- [x] 补齐 `CheckFileOffsetBits.cmake` 所引用的 `COPYING-CMAKE-SCRIPTS` 许可证/归属，或以法律上等价且明确可追踪的方式并入现有许可文件；审计所有 source、安装包与发布包的 attribution 完整性。
- [x] 修复仓库自身的 Kotlin Gradle Project object dependency notation，使用 typed `DependencyHandler.project(String)`；以 `--warning-mode=fail` 将剩余 warning 精确归因到稳定版 AGP 9.2.1，并把采用已包含上游修复的 AGP 9.3 stable 及首发前 warning-free 验收写入 Phase 20，不为关闭本阶段采用 preview toolchain。
- [x] 将 `docs/toolchains.md` 与实际 JDK 26、Android compile/target SDK 37 及当前 Gradle project paths 对齐，删除已不存在的 `:android:dependencies` 命令，并重新核对 IDE/toolchain policy。
- [x] 清除迁移后配置残留：删除 `.gitattributes`、`.gitignore`、`.prettierignore`、ESLint 和相关 ignore/audit 配置中对已删除 root `tests/`、legacy `android/`、`spm/`、`xml2md_gfm.xsl`、旧 build-phase/local Gradle-home paths 等无效引用；所有文本配置补 final newline，exclusions 只覆盖真实 generated/vendored/golden paths，并为 root Node scripts 声明显式 ESLint globals。
- [x] 审查 `pnpm-workspace.yaml` 的 `samples/*`：若 samples 不是 workspace packages 则删除空 glob；若保留则提供真实、受验证且归属明确的 package manifests。
- [x] 对继承 C source/test 中剩余 TODO/FIXME 逐项分类：修复真实缺陷，或记录为有所有者和理由的非阻塞 ledger；不得留下无法判断是否遗漏的迁移注释。
- [x] 使用 repo Gradle Wrapper 在隔离 local Maven repository 发布并运行 KMP Gradle、JVM Gradle Module Metadata 与 Android consumers，并由 JVM consumer 实际加载 native payload；真实 Maven compatibility smoke 归 Phase 19 required CI。
- [x] 完成 Phase 17 remediation report，逐项记录 audit 复现、根因、修复、回归测试、关闭证据，以及转交 Phase 18/19/20/21 的共享合同、远端执行、发布和最终物理收尾边界。

Acceptance：

- [x] 安装前缀只向 C consumer 呈现一个 `libmarkdown-core` 与 public facade。
- [x] Shared-only/static pkg-config 与标准 CMake consumer checks 已实现并接入 CI。
- [x] 全量 format/lint/audit/CodeQL 配置、许可证、工具链文档和 Gradle warnings 已收敛。
- [x] 四端 correctness、ASan/UBSan/TSan、deployment matrix、package consumers 与发布内容审计均有可重复入口。
- [x] 共享 canonical AST spec 收敛归 Phase 18，远端 CI 绿色结果归 Phase 19，发布执行归 Phase 20，Git snapshot、物理工作区清理与最终 clean-checkout 复验归 Phase 21，且均不反向阻塞本阶段实现关闭。

任务与原始 audit 证据记录在 `docs/migration/2026-07-12-phase-17-repo-audit-remediation.md`。

### Phase 18：建立跨平台共享 canonical AST conformance spec

本阶段把 canonical Markdown/`.ast` pairs 从 C package 私有 fixture 提升为根级产品合同。
它只定义共享 conformance data 与 coverage manifest，不建立 root runner，也不把 parser
correctness corpus 复制给 bindings。C、Swift、Kotlin 和 ES 继续由各自原生
`conformance:<platform>` target 通过公开 parse/immutable AST/Visitor/Walker/TreeDumper
路径消费同一份 spec。

Tasks：

- [x] 建立根级 `specs/canonical-ast/`，迁移现有 canonical Markdown/`.ast` pairs、README 和 coverage manifest；删除 C package 私有副本，并禁止 Swift/Kotlin/ES 提交平台副本。
- [x] 冻结 manifest discovery、parse options、排序、UTF-8/LF/final-newline 与 coverage schema，覆盖全部 28 种 Markup、所有 behavior-bearing fields、null states、scope、escaping 和 child order。
- [x] 让 C conformance target 通过公开 C parse/document dump API 比较全部共享 cases；CommonMark、extensions、regression、pathological、fuzz 与 robustness correctness corpus 继续由 C package 独占。
- [x] 让 Swift macOS/iOS Simulator conformance targets 通过公开 `Document.parse`、Visitor/Walker 与 `TreeDumper` 消费共享 spec，删除 package-local expected tree literals。
- [x] 让 Kotlin JVM、Android host/emulator、macOS ARM64 与 Linux x64 conformance targets 消费同一共享 spec，删除 package-local expected tree literals。
- [x] 让 TypeScript/ES Node conformance target 通过公开 npm API 与 `TreeDumper` 消费共享 spec，删除 package-local expected tree literals，不调用 C dump 或另一 binding 输出。
- [x] 为 simulator/emulator/native test bundle 从同一 root source 生成 derived resources；不得依赖 repo cwd、越界 symlink、网络下载或手工维护的 tracked copy。
- [x] 保留各 binding 自己的 Visitor exhaustiveness、Walker events、error/ownership/lifetime unit tests；共享 spec 只属于 conformance，不进入 correctness 或 benchmark discovery，也不新增 `spec:*` task。
- [x] 提供显式 golden maintenance 命令和 fail-closed audits：rewrite 只生成待人工审查 diff；拒绝 runner、重复 corpus、空 discovery、未清单 case、发布包携带 spec data 或任一平台未接入。
- [x] 更新 Phase 5/7/8/11–17、canonical AST/dump、test architecture 与 package ownership 文档，撤销“C 独占 canonical goldens、bindings 只能使用 package-local snapshot”的旧结论。

Acceptance：

- [x] `specs/canonical-ast/` 是唯一 canonical AST corpus。
- [x] 四端原生 conformance targets 枚举同一非空 manifest 并 byte-for-byte 通过。
- [x] 故意破坏任一 binding mapping、Visitor dispatch、Walker hierarchy/order、scope/escaping 或 TreeDumper grammar 都会使对应 platform conformance 失败。
- [x] Spec data 不进入发布产物，correctness、conformance 与 benchmark 仍互斥。

实施与验收记录见 `docs/migration/2026-07-13-phase-18-shared-canonical-ast-spec.md`。

### Phase 19：建立质量门禁与非阻塞 PR observability

Tasks：

- [x] 在 required CI 实际运行完整 platform matrix，并取得 Kotlin Linux x64 与 repo-managed Android emulator correctness/conformance 绿色证据；缺少 host、simulator、browser 或 emulator 时必须失败，不能静默跳过。
- [x] 取得 required CI 中 public-surface/package-content/pkg-config/CMake consumer audits 与 CodeQL 全部绿色证据；workflow wiring 可在前序阶段完成，远端执行结果统一由本阶段验收。
- [x] 添加 repo-owned Maven Wrapper 与最小 JVM Maven consumer，从同一隔离 local Maven repository 运行 `verify` 并实际调用 `Document.parse`；required CI 必须执行该真实 Maven smoke，且不得要求开发机预装全局 `mvn`。
- [x] 为 blocking CI 与 CodeQL 分别建立稳定、唯一的 `Required gates` 与 `CodeQL gate` 汇总 checks；汇总 job 必须在任一依赖失败、取消或跳过时失败，ruleset 不直接依赖易变的 matrix job names。
- [x] 让所有 blocking workflows 同时监听 `pull_request` 与 `merge_group`，使同一 required checks contract 可安全启用 GitHub merge queue。
- [x] 提交可导入的 default-branch ruleset recipe，只要求 `Required gates` 与 `CodeQL gate`；禁止把 benchmark、binary size、coverage trend 或其他 informational pipeline 加入 required status checks。
- [ ] 在 GitHub repository 导入并启用 ruleset，验证失败 gate 会阻止 PR merge、最新 base revision policy 生效，并记录最小 bypass ownership；仓库外设置的实际启用状态不能仅以 committed JSON 代替。
- [x] 建立非阻塞 PR metrics workflow，运行 C、Swift、Kotlin/JVM 与 ES/WASM benchmark，并报告 C shared library、Kotlin/JVM JAR 与 ES/WASM binary sizes；任何 metrics 缺失或回归都只能提示，不能改变 required gate 结论。
- [x] 使用 fork-safe 的两段式 PR comment：只读 `pull_request` workflow 执行不可信代码并上传纯数值 artifact，具有写权限的 `workflow_run` commenter 不 checkout、不执行 artifact/PR code，只校验 allowlist 数值并创建或更新单条 PR comment。
- [x] 添加 CI policy audit，机器校验 stable gate names、ruleset contexts、`merge_group`、metrics 非阻塞边界、commenter 权限分离与 scheduled benchmark 不进入 PR gate。

Acceptance：

- [ ] Default branch ruleset 已启用，且只把 `Required gates` 与 `CodeQL gate` 作为 required status checks。
- [ ] 完整 correctness/conformance/sanitizer/consumer/package/security matrix 在 `pull_request` 与 `merge_group` 上 fail-closed，失败或缺失 checks 会阻止 merge。
- [ ] Benchmark 与 binary-size pipeline 始终 non-blocking，并能为同仓和 fork PR 安全更新一条 informational comment。
- [ ] Privileged commenter 从不执行不可信代码或 artifact。

配置、权限边界、required/non-required 清单与启用步骤见 `docs/migration/2026-07-13-phase-19-quality-gates.md`。

### Phase 20：建立发布支持与发布 CI

Tasks：

- [ ] 准备首个 `1.0.0` release，确认新 repo 不包含旧 tags，且发布文档明确不承诺 C ABI compatibility。
- [ ] 对齐 C、SwiftPM、Maven 和 npm 版本。
- [ ] 在 AGP 9.3 stable 发布后升级并重新验证 Gradle/Kotlin/Android compatibility matrix；cache-cold model、Android host/device tests、publication 与 consumer checks 必须在 `--warning-mode=fail` 下通过，确认 AGP 9.2.1 的上游 Project dependency notation warning 已消失，不得以 9.3 preview 作为首发工具链。
- [ ] 验证 SwiftPM source URL、repo-derived identity `markdown-core` 和 product/module `MarkdownCore`。
- [x] Maven Central `com.nouprax` namespace 已通过 `nouprax.com` DNS TXT 完成所有权验证。
- [ ] 验证 Maven publication coordinate 为 `com.nouprax:kotlin-markdown-core:<version>`。
- [ ] 验证 KMP root、JVM、Android 和所有 Native target publications 在同一 Central deployment 中齐全，POM/Gradle Module Metadata 引用与 target-specific coordinates 正确。
- [ ] 从 staged/local Maven repository 运行 KMP Gradle、JVM Gradle Module Metadata、repo-owned Maven Wrapper 驱动的真实 JVM Maven 和 Android AAR consumer tests，并在 IntelliJ IDEA/Android Studio 执行 release clean-sync smoke test。
- [ ] 创建有过期时间的 Maven Central Portal user token，将其设置为 `MAVEN_CENTRAL_USERNAME` / `MAVEN_CENTRAL_PASSWORD` environment secrets。
- [ ] 创建带 passphrase 的 PGP signing key，发布 public key，将 private key/passphrase 设置为 `MAVEN_SIGNING_KEY` / `MAVEN_SIGNING_PASSWORD` environment secrets。
- [ ] 验证 Maven POM metadata、sources/javadoc artifacts、checksums 和每个必需 artifact 的 `.asc` signature。
- [ ] 验证 npm organization `nouprax` 的 publish access，并将 `@nouprax/es-markdown-core` 配置为 public scoped package。
- [ ] 完成 npm 首次 bootstrap publish，绑定 `nouprax/markdown-core` 的精确 release workflow/environment 为 trusted publisher，并撤销 bootstrap token。
- [ ] 为 npm publish job 配置 `id-token: write` / `contents: read`，验证 provenance，然后禁用传统 token publishing。
- [ ] 创建受保护 `release` GitHub environment，配置 required reviewer、tag/branch restrictions 和 Maven-only environment secrets。
- [ ] 确保 GitHub Release job 仅在必要时使用 `contents: write` 的 workflow-provided `GITHUB_TOKEN`。
- [ ] 添加不读取 release secrets 的发布 dry-run，验证产物内容、版本、签名、checksums 和 registry metadata。
- [ ] 创建 `docs/releasing.md`，记录认证配置、secret names、轮换、撤销、离线 signing-key 备份和泄漏响应。
- [ ] 建立 changelog、release notes 和发布前检查。
- [ ] 从 clean checkout 运行全量 build/test/conformance/consumer checks。
- [ ] 验证发布产物不包含未预期 public headers、symbols、renderer 或 runtime files。

Acceptance：

- [ ] Phase 19 质量门禁已通过。
- [ ] 同一 release commit 的 release build、registry staging、签名、checksums、provenance 与 dry-run 全部绿色。
- [ ] C、`swift-markdown-core`、`kotlin-markdown-core` 和 `@nouprax/es-markdown-core` 的协调版本可生成并验证。
- [ ] npm OIDC、Maven Portal token/PGP signing 和 GitHub Release 最小权限流程已完成一次端到端验证。
- [ ] 不存在长期、未记录或未受保护的 publish credential。

实施与验收记录见 `docs/migration/2026-07-13-phase-20-release-support.md`。

### Phase 21：最终仓库收尾与 clean-checkout 复验

Tasks：

- [ ] 完成 Git/物理工作区收尾：逐项审查所有 tracked deletions 与 untracked package sources，确保应交付文件全部记录且没有意外删除；此项只审查和记录最终 snapshot，不重开 Phase 17 的实现决策。
- [ ] 在安装依赖前清除 `.build/`、`build/`、`.gradle/`、`.pnpm-store/`、`.swiftpm/`、`.tools/`、`node_modules/`、package `build/dist/.cxx` 等 ignored 产物，并以 `scripts/audit-repository.sh --physical` 拒绝 empty directory、build/cache/dependency/IDE 残留。
- [ ] 使用 `pnpm audit:repository:clean` 重新执行 clean Git snapshot、secret、large-file、symlink/broken-link、file-mode、final-newline、package-coordinate 和 license audits；随后从 clean checkout 安装固定依赖并复跑 root `verify`、public-header/symbol、package-content、test/CI topology、consumer 与 release dry-run checks。
- [ ] 完成开发/CI 环境收尾：README 链接到 `docs/development-environment.md`；该文档以 required、platform-specific、repo-managed/optional 分类列出并固定 C/C++ compiler、CMake/Ninja/pkg-config、Xcode/Swift、JDK、Android SDK/NDK/emulator images、Gradle Wrapper、Maven Wrapper、Node.js、pnpm、Emscripten、Python formatter tooling 等全部依赖、版本来源和验证命令；提供幂等、非交互的 `scripts/init-environment.sh`（至少支持只读 `--check` 与受支持 host 的 `--install`/bootstrap），quality/release CI 在官方 setup actions 后复用同一检查入口，脚本不得安装 Xcode、读取 release secrets 或把全局 Gradle/Maven 当成前置条件。
- [ ] 汇总 Phase 0–20 的关闭状态、已接受例外、远端 CI/ruleset/release evidence 与最终 artifact coordinates，确保没有未归属的 TODO、临时 credential、preview toolchain 或仅存在于本机的验收条件。

Acceptance：

- [ ] Phase 19 质量门禁和 Phase 20 发布支持均已完成。
- [ ] 最终 Git snapshot 经审查，物理 checkout 在依赖安装前无 generated、cache 或 IDE 残留。
- [ ] 统一环境入口能从 clean host 复现 quality/release toolchain。
- [ ] 安装固定依赖后全仓 verify、consumer、package、security 与 release dry-run checks 全绿。
- [ ] README 和关闭报告可作为新 contributor 与发布维护者的唯一入口。

实施与验收记录见 `docs/migration/2026-07-13-phase-21-final-closure.md`。

## 22. 已确定设计决策

以下决策已冻结为首版实施输入，不再作为 setup session 的开放问题：

1. 新 release lineage 从 `1.0.0` 开始；不迁移、保留或归档旧 tags。
2. Swift 仅支持最新两个已正式发布的 iOS/macOS major generations；首版为 iOS 18/26、macOS 15/26。
3. Kotlin 交付 Kotlin Multiplatform library，而非 Android-only AAR；公共 API 位于 `commonMain`，首版覆盖 Android、通用 JVM 和声明的 Kotlin/Native matrix。
4. Swift、Kotlin、ES 的 `Document.parse` 都是同步 API；ES package 负责在该入口可用前完成 WASM 初始化，异步调度由 consumer 决定。
5. AST 保留类型名 `List`；Kotlin consumer 使用 `com.nouprax.markdown.core.List` 或 import alias 解决标准集合重名。
6. 每个 `Markup` 公开完整、非 optional 的 `Scope(start, end)`，不只公开 start position；坐标数值与语义原样继承同一 release 的 native C parser，binding 不建立额外解释层。
7. Consumer 可逐次传入跨端一致的 immutable `ParseOptions`。默认启用 smart punctuation、footnotes、strip HTML comments、table、strikethrough、autolink、task list、formula、dollar/LaTeX formula delimiters 和 directive；source tracking 永远启用。`unsafe`、`github-pre-lang`、`full-info-string` 等 renderer-only flags 不进入新 options。
8. 三端同时公开 typed Visitor 和只读 depth-first Walker API。
9. TypeScript 使用递归 `readonly`，但不实施任何 runtime freeze。
10. C facade 使用 length-delimited UTF-8 string view 与显式 error/free model；不承诺跨 release C binary ABI compatibility，每次 release clean rebuild，不提供兼容 shim。
11. 原生 AST dump 使用 canonical UTF-8 file-tree 文本，以 `├──`、`└──` 和 `│` 直观表达真实 parent/child 关系。每个节点占一行，依次输出 kind、严格 `Scope`、全部 behavior-bearing fields 和 child count；字段顺序由 schema 固定，默认值和 null/empty 状态不得省略。相同 source/options 的 byte-for-byte diff 必须能暴露公共 AST behavior drift；有意变化须人工评审 golden diff，不保留隐藏差异的兼容模式。
12. Directive Markdown source 使用标准 `{key=value}` attribute-list grammar，支持 bare/quoted values、`#id` 与 `.class`；parser 将其规范化为只含 string key/value 的 JSON object string 交给 consumer。duplicate/id/class 依标准合并，禁止 HTML projection 和 nested/non-string JSON values，并由 complexity tests 排除 O(n²) 扫描与去重。

若实现审计发现这些决策无法满足，必须先修改并评审本需求文档，再改变实现方向。
