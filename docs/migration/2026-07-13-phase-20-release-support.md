# Phase 20: release support and release CI

状态：实施中。仓库内版本合同、AGP 9.3 升级、四端 staging、Maven 聚合/签名审计、
无 secret dry-run、正式发布 workflow、发布手册及受保护 GitHub release environment/tag
ruleset 已落地；设备/IDE、远端 dry-run、registry bootstrap、Maven secrets 与 PGP 外部配置仍待完成。

## Boundary

Phase 20 只负责从同一 release commit 构建、验证并协调发布 C、SwiftPM、Maven/KMP 和
npm artifacts。它不重新定义 Phase 18 的共享 AST contract，也不改变 Phase 19 的
required/non-blocking quality gate；最终 Git snapshot、物理 checkout 清理、开发环境
onboarding 和跨阶段总复验属于 Phase 21。

## Tasks

- [x] 准备首个 `1.0.0` release，确认新 repo 不包含旧 tags，且发布文档明确不承诺 C ABI compatibility。
- [x] 对齐 C、SwiftPM、Maven 和 npm 版本，并拒绝任一 artifact version 漂移。
- [ ] 在 AGP 9.3 stable 发布后升级并重新验证 Gradle/Kotlin/Android compatibility matrix；cache-cold model、Android host/device tests、publication 与 consumer checks 必须在 `--warning-mode=fail` 下通过，不得以 preview 工具链首发。
- [x] 验证 SwiftPM source URL、repo-derived identity `markdown-core` 和 product/module `MarkdownCore`。
- [x] 从 `swift package archive-source` 解包到 repo 外目录，验证 root canonical contract、SwiftPM build-tool plugin、provider conformance 和只依赖 `MarkdownCore` product 的独立 consumer；source archive 必须保留测试合同，但 consumer build 不得执行测试插件或携带 derived fixture。
- [x] Maven Central `com.nouprax` namespace 已通过 `nouprax.com` DNS TXT 完成所有权验证。
- [ ] 验证 Maven publication coordinate 为 `com.nouprax:kotlin-markdown-core:<version>`，并确保 KMP root、JVM、Android 和所有 Native target publications 在同一 Central deployment 中齐全；host staging 已通过，完整 Linux/macOS 聚合待远端 dry-run 取证。
- [ ] 验证 POM、Gradle Module Metadata、sources/javadoc artifacts、checksums、signatures、target-specific coordinates 与 native payload 完整一致；本地 metadata/content 审计已通过，完整一次性 PGP 签名审计待远端 dry-run 取证。
- [ ] 从 staged/local Maven repository 运行 KMP Gradle、JVM Gradle Module Metadata、repo-owned Maven Wrapper JVM Maven 与 Android AAR consumers，并在 IntelliJ IDEA/Android Studio 执行 release clean-sync smoke test。
- [x] 创建有过期时间的 Maven Central Portal user token，并通过受保护 `release` GitHub environment 提供 `MAVEN_CENTRAL_USERNAME`/`MAVEN_CENTRAL_PASSWORD`。
- [x] 创建带 passphrase 的 PGP signing key、发布 public key，并通过受保护 environment 提供 `MAVEN_SIGNING_KEY`/`MAVEN_SIGNING_PASSWORD`；key、双 keyserver 发布、environment secrets 和离线 private-key/revocation-certificate 备份已完成。
- [ ] 验证 npm organization `nouprax` 的 public scoped publish access，完成首次 bootstrap publish，将精确 release workflow/environment 绑定为 trusted publisher，并撤销 bootstrap token。
- [x] 为 npm publish job 配置最小 `id-token: write`/`contents: read`，并在 workflow policy 中禁止传统 npm token；实际 OIDC provenance 待 registry bootstrap 后验证。
- [x] 创建受保护 `release` environment，配置 required reviewer、tag/branch restrictions 和 Maven-only secrets；environment、tag-only policy、active ruleset、GitHub Release 最小权限与四个 Maven environment secrets 已完成。
- [x] 添加完全不读取 release secrets 的 release dry-run，验证 artifact contents、versions、metadata、checksums、signatures 和 provenance inputs。
- [x] 创建 `docs/releasing.md`，记录认证、secret names、轮换、撤销、trusted publishing、offline signing-key backup、泄漏响应、changelog、release notes 和发布前检查。
- [ ] 从 clean checkout 运行全量 build、correctness、共享 spec conformance、consumer、package/public-surface/security 和 release dry-run checks。
- [x] 验证 C install、Maven、npm 与 Swift compiled public product 不包含 shared spec corpus、未预期 public headers/symbols、renderer、private extensions library、native handles 或 runtime implementation files；SwiftPM source archive 必须保留 repo-owned tests/specs，且外部 consumer 只构建 product targets。

## 2026-07-14 implementation evidence

- `VERSION` 是 CMake、Gradle 与 npm 的单一版本合同；`release:check-version` 同时校验
  SwiftPM identity/product、consumer 示例、tag namespace 和 `v<VERSION>`。
- AGP 已升级到 9.3.0，Gradle 9.6.1；全新 `GRADLE_USER_HOME` 的 `projects`、Android
  host correctness/conformance、publication 和 staged consumers 均在
  `--warning-mode=fail` 下通过。managed-device 与 IDE clean-sync 仍保留为远端人工证据。
- C install/tarball、Swift source archive、npm tarball、Linux/macOS Maven staging、JNI
  聚合、Central bundle、一次性 dry-run PGP 签名、checksum 和 staged consumer 均有独立脚本；
  正式签名只在 host 聚合完成后进行，避免为旧字节保留 sidecar。
- `.github/workflows/release-dry-run.yml` 无 environment、secret 或 write permission；
  `.github/workflows/release.yml` 仅接受精确 release tag，并将 Maven secrets 限定在
  `release` environment job，将 npm job 限定为 OIDC 权限。
- GitHub environment `release`（id `18166863298`）已启用 `DongyuZhao` required reviewer；
  deployment policy（id `54675963`）只接受 `v*.*.*` tag；`release tag protection`
  ruleset（id `18962304`）active，并将 matching tag 的 create/update/delete bypass 限于
  `DongyuZhao`。组织当前没有 reviewer team，因此暂时允许唯一 operator 自审，待增加第二
  release owner 后切换为 team reviewer 并禁止 self-review。Actions 默认 token 权限为 read，
  四个 Maven environment secrets 已配置且不保存占位 credential；GitHub 默认 admin bypass
  当前仍为 enabled，增加独立 reviewer team 时一并关闭。
- 本地已通过 `pnpm verify`、CI/repository/public-surface/package-content audits、C/npm/Swift
  staging、macOS host Maven audit，以及 KMP、JVM Gradle、JVM Maven Wrapper、Android staged
  consumers。完整跨 host 签名 dry-run 需由新增 workflow 在提交后执行。

## Acceptance

- [ ] Phase 19 required gates 已绿色且 ruleset 已启用；release workflow 只接受受保护 tag/environment，并对同一 commit 生成四端协调版本。
- [ ] C、SwiftPM、Maven/KMP 和 npm staged artifacts 的内容、metadata、checksums、签名与 provenance 全部可独立复验；所有声明的 consumers 从 staged artifacts 实际运行。
- [ ] npm 使用 OIDC trusted publishing，Maven 使用最小范围且有过期时间的 Portal token 与 PGP signing；不存在长期、未记录或未受保护的 publish credential。
- [ ] Release dry-run 不读取 secrets，正式 workflow 的权限按 job 最小化，GitHub Release 只发布经过同 commit 验证的 artifacts。
- [ ] 二进制与安装型发布内容不携带 root shared spec、test corpus、private implementation target、renderer 或任何未在 Phase 16 public-surface allowlist 中批准的文件或符号；SwiftPM source distribution 例外地保留测试合同源，并证明它不进入 consumer product graph。
