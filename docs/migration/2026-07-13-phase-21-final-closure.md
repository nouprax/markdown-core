# Phase 21: final repository closure

状态：已关闭。最终 Git snapshot、无依赖物理 checkout、统一环境 bootstrap、全仓本地
复验及 Phase 19/20 远端证据均已完成。

## Boundary

Phase 21 只负责跨阶段最终收尾：审查最终 Git snapshot、移除本机 generated/cache/IDE
状态、从无依赖的物理 checkout 开始复验、完成开发环境 onboarding，并汇总 Phase 0–20
的关闭证据。它不重新设计 Phase 17 的 package/test/public-surface contract，也不阻塞
Phase 19 quality workflow 或 Phase 20 release workflow 的实现与远端验证。

## Tasks

- [x] 审查所有 tracked deletions、modified files 与 untracked package sources，确保最终 snapshot 只包含有意变更。
- [x] 在安装任何依赖前移除 build/cache/dependency/IDE 输出，并运行 `scripts/audit-repository.sh --physical`。
- [x] 记录最终 snapshot 后运行 `pnpm audit:repository:clean`。
- [x] 使用 `scripts/init-environment.sh` 从 clean host 安装或校验公开文档列出的固定依赖。
- [x] 复跑 root `verify`、平台 consumers、package/public-surface/security checks 与 release dry run，并把远端 ruleset、CI 和 registry evidence 汇入最终报告。

## Acceptance

- [x] Phase 19 质量门禁和 Phase 20 发布支持均已完成。
- [x] 最终 Git snapshot 经审查，物理 checkout 在依赖安装前无 generated、cache、dependency、package-manager 或 IDE 残留。
- [x] `audit:repository`、`audit:repository:clean` 与 `scripts/audit-repository.sh --physical` 保持各自边界，最终模式不因日常 `verify` 而削弱。
- [x] 统一环境入口能从 clean host 复现 quality/release toolchain。
- [x] 安装固定依赖后全仓 verify、consumer、package、security 与 release dry-run checks 全绿。
- [x] README 和关闭报告可作为新 contributor 与发布维护者的唯一入口。
- [x] 本机 ignored output 不作为 Phase 19 或 Phase 20 的实现 blocker。

## Final snapshot and bootstrap evidence

2026-07-15 在 macOS arm64 上执行 `git clean -fdX`，移除 `.build/`、`.gradle/`、
`.kotlin/`、`.pnpm-store/`、`.swiftpm/`、`.tools/`、`node_modules/`、root/package build
与 dist 输出以及本机 IDE 状态。安装依赖前的 `scripts/audit-repository.sh --physical`
通过；最终审查没有 tracked deletion 或未记录的 package source。仓库只允许已 tracked
的共享 `.idea/runConfigurations/All_Kotlin_tests.xml`，untracked/ignored `.idea` 内容仍由
physical audit 拒绝。

README 现在链接 `docs/development-environment.md`，`scripts/init-environment.sh` 提供
只读 `--check`、幂等 `--install` 与按 job 选择的 component checks。完整 install 与紧随
其后的第二次 install 均通过；第二次运行复用固定 Android/Emscripten/依赖与 repo-local
工具。bootstrap 过程中还发现旧 macOS ARM daemon 坐标错误指向缺少 JDK 工具的
`OpenJDK21U-jdk` 归档；用 Gradle `updateDaemonJvm --jvm-version=26` 重新生成全平台坐标后，
macOS ARM 坐标已验证指向完整 `OpenJDK26U-jdk` 归档。quality、release dry-run 和正式
release workflow 在各自官方 setup actions 后复用同一 `--check` 入口；该入口不读取
release secrets、不安装 Xcode，也不要求全局 Gradle/Maven。

## Final local verification

固定依赖安装完成且 Git snapshot clean 后，以下入口通过：

- `pnpm audit:repository:clean` 与完整 `scripts/init-environment.sh --check`；
- root `pnpm verify`，覆盖 formatter/linter、Gradle model、version contract、repository、
  CI/test/public-surface/package audits；
- `pnpm check:kotlin-consumers`，覆盖 KMP/JVM Gradle、Android AAR 与 repo-owned Maven
  Wrapper consumer；本轮修复了 macOS Bash 3 在默认 Maven repository 路径展开空数组的
  portability 问题；
- `pnpm release:dry-run`，覆盖 C install archive、Swift source/product consumer、npm
  tarball/types/runtime consumer、Maven/KMP staging、一次性 PGP signing、checksums、
  staged consumers 与 Central bundle audit，全程未读取发布凭据。

Swift formatter 继续输出项目既有的 public-documentation diagnostics，但 formatter 以成功
状态退出，SwiftLint 报告 0 violations；没有新增或隐藏 lint failure。最终 TODO/FIXME 审查
只命中 Phase 17 已归类的历史说明、PGP keyring 随机编码及标准 WASI
`wasi_snapshot_preview1` import name，不存在未归属实现项、preview toolchain 或 placeholder
credential。

## Inherited remote and registry evidence

- Phase 19 的 required CI
  [run 29305643974](https://github.com/nouprax/markdown-core/actions/runs/29305643974)
  与 CodeQL
  [run 29305644011](https://github.com/nouprax/markdown-core/actions/runs/29305644011)
  已证明完整 hosted-runner matrix；active `main quality gates` ruleset 只要求稳定汇总
  `Required gates` 和 `CodeQL gate`。
- Phase 20 的受保护 tag release
  [run 29444753606](https://github.com/nouprax/markdown-core/actions/runs/29444753606)
  与恢复发布
  [run 29447029321](https://github.com/nouprax/markdown-core/actions/runs/29447029321)
  已完成同一 `1.0.2` release lineage 的 artifact staging、签名、consumer、registry 与
  provenance 验证；恢复流程复用已验证 artifacts，没有重跑 build/test matrix。
- 最终公开 coordinates 是
  [`@nouprax/es-markdown-core@1.0.2`](https://www.npmjs.com/package/@nouprax/es-markdown-core/v/1.0.2)、
  [`com.nouprax:kotlin-markdown-core:1.0.2`](https://repo1.maven.org/maven2/com/nouprax/kotlin-markdown-core/1.0.2/)
  和 [GitHub Release v1.0.2](https://github.com/nouprax/markdown-core/releases/tag/v1.0.2)；
  release environment 恢复为只接受 `v*.*.*` tag，失败的 Central deployment 与临时 main
  policy 已删除。

Phase 21 的最终 PR 只验证本阶段新增的 onboarding/JDK/portability 改动；它不以 commit SHA
重新证明 Phase 19/20，也不重新发布 `1.0.2`。
