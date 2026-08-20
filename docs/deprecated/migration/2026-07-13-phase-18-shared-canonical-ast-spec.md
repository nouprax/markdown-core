# Phase 18: shared canonical AST conformance spec

状态：实现与本地可用平台验收完成。远端 Linux x64 和 required-CI 绿色证据仍归
Phase 19，不反向阻塞本阶段共享合同关闭。

## Boundary

Phase 18 新增根级 `specs/canonical-ast/`，它只包含产品级 Markdown/`.ast` 合同数据、
coverage manifest 和维护说明，不包含 runner。C package 继续独占 CommonMark、extensions、
regression、pathological、fuzz 和 robustness 等 parser correctness corpus；共享 canonical
AST spec 不进入普通 `test:<platform>` correctness discovery，也不建立新的 `spec:*` task。

各平台现有 `conformance:<platform>` 入口仍直接调用原生 runner。runner 通过公开
`Document.parse`、公开 immutable AST、公开 Visitor/Walker 和公开 `TreeDumper` 生成文本，
再与同一份 `.ast` byte-for-byte 比较。共享数据是 conformance oracle，不是生产
serialization/transport format，也不得用于构造生产 AST。

## Tasks

- [x] 将 `packages/markdown-core/tests/canonical-ast/` 的 Markdown/`.ast` pairs、README 和 coverage manifest 迁移到根级 `specs/canonical-ast/`，明确其唯一 owner 是跨产品公开 AST contract；C package 不再保留私有副本。
- [x] 冻结 manifest schema 与 case discovery：每个 case 显式列出 Markdown input、expected dump、所需 parse options 和覆盖标签；路径、排序、UTF-8、LF 与 final newline 必须确定，runner 不得各自维护第二份 case 清单或 normalization。
- [x] 使 corpus 覆盖全部 28 种 `Markup`、所有 behavior-bearing fields、enum/boolean/null states、scope coordinates、escaping、child order、empty/populated children 和 table/directive/footnote/formula 等结构；coverage audit 对缺失和未声明覆盖 fail closed。
- [x] 将 C conformance target 改为读取共享 corpus，通过公开 C parse/document dump API 比较全部 cases；CommonMark/extension/regression correctness fixtures 仍留在 C package。
- [x] 将 Swift conformance target 改为对全部共享 cases 调用公开 `Document.parse` 与 `Markup.dump()`/`TreeDumper.dump(_:)`，删除 package-local expected tree literals；macOS 与 iOS Simulator 必须消费同一物理 corpus。
- [x] 将 Kotlin common conformance contract 接入全部共享 cases，删除 package-local expected tree literals；JVM、Android host、repo-managed Android emulator、macOS ARM64 与 Linux x64 原生 targets 必须消费同一物理 corpus。
- [x] 将 TypeScript/ES Node conformance target 改为对全部共享 cases 调用公开 npm API 与 `TreeDumper`，删除 package-local expected tree literals；不得调用 C dump、读取另一 binding 输出或引入 JSON bridge。
- [x] 为 simulator/emulator/native test bundles 配置从根级 spec source 生成的 derived resources；不提交 Swift/Kotlin/ES fixture 副本，不使用越界 symlink，不依赖开发机 current working directory，clean build 与 packaged test bundle 均可定位同一内容。
- [x] 保留各 binding 自己的 focused API unit tests，用于 Visitor exhaustiveness、Walker event semantics、error/ownership/lifetime 等共享 dump 无法表达的行为；不得把 parser correctness corpus 复制进 binding package。
- [x] 提供显式、人工审查导向的 golden maintenance 命令：rewrite 只能由公开 C canonical dump 生成候选 diff，不能在 test/CI 中自动接受结果；schema 或 dump grammar 变化必须在同一 change 更新规范、manifest、goldens、四端实现与验收。
- [x] 更新 topology/public-surface/package audits：允许根级 `specs/` contract data，但禁止其中出现 runner；拒绝 package-local canonical corpus、未清单化 case、发布包携带 spec data、conformance target 缺席或通过空 discovery/skip 获得绿色。
- [x] 更新 Phase 5/7/8/11–17、canonical AST/dump、test architecture 和 repo setup 文档，撤销“C 独占 goldens/binding 只能使用 package-local snapshot”的旧结论，并冻结 correctness、conformance、benchmark 三类入口仍互斥。

## Acceptance

- [x] 根级 `specs/canonical-ast/` 是唯一 canonical Markdown/`.ast` corpus；coverage manifest 完整覆盖 28 种 Markup 与冻结字段合同，仓库不存在平台副本或第二份 case list。
- [x] C、Swift、Kotlin 和 ES 的现有原生 conformance targets 都枚举非空的同一 manifest，使用各自公开 parse/AST/Visitor/Walker/TreeDumper 路径，并对每个 case byte-for-byte 通过。
- [x] Swift iOS Simulator 与 Kotlin Android emulator 的 test bundle 从同一 source 生成资源；clean CI 不依赖 repo cwd、本机 fixture、网络下载或手工复制。
- [x] 普通 correctness targets 不发现共享 spec cases，benchmark 不读取它们；根 manifest 不提供 runner 或新的公共 task route。
- [x] 任意 binding field mapping、Visitor dispatch、Walker hierarchy/order、scope/escaping 或 TreeDumper grammar 的故意破坏都会让对应 platform conformance target 失败。
- [x] Format、lint、canonical coverage、test topology、public surface、package contents 与根级 `verify` 全部通过，且 spec data 不进入 C install、Swift public product、Maven 或 npm 二进制发布产物；SwiftPM source archive 保留测试合同源。

## Implementation

`specs/canonical-ast/manifest.json` 是唯一 discovery source。v1 schema 固定六个
case 的 input、expected、11 个 parse options、manifest order、UTF-8、LF、final
newline，以及 kind/state/order coverage tags。`check-canonical-ast-fixtures.mjs`
从 AST contract 自动提取 28 kinds 和 47 个 behavior-bearing fields，并对缺失、
未知、未清单文件、字段顺序和空 discovery fail closed。

四端消费路径：

- CMake 在 configure 时解析 manifest 并把每个 case 和 option mask 传给
  `facade_test`；公开 C parse/document dump 与 CLI dump 均逐字节比较。
- SwiftPM `GenerateCanonicalASTResources` build-tool plugin 声明 root corpus 为
  inputs，调用 package-owned Swift executable tool，在 plugin work directory 生成
  单一 `canonical-ast-fixtures.json` resource；macOS 与 iOS Simulator 使用同一个
  `Bundle.module` loader，manifest 不再以 `../../..` 声明 target 外 resource。
- Gradle 的 cacheable `GenerateCanonicalAstFixtures` task class 直接解析 manifest，
  生成 build-only `commonTest` Kotlin data；JVM、Android host/device 与
  Kotlin/Native targets 编译同一 generated source。生成任务支持 configuration
  cache，且 test compilation/ktlint 显式依赖它，不再跨工具调用 Node generator。
- ES package 使用 npm/pnpm `preconformance` lifecycle 从 root manifest 生成
  `build/generated/conformance/canonical-ast-fixtures.json`；Node test 只读取 package
  build output，并通过公开 npm `Document.parse`/`TreeDumper` 逐 case 比较，不依赖
  `cwd` 或跨 package 相对路径。

`generate-canonical-ast-candidates.sh` 只通过公开 C CLI 将候选输出写入
`build/canonical-ast-candidates/` 并打印 diff；它不会修改 accepted goldens，且对
尚未支持的非默认 parse options fail closed。

## Defect closed during native verification

Kotlin/macOS 初次复验发现 native executable 仍嵌入旧 `MKC1` bridge archive，而
common decoder 已要求 `MKC2`。根因是 cinterop task 只 `dependsOn` native build，
却未把 archive directory 声明为 input，导致增量构建复用旧 klib。现在 cinterop
显式跟踪 archive；重建后 macOS ARM64 conformance 全绿，decoder 同时给出精确
magic-byte 诊断，避免同类失败只显示泛化错误。

## Verification evidence

- manifest/architecture: canonical coverage、test topology、public surface、
  package contents audits 全绿；npm/Maven/C install/Swift compiled public product 均
  不携带 spec data，SwiftPM source archive 保留可复验的测试合同源。
- C host、Swift macOS、Swift iOS Simulator、Kotlin JVM、Android host、macOS
  ARM64、repo-managed Android API 36 4K/API 37 16K 和 ES Node conformance 全绿。
- Android 两个 managed devices 各运行四个 `AstTest`；Swift build 日志确认
  build-tool plugin 先生成、再把 derived JSON 纳入 test bundle。
- Linux x64 的 task wiring 与 generated common-test input 已冻结；远端 Linux
  required-CI 绿色结果按阶段边界归 Phase 19。
