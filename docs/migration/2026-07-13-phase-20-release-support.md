# Phase 20: release support and release CI

状态：实施中。仓库内版本合同、AGP 9.3 升级、四端 staging、Maven 聚合/签名审计、
无 secret dry-run、正式发布 workflow、发布手册、受保护 GitHub release environment/tag
ruleset、Maven secrets、PGP 外部配置、npm registry bootstrap/trusted publisher 与 JetBrains
Gradle/KMP clean-import 均已落地；受保护 `v1.0.1` 在产物构建前 fail-closed 且未发布，
协调版本 `1.0.2` 的首次 OIDC provenance 和完整正式 release 仍待完成。

## Boundary

Phase 20 只负责从同一 release commit 构建、验证并协调发布 C、SwiftPM、Maven/KMP 和
npm artifacts。它不重新定义 Phase 18 的共享 AST contract，也不改变 Phase 19 的
required/non-blocking quality gate；最终 Git snapshot、物理 checkout 清理、开发环境
onboarding 和跨阶段总复验属于 Phase 21。

## Tasks

- [x] 以 npm `1.0.0` bootstrap 建立新 release lineage；保留发布前失败的受保护 `v1.0.1`，
  并准备首个四端协调 release `1.0.2`；确认新 repo 不包含旧 tags，且发布文档明确不承诺 C ABI compatibility。
- [x] 对齐 C、SwiftPM、Maven 和 npm 版本，并拒绝任一 artifact version 漂移。
- [x] 在 AGP 9.3 stable 发布后升级并重新验证 Gradle/Kotlin/Android compatibility matrix；cache-cold model、Android host/device tests、publication 与 consumer checks 必须在 `--warning-mode=fail` 下通过，不得以 preview 工具链首发。
- [x] 验证 SwiftPM source URL、repo-derived identity `markdown-core` 和 product/module `MarkdownCore`。
- [x] 从 `swift package archive-source` 解包到 repo 外目录，验证 root canonical contract、SwiftPM build-tool plugin、provider conformance 和只依赖 `MarkdownCore` product 的独立 consumer；source archive 必须保留测试合同，但 consumer build 不得执行测试插件或携带 derived fixture。
- [x] Maven Central `com.nouprax` namespace 已通过 `nouprax.com` DNS TXT 完成所有权验证。
- [x] 验证 Maven publication coordinate 为 `com.nouprax:kotlin-markdown-core:<version>`，并确保 KMP root、JVM、Android 和所有 Native target publications 在同一 Central bundle 中齐全；Linux/macOS host staging 与聚合已由远端 dry-run 验证。
- [x] 验证 POM、Gradle Module Metadata、sources/javadoc artifacts、checksums、signatures、target-specific coordinates 与 native payload 完整一致；完整一次性 PGP 签名审计已由远端 dry-run 验证。
- [x] 从 staged/local Maven repository 运行 KMP Gradle、JVM Gradle Module Metadata、repo-owned Maven Wrapper JVM Maven 与 Android AAR consumers。
- [x] 在 Android Studio Quail 2 2026.1.2 或等价 IntelliJ IDEA 2026.1 执行一次 release
  clean-import smoke test；除 sync 成功外，必须确认 KMP source sets 可见、根 Gradle
  `allKotlinTests` task 可从 IDE 运行；shared `All Kotlin tests` 仅为该 task 的平台无关快捷
  入口，不是 sample app；不重复要求两个使用同一 JetBrains Gradle/KMP importer 的 IDE。
- [x] 创建有过期时间的 Maven Central Portal user token，并通过受保护 `release` GitHub environment 提供 `MAVEN_CENTRAL_USERNAME`/`MAVEN_CENTRAL_PASSWORD`。
- [x] 创建带 passphrase 的 PGP signing key、发布 public key，并通过受保护 environment 提供 `MAVEN_SIGNING_KEY`/`MAVEN_SIGNING_PASSWORD`；key、双 keyserver 发布、environment secrets 和离线 private-key/revocation-certificate 备份已完成。
- [x] 验证 npm organization `nouprax` 的 public scoped publish access，完成首次 bootstrap publish，将精确 release workflow/environment 绑定为 trusted publisher，并撤销 bootstrap CLI session/token。
- [x] 为 npm publish job 配置最小 `id-token: write`/`contents: read`，在 workflow policy 中禁止传统 npm token，并在 registry 要求 2FA 且禁止 token；实际 OIDC provenance 待首次 GitHub Actions publication 验证。
- [x] 创建受保护 `release` environment，配置 required reviewer、tag/branch restrictions 和 Maven-only secrets；environment、tag-only policy、active ruleset、GitHub Release 最小权限与四个 Maven environment secrets 已完成。
- [x] 添加完全不读取 release secrets 的 release dry-run，验证 artifact contents、versions、metadata、checksums、signatures 和 provenance inputs。
- [x] 创建 `docs/releasing.md`，记录认证、secret names、轮换、撤销、trusted publishing、offline signing-key backup、泄漏响应、changelog、release notes 和发布前检查。
- [x] 从 clean checkout 运行全量 build、correctness、共享 spec conformance、consumer、package/public-surface/security 和 release dry-run checks。
- [x] 验证 C install、Maven、npm 与 Swift compiled public product 不包含 shared spec corpus、未预期 public headers/symbols、renderer、private extensions library、native handles 或 runtime implementation files；SwiftPM source archive 必须保留 repo-owned tests/specs，且外部 consumer 只构建 product targets。

## 2026-07-14 implementation evidence

- `VERSION` 是 CMake、Gradle 与 npm 的单一版本合同；`release:check-version` 同时校验
  SwiftPM identity/product、consumer 示例、tag namespace 和 `v<VERSION>`。
- AGP 已升级到 9.3.0，Gradle 9.6.1；全新 `GRADLE_USER_HOME` 的 `projects`、Android
  host correctness/conformance、publication 和 staged consumers 均在
  `--warning-mode=fail` 下通过；CI run
  [29387109813](https://github.com/nouprax/markdown-core/actions/runs/29387109813) 的 Android
  managed-emulator correctness/conformance 也已通过。Android Studio Quail 1 2026.1.1 只支持
  AGP 9.2，升级到支持 AGP 9.3 的 Quail 2 2026.1.2 后完成真实 Gradle sync；Kotlin MPP importer
  请求的 Gradle distribution sources 与 dependency source artifacts 已纳入严格 dependency
  verification。IDE 日志记录 `onSuccess(RESOLVE_PROJECT:2)`、`onImportFinished`，最终 sync
  在 5.559 秒完成。随后从无 `.idea`/project cache 的 Android Studio Quail 2 clean import
  验证 `commonMain`、`commonTest`、`jvmMain`、Android 与 Native source sets 及其 Kotlin source
  roots 可见。根 Gradle `allKotlinTests` 聚合 JVM、Android host、当前主机 Native 的
  correctness/conformance，以及 API 36 4 KB/16 KB managed-device 全量测试；shared
  `.idea/runConfigurations/All_Kotlin_tests.xml` 只调用该 Gradle 入口，不创建 sample app。
  Android Studio 从该 shared entry 实际执行 `allKotlinTests`：host 侧 42 tests passed，
  两台 managed device 各 14 tests passed，最终 `BUILD SUCCESSFUL in 1m 39s`。这只是
  developer aggregate；CI/release 仍分别执行具名 platform correctness/conformance gates。
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
  consumers。
- 2026-07-15 在清除 Gradle outputs、3 个 managed devices 与 clean-import 临时备份后，
  IDE/KMP 修订的本地 CI regression audit 再次通过根 `pnpm verify`；其中 Gradle model、
  CI policy、test topology、repository、public surface 与 package-content checks 全绿。
  clean 后的 Android runtime `assembleRelease` 实际执行 arm64-v8a、armeabi-v7a、x86 与
  x86_64 四个 CMake build，证明 `idea.sync.active` 分支不影响真实构建；最终
  `allKotlinTests --dry-run --warning-mode=fail` 也完整解析成功，未触发远端 workflow。
- `1.0.2` 修正 JVM bundled-native loader 的退出清理顺序，改用 JVM platform library-name
  mapping，并对必须使用绝对路径的 JAR extraction 加入有理由的 scoped lint suppression；
  JVM correctness/conformance、根 `pnpm verify` 与 host release dry-run 均通过。dry-run
  生成并复验 C、Swift source、npm、Maven/KMP staged artifacts、一次性 PGP signatures、
  staged consumers 与最终 SHA-256/SHA-512，未读取 release secrets 或触发远端 workflow。
- 发布候选版本提升到 `1.0.1` 后，本地 host release dry-run 再次通过：C 与 Swift artifacts、
  npm tarball、macOS Maven publications、一次性 PGP 签名/checksum audit，以及 KMP、JVM
  Gradle、Android 和 Maven consumers 均使用 `1.0.1` 成功。对外 GitHub Release 固定读取
  `docs/releases/1.0.1.md`，CI policy 拒绝自动生成 notes 和内部 phase/acceptance 记录。
- 首次受保护 tag 验证在构建前 fail-closed，并暴露了历史 check-run/SHA 查询会把 release
  错误耦合到 PR/main 执行时序。正式 workflow 现从不可变 tag 直接调用完整 reusable CI
  build/test suite，不查询旧 checks、不依赖 CodeQL；普通 CI 只匹配 branches/PR，发布只由
  `v*.*.*` tag 驱动，因此并行 review/merge 不会改变已选定的 release snapshot。该
  `v1.0.1` attempt 未构建、上传或发布任何 artifact，tag 保持不可变，下一协调版本为 `1.0.2`。
- draft PR [#2](https://github.com/nouprax/markdown-core/pull/2) 的 release dry-run
  [run 29386638494](https://github.com/nouprax/markdown-core/actions/runs/29386638494) 在 commit
  `757060ec02f6e48d810ee4be9dc01a3d0333ffa6` 上通过：Linux/macOS C artifacts、Swift source
  archive/product consumer、npm tarball consumer、Linux/macOS Maven staging、跨 host 聚合、一次性
  PGP 签名与审计、KMP/JVM Gradle/JVM Maven/Android staged consumers、可独立复验的 Central
  bundle 和最终 fail-closed gate 全绿；dry-run 未读取 release environment 或 secrets。
- 2026-07-15 通过 npm CLI web authentication 与 security-key 2FA 完成
  [`@nouprax/es-markdown-core@1.0.0`](https://www.npmjs.com/package/@nouprax/es-markdown-core)
  首次 public bootstrap publish；发布前 tarball consumer、`exports.types`、内容清单与
  `npm publish --dry-run` 均通过。npm trusted publisher 已精确绑定
  `nouprax/markdown-core`、`release.yml`、`release` environment，权限仅为 `npm publish`；
  package publishing access 已切换为 require 2FA and disallow tokens。bootstrap CLI
  session 随后通过 `npm logout` 撤销，`npm whoami` 返回 `ENEEDAUTH`。首次正式 OIDC
  publication 的 provenance attestation 仍按 Acceptance 独立跟踪。退出登录后的公开
  `npm view` 已复核 version `1.0.0`、repository metadata 与 registry shasum
  `969853cf63edce7975ec185d73784d6c62e11d06`。
- GitHub
  [CI](https://github.com/nouprax/markdown-core/actions/workflows/ci.yml?query=branch%3Amain)、
  [CodeQL](https://github.com/nouprax/markdown-core/actions/workflows/codeql.yml?query=branch%3Amain) 与
  [release dry-run](https://github.com/nouprax/markdown-core/actions/workflows/release-dry-run.yml?query=branch%3Amain)
  已分别证明 Phase 19 review gates、四端 build/conformance/consumer/package/security matrix，
  以及 artifact staging、一次性 dry-run signing、checksums 和 provenance inputs。正式
  release 不复用这些 run 的 SHA，而是在 tag snapshot 上重新执行 required build/test gate。

## Acceptance

- [x] Phase 19 required gates 已绿色且 ruleset 已启用；release workflow 只接受受保护 tag/environment，并对同一 commit 生成四端协调版本。
- [x] C、SwiftPM、Maven/KMP 和 npm staged artifacts 的内容、metadata、checksums、签名与 provenance inputs 全部可独立复验；所有声明的 consumers 从 staged artifacts 实际运行。
- [ ] npm 使用 OIDC trusted publishing，Maven 使用最小范围且有过期时间的 Portal token 与 PGP signing；不存在长期、未记录或未受保护的 publish credential。
- [x] Release dry-run 不读取 secrets，正式 workflow 的权限按 job 最小化，GitHub Release 只发布经过同 commit 验证的 artifacts。
- [x] 二进制与安装型发布内容不携带 root shared spec、test corpus、private implementation target、renderer 或任何未在 Phase 16 public-surface allowlist 中批准的文件或符号；SwiftPM source distribution 例外地保留测试合同源，并证明它不进入 consumer product graph。
