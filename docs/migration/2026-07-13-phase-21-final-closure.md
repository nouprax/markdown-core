# Phase 21: final repository closure

状态：待 Phase 19 质量门禁与 Phase 20 发布支持完成后执行。

## Boundary

Phase 21 只负责跨阶段最终收尾：审查最终 Git snapshot、移除本机 generated/cache/IDE
状态、从无依赖的物理 checkout 开始复验、完成开发环境 onboarding，并汇总 Phase 0–20
的关闭证据。它不重新设计 Phase 17 的 package/test/public-surface contract，也不阻塞
Phase 19 quality workflow 或 Phase 20 release workflow 的实现与远端验证。

## Tasks

- [ ] 审查所有 tracked deletions、modified files 与 untracked package sources，确保最终 snapshot 只包含有意变更。
- [ ] 在安装任何依赖前移除 build/cache/dependency/IDE 输出，并运行 `scripts/audit-repository.sh --physical`。
- [ ] 记录最终 snapshot 后运行 `pnpm audit:repository:clean`。
- [ ] 使用 `scripts/init-environment.sh` 从 clean host 安装或校验公开文档列出的固定依赖。
- [ ] 复跑 root `verify`、平台 consumers、package/public-surface/security checks 与 release dry run，并把远端 ruleset、CI 和 registry evidence 汇入最终报告。

## Acceptance

- [ ] Phase 19 质量门禁和 Phase 20 发布支持均已完成。
- [ ] 最终 Git snapshot 经审查，物理 checkout 在依赖安装前无 generated、cache、dependency、package-manager 或 IDE 残留。
- [ ] `audit:repository`、`audit:repository:clean` 与 `scripts/audit-repository.sh --physical` 保持各自边界，最终模式不因日常 `verify` 而削弱。
- [ ] 统一环境入口能从 clean host 复现 quality/release toolchain。
- [ ] 安装固定依赖后全仓 verify、consumer、package、security 与 release dry-run checks 全绿。
- [ ] README 和关闭报告可作为新 contributor 与发布维护者的唯一入口。
- [ ] 本机 ignored output 不作为 Phase 19 或 Phase 20 的实现 blocker。
