# 跨平台仓库 Setup 模板

> 适用范围：同一仓库维护一个核心实现及多个平台 binding/package，需要统一 PR quality
> gate、default-branch CI、无密钥 release dry run，以及由受保护 SemVer tag 驱动的协调发布。
>
> 本文只定义仓库控制面和交付合同，不依赖产品名称、源码内容、语言组合、包坐标或 registry。
> 文中的 `<...>` 必须在目标仓库中替换；`scripts/repo` 是目标仓库需要实现的唯一 adapter。

## 1. 模板目标

迁移完成后，目标仓库应满足以下结果：

1. 所有 binding 都消费同一 commit 中的 core，不允许跨版本拼装。
2. correctness、跨端 conformance、consumer 和 package-content 验证彼此独立且全部阻塞 PR。
3. ruleset 只依赖两个稳定汇总 check：`Required gates` 与 `CodeQL gate`。
4. feature branch 只运行 pull request gate；default-branch push、pull request 和 merge queue 使用独立
   concurrency lane，main CI 不抢占 PR required check。
5. 可重复、确定性的 benchmark 作为 Test phase 的 required leaf 执行；具体耗时、内存、binary
   size 等数值只提供观测，不使用 hosted-runner 绝对阈值阻塞合并。
6. release dry run 不读取 environment、repository secret 或长期 signing key。
7. 正式发布只由不可变 `vX.Y.Z` tag 触发，并在 tag snapshot 上重新运行完整 CI。
8. artifact 先构建、审计并经过 staged consumer，再获得发布权限；发布 job 使用最小权限。
9. 所有生态 package 和 GitHub Release 来自相同 tag 和版本；各自发布的字节不从历史 CI run
   拼装。
10. GitHub 控制面可由本文的 bootstrap 脚本重复、幂等地配置。
11. 若仓库需要 owner veto，team approval 使用独立 ruleset；owner bypass 不得连带绕过 CI。
12. Dependency graph 只把真实发布/runtime 依赖呈现为产品依赖；formatter、lint、emulator、
    compiler plugin 等工具链依赖另行审计，不能被合成 manifest 误报成消费者依赖。

本模板抽象自当前仓库的以下已运行结构：

- [CI workflow](../.github/workflows/ci.yml)
- [CodeQL workflow](../.github/workflows/codeql.yml)
- [release dry run](../.github/workflows/release-dry-run.yml)
- [tag release](../.github/workflows/release.yml)
- [CI benchmark/metrics producers](../.github/workflows/ci.yml) 与
  [privileged commenter](../.github/workflows/pr-metrics-comment.yml)
- [default-branch ruleset](../.github/rulesets/main.json)、
  [owner approval ruleset](../.github/rulesets/owner-review.json)、
  [release-tag ruleset](../.github/rulesets/release-tags.json) 与
  [release environment policy](../.github/environments/release.json)

这些文件是设计证据，不是可原样复制的通用模板；其中的语言版本、runner、包坐标、actor ID、
artifact 路径和发布顺序都属于当前仓库 adapter。

## 2. 不变量、adapter 与扩展项

### 2.1 必须保持的不变量

| 不变量 | 原因 |
| --- | --- |
| 单一 commit、单一版本、协调发布 | 防止 core 与 binding 产生不可验证的组合 |
| 同一份 canonical fixtures/spec 驱动所有 binding conformance | 防止各平台测试各自证明自己 |
| 每个平台都有真实 consumer | 单元测试不能证明 package metadata、link、loader 或 exports 可用 |
| required check 使用稳定聚合名 | matrix、runner 和 binding 增减不应要求修改 ruleset |
| 聚合 job 使用 `if: always()` 并显式要求全部依赖为 `success` | cancelled、skipped 和 failure 必须 fail closed |
| PR/default-branch push/merge queue 分开 concurrency | main runner teardown 不得取消或饿死 PR check |
| dry run 与正式 release 使用相同 staging/audit adapter | 避免“测试了一套，发布了另一套” |
| release 从 tag snapshot 自证，不查询旧 check-run | 避免发布正确性依赖可变的历史运行时序 |
| build/stage job 无 publish 权限 | 被构建脚本入侵时仍不能直接发布 |
| release environment 只允许 SemVer tag | 手工 branch workflow 不得获得 release secrets |

### 2.2 目标仓库必须实现的 adapter

所有 workflow 只调用根级 `scripts/repo`。它可以转发给 Make、CMake、SwiftPM、Gradle、
pnpm、Cargo、Bazel 或其他原生命令，但 workflow 不应复制业务逻辑。

```text
scripts/repo doctor --check <capability...>
scripts/repo format --check
scripts/repo lint
scripts/repo test <binding> <target>
scripts/repo conformance <binding> <target>
scripts/repo consumer <binding> <target> [--repository <path>]
scripts/repo audit repository|ci|tests|surface|packages
scripts/repo release check-version [--tag vX.Y.Z]
scripts/repo release stage <artifact-id> <output-dir>
scripts/repo release sign <input-dir> [--ephemeral]
scripts/repo release verify <input-dir> [--signed]
scripts/repo release publish <channel> <artifact-path>
scripts/repo release checksums <release-dir>
```

Adapter 的通用合同：

- 所有命令必须在 repo root 可执行，并使用非交互模式。
- `--check`、test、conformance、consumer、audit 和 stage 成功返回 `0`，失败返回非零。
- 正常 CI、PR 与 dry run 不得读取 publish credential。
- stage 只写指定 output/build 目录，不修改 tracked files。
- stage 产物必须可离线交给 consumer；consumer 不得回退到 workspace 源码。
- `release check-version --tag` 必须接受且只接受 `v<exact VERSION>`。
- 版本检查必须覆盖根版本、全部 package manifest、consumer fixture、release notes 和 artifact
  metadata；任一漂移都失败。
- `release sign --ephemeral` 只能生成一次性 key，完成后销毁；它用于证明签名和 bundle 结构，
  不能证明正式 key 的身份。
- publish 必须消费已经 stage、verify 和 consumer-tested 的字节，不得在 publish job 重新 build。
- 每个 adapter 命令应能在本地运行；GitHub Actions 只是调度器，不是唯一实现。

### 2.3 可按仓库裁剪的扩展项

可以删减不存在的平台、registry 或 sanitizer，但不能删减仍被声明支持的平台。典型扩展项包括：

- ASan、UBSan、TSan、fuzz、ABI/API compatibility。
- iOS Simulator、Android Emulator、Windows、Linux、macOS deployment target matrix。
- npm、Maven Central、PyPI、crates.io、NuGet、GitHub Packages。
- source archive、binary archive、XCFramework、WASM、JNI/native payload。
- benchmark、coverage、binary size 和性能回归 comment。

## 3. 推荐仓库形态

```text
.
├── .github/
│   ├── environments/
│   │   ├── release.json                 # 可审计 recipe，不包含 secret value
│   │   └── release-tag-policy.json
│   ├── rulesets/
│   │   ├── main.json
│   │   ├── owner-review.json            # 可选：owner veto，与 CI ruleset 隔离
│   │   └── release-tags.json
│   └── workflows/
│       ├── ci.yml                       # reusable + PR/main push/merge_group
│       ├── codeql.yml                   # 独立安全 gate
│       ├── dependency-submission.yml    # default-branch/scheduled，受控 product graph
│       ├── pr-metrics-comment.yml       # privileged workflow_run consumer，可选
│       ├── release-dry-run.yml           # PR/manual，无 secret
│       └── release.yml                   # protected tag only
├── docs/
│   ├── releasing.md                     # 人员、密钥轮换、recovery runbook
│   └── toolchains.md                    # 精确工具链与 runner matrix
├── packages/
│   ├── core/
│   ├── <binding-a>/
│   ├── <binding-b>/
│   └── <binding-c>/
├── specs/                               # canonical public contract
├── scripts/
│   ├── repo                             # workflow 唯一 adapter
│   ├── audit-ci-policy                  # 检查本文不变量
│   └── <内部实现脚本>
├── tests/
│   └── consumers/                       # 按实际发布方式消费 staged artifact
├── VERSION                              # 单一 canonical SemVer，推荐
└── <根级 package/build manifests>
```

若生态要求 manifest 位于根目录，例如 SwiftPM tag package，则保留根 manifest；目录形式不是
目标，不变量和命令合同才是目标。

## 4. 跨平台 binding 验证模型

### 4.1 Binding 清单

迁移前填写此表，未填写的 target 不得声称受支持：

| Binding | Host/build target | Correctness | Conformance | Consumer | Release artifact |
| --- | --- | --- | --- | --- | --- |
| `<core>` | `<linux/macos/windows>` | 必须 | canonical source | install/link/run | `<archive/package>` |
| `<binding-a>` | `<targets>` | 必须 | shared fixtures | clean external project | `<registry/source>` |
| `<binding-b>` | `<targets>` | 必须 | shared fixtures | clean external project | `<registry/binary>` |
| `<binding-c>` | `<targets>` | 必须 | shared fixtures | pack/install/import | `<registry/binary>` |

### 4.2 四种证据不得合并

1. **Correctness**：实现自身行为、错误路径、边界和回归。
2. **Conformance**：所有 binding 对同一输入、选项、schema 和 expected output 产生相同语义。
3. **Consumer**：从 staged artifact 安装/解析依赖，完成 build、link/load/import 和最小运行。
4. **Package audit**：检查 allowlist/denylist、metadata、license、checksums、签名和禁止泄露的文件。

`consumer` 不得通过 workspace dependency、composite build、源码相对路径、未发布 target 或
开发机全局缓存绕过 staged artifact。应在临时目录、独立 dependency cache 或显式本地 registry
中运行。

### 4.3 Canonical conformance

共享 conformance 必须至少固定：

- schema/version；
- input bytes 与编码；
- parse/config options；
- 节点或数据结构的顺序、nullability、默认值和错误语义；
- deterministic serialization/dump；
- Unicode、换行、locale 和 platform path 规范化；
- 每个字段的 non-default、empty、null 和边界 fixture。

有意改变公共行为时，同一 reviewed commit 必须同时修改 schema、core、全部 binding、fixtures、
goldens 和 consumer；不得提供静默吞掉 drift 的 normalization。

### 4.4 Host-specific toolchain 与真实 runtime

声明支持一个 target，不代表任意 host 都能构建或运行它。Binding adapter 必须显式记录：

- 哪些 artifact 可以 cross compile，哪些 cinterop/JNI/FFI 步骤需要目标平台原生 toolchain；
- CI runner 的 CPU architecture，以及 emulator/simulator system image 的 ABI、API、image source；
- runtime acceleration 前置条件，例如 Linux Android Emulator 的 x86_64 image 与 KVM；
- 不兼容 host 上是 fail、禁用该 target，还是把工作路由到兼容 runner；
- release matrix 中由哪个原生 host 产出并 consumer-test 每个 native artifact。

例如 KMP target 含 cinterop 时，不应让 Linux 假装生成 macOS klib；可以关闭 klib cross
compilation，前提是 macOS runner 仍真实构建、测试并 stage macOS publication。Android managed
device 也不能只写一个 device profile：workflow 必须安装 DSL 最终解析出的精确 system image，
并在启动测试前验证 host architecture、ABI 和 acceleration。

## 5. PR quality gate

### 5.1 `ci.yml` 触发器与并发

```yaml
name: CI

on:
  workflow_call:
  pull_request:
  push:
    branches: [main]
  merge_group:
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: ci-${{ github.event_name }}-${{ github.event.pull_request.number || github.ref }}
  cancel-in-progress: true
```

不要为了“提前验证”而在所有 feature branch 上同时启用 `push` 和 `pull_request`，否则同一个 PR
commit 会完整验证两次。Main CI 只接受 default-branch push；PR 与 merge queue 使用各自事件。
也不要按 SHA 跨事件去重：main、PR、merge queue 和 reusable release 是不同控制面，一个被取消或
长时间 teardown 的 runner 不能阻止另一控制面启动。

### 5.2 Blocking job 分层

建议将 blocking DAG 固定为 `Health Check → Build → Build Test → Test → Required Gate`。每层内部
按平台并行，层间通过单一、可审计的 barrier 连接：

| 层 | 必须包含 |
| --- | --- |
| Health Check | frozen dependency install、format check、lint、contract audit、repo cleanliness；按 repository/C/ES/Kotlin/Swift 等 scope 并行 |
| Build | 主要 host/compiler/build-mode matrix；只构建产品、deployment targets 与带 source SHA/manifest/digest 的不可变产品 artifact |
| Build Test | 在 Build 后编译/链接 test suites、instrumentation APK、sanitizer trees、conformance fixtures，并验证真实 consumer 与 pack/archive/publication 内容 |
| Test | 每个声明平台的 correctness/conformance/benchmark，以及 sanitizer、race、emulator、browser 等纯 no-build runtime suite |
| Required Gate | fail-closed 聚合 Test 层；保持唯一稳定 ruleset context |

matrix 使用 `fail-fast: false`，让一次 CI 提供完整故障面。runner 和 toolchain 必须在
`docs/toolchains.md` 固定；升级工具链与修改产品行为应尽量分离。

可见 job name 是 pipeline API，不应直接显示 `ubuntu-latest`、`shared=ON`、内部 suite key 或
artifact key。所有 matrix entry 提供独立的 `label`，统一采用
`<Phase> - <Platform> / <Environment> · <Variant> · <Suite>`；执行逻辑继续使用 `os`、`mode`、
`artifact-label` 等机器字段。推荐 phase vocabulary 只有：`Health Check`、`Quality Gate`、`Build`、
`Build Test`、`Test`、`Security Scan`、`Build Release`、
`Assemble Release`、`Release Artifacts - Ready`、`Publish Release`、`Resume Release` 与 `Report`。
稳定 ruleset gates 是兼容性例外，保留既有 API 名。policy audit 必须拒绝 visible name 引用
`matrix.os`、`matrix.suite`、`matrix.compiler` 等实现字段。

Build Test 从 Build artifact 恢复 build tree 时，必须显式覆盖产品阶段的 tests-off 配置，并在上传
测试产物前断言 test inventory 非空。许多原生 runner（包括 CTest）会把“发现零个测试”视为成功；
若不增加 inventory assertion，分层 pipeline 可能出现结构完整但实际没有测试的伪绿。

Hygiene 不只检查空格：需要统一 control-flow braces、include/import 顺序或其他结构规则时，应写入
版本化 formatter 配置并固定 formatter 版本。Runtime job 必须在执行前证明其环境：Android
emulator 安装精确 system image、匹配 host ABI 并验证 KVM；Gradle/编译器等多层封装命令应默认输出
stacktrace 或等价详细诊断，不能让远端失败只剩一行“device/image not found”。
Apple simulator 同样不能硬编码 hosted image 上可能消失的预创建设备。Build Test 应对 generic
simulator destination 构建，Test consumer 再从 `simctl` 发现已安装 runtime/设备；device set 为空时
基于已安装 runtime 创建临时设备。`OS=latest` 和固定手机名称都不是稳定 CI contract。

#### 5.2.1 完整 blocking DAG

迁移时不要把下图压缩成一个不断复用的 runner。箭头表示 artifact/barrier 依赖，不表示后一个 job
可以在 workspace 中继续前一个 job 的进程或缓存：

```text
Health Check - Repository/Core/Binding ...
                    │
                    ▼
         Health Checks - Ready
                    │
       ┌────────────┼────────────┐
       ▼            ▼            ▼
 Build - Core   Build - Binding  Build - deployment contracts
       └────────────┼────────────┘
                    ▼
              Builds - Ready
                    │
  ┌─────────────────┼──────────────────┐
  ▼                 ▼                  ▼
Build Test - suites/APK   Build Test - sanitizer   Build Test - consumers/packages
  └─────────────────┼──────────────────┘
                    ▼
           Build Tests - Ready
                    │
  ┌─────────────────┼────────────────────────────────────┐
  ▼                 ▼                 ▼                  ▼
Test - Correctness  Test - Conformance  Test - Runtime/Sanitizer  Test - Benchmark
                                                               │
                                                               ▼
                                                     Benchmarks - Ready
  └──────────────────────────────┬──────────────────────────────┘
                                 ▼
                           Tests - Ready
                                 │
                                 ▼
             Required gates | Development branch gates
```

`Health Checks - Ready`、`Builds - Ready` 和 `Build Tests - Ready` 是 phase boundary。前一层的所有
producer 全部成功后，下一层才启动；下一层内部所有 leaf 使用独立 runner 并行。`Benchmarks - Ready`
是 Test phase 内的子 barrier，不是另一条可跳过的观测 pipeline。最终的 `Tests - Ready` 必须同时
依赖 correctness、conformance、runtime/sanitizer 和 benchmark barrier。

#### 5.2.2 完整 job inventory

下表是当前多语言参考实现的完整职责清单。迁移者可删除目标仓库不存在的平台，但必须为仍声明
支持的 host、ABI、deployment target 和 runtime 保留等价 leaf；`<...>` 表示内容无关的替换点。

| Phase | 标准可见名称 | 产物或执行合同 |
| --- | --- | --- |
| Health Check | `Health Check - Repository`、`Health Check - <Core>`、`Health Check - <Binding>` | frozen install、format、lint、model/contract/policy audit；不构建 release artifact |
| Health barrier | `Health Checks - Ready` | 要求每个 repository/platform health check 为 `success` |
| Build core | `Build - <Core> / <OS> · <Compiler> · <Linkage>` | tests-off 的 immutable product tree、manifest、digest |
| Build binding | `Build - <Binding> / <Host> · <Publication or Package>` | package/product tree；native payload 由兼容 host 产生 |
| Build contract | `Build - <Binding> / <Deployment Target>` | 只证明声明的 deployment target 可构建，不混入 runtime suite |
| Build barrier | `Builds - Ready` | 原子放行全部 immutable product artifacts |
| Build Test repository | `Build Test - Repository / Package Contents` | 对真实 pack/archive/publication 做递归 allowlist/denylist 和 metadata audit |
| Build Test core | `Build Test - <Core> / <OS> · <Compiler> · <Linkage>` | 从对应 product artifact 构建 correctness/conformance/benchmark test tree，并断言 inventory 非空 |
| Build Test binding | `Build Test - <Binding> / Test Products` | 生成可直接运行且不需要编译器的 test bundle/binary |
| Build Test device | `Build Test - <Binding> / <ABI> · Instrumentation APK` | 一次构建 instrumentation package，供多个独立 device consumers 复用 |
| Build Test safety | `Build Test - <Core> Sanitizer / <ASan|UBSan|TSan>` | 生成已插桩 test tree；不在同一 job 执行 suite |
| Build Test consumer | `Build Test - <Binding> / Consumers` | 从 staged package 验证真实外部 consumer，不允许 workspace fallback |
| Build Test barrier | `Build Tests - Ready` | 要求 package、consumer、全部 test products 和 sanitizer products 为 `success` |
| Test correctness | `Test - <Platform> / <Runtime> · Correctness` | 下载一个 test artifact，在 fresh runner 只运行该 suite |
| Test conformance | `Test - <Platform> / <Runtime> · Conformance` | 对 shared canonical fixtures 运行一个 suite |
| Test device | `Test - <Binding> / <Device Variant> · <ABI> · <Suite>` | 每个 page size/image/suite 一个 emulator lifecycle；禁止 Gradle build/publication |
| Test safety | `Test - <Core> Sanitizer / <ASan|UBSan|TSan>` | 下载已插桩 tree 并执行；失败保留 stacktrace/process/runtime evidence |
| Test benchmark | `Test - <Platform> / <Runtime> · Benchmark` | benchmark 进程 required；只运行预构建 benchmark executable |
| Benchmark barrier | `Benchmarks - Ready` | 要求每个平台 benchmark leaf 成功；metrics JSON 上传失败可 non-blocking |
| Test barrier | `Tests - Ready` | fail-closed 聚合全部 correctness/conformance/device/sanitizer/benchmark 结果 |
| Ruleset API | `Required gates` | 只由 PR/merge queue 产生，ruleset 依赖此稳定 context |
| Main summary | `Development branch gates` | 只由 default-branch push、manual/reusable 非 PR run 产生，不满足 PR ruleset |

一个四端参考实例会展开成：

- Core：Linux Clang shared、Linux GCC static、macOS Clang shared、Windows static；每个配置分别拥有
  correctness/conformance consumers，Linux 另有 ASan/UBSan/TSan 和一个 benchmark consumer。
- Apple binding：一个 product producer、声明的 iOS/macOS deployment-target producers、一个同时
  生成 macOS/iOS test products 的 Build Test producer；macOS/iOS correctness/conformance 和 macOS
  benchmark 分别运行。
- Kotlin/KMP binding：Linux 与 macOS publication producers；Linux x64/macOS arm64 host-test producers；
  JVM、Android host、Linux Native、macOS Native correctness/conformance consumers；一个 x86_64
  instrumentation APK producer服务 `{4 KB,16 KB} × {correctness,conformance}` 四个独立 emulator
  consumers；JVM benchmark 独立运行。
- ES/WASM binding：一个 product package producer、一个 test bundle producer；Node correctness、Node
  conformance、browser correctness 与 Node benchmark 分别运行。

#### 5.2.3 Artifact handoff 合同

Cache 只能加速相同命令，不能承担 phase handoff。每个 producer 上传的 artifact 至少包含：

```text
artifact-manifest.json
  schemaVersion
  sourceSha
  producer
  target/host/architecture
  buildMode/toolchain
  payloads[]
  testInventory[]        # test artifact 必须非空
SHA256SUMS
<product-or-test-payload>
```

Build Test 下载 product artifact 后必须先验证 manifest、`sourceSha` 和 digest；Test 下载 test artifact
后再次验证，并禁止调用 compiler、linker、package publication 或其他 suite。PR artifact 的身份使用
`github.event.pull_request.head.sha`，其他事件使用 `github.sha`；GitHub 为 PR 生成的临时 merge SHA
不能写成 metrics 的 source identity。Artifact name 可以包含 SHA 防止串线，但 concurrency 和 workflow
授权不能只依赖 SHA。

#### 5.2.4 Event 与 pipeline ownership

| Event | 执行内容 | 最终可见 context | Concurrency lane |
| --- | --- | --- | --- |
| `pull_request` | 完整 Health Check → Build → Build Test → Test | `Required gates` | PR number |
| `merge_group` | 与 PR 相同的完整图 | `Required gates` | merge-group ref |
| default-branch `push` | 同一完整图，为 baseline/健康状态产证 | `Development branch gates` | default-branch ref |
| `workflow_call` from release | 在 tag snapshot 上执行同一完整图 | `Development branch gates`，由 release 的 `Quality Gate - Release` 消费 | tag/reusable run |
| feature-branch `push` | 不执行完整 CI | 无 | 无 |
| `workflow_dispatch` | 诊断或显式演练，不得伪造 PR context | `Development branch gates` | dispatch ref |

Release dry run、CodeQL、dependency submission 和 privileged metrics commenter 使用独立 workflow 和
独立 concurrency；它们不能与 PR CI 共用 SHA lane，也不能冒充 `Required gates`。

### 5.3 唯一稳定聚合 check

```yaml
  tests-ready:
    name: Tests - Ready
    if: ${{ always() }}
    needs: [test-c, test-es, test-kotlin, test-swift]
    runs-on: ubuntu-latest
    steps:
      - name: Require every platform test
        env:
          C: ${{ needs.test-c.result }}
          ES: ${{ needs.test-es.result }}
          KOTLIN: ${{ needs.test-kotlin.result }}
          SWIFT: ${{ needs.test-swift.result }}
        run: |
          for result in "$C" "$ES" "$KOTLIN" "$SWIFT"
          do
            test "$result" = success
          done

  required-gates:
    name: ${{ (github.event_name == 'pull_request' || github.event_name == 'merge_group') && 'Required gates' || 'Development branch gates' }}
    if: ${{ always() }}
    needs: tests-ready
    runs-on: ubuntu-latest
    steps:
      - name: Require the complete test layer
        env:
          TESTS_READY: ${{ needs.tests-ready.result }}
        run: test "$TESTS_READY" = success
```

规则：

- 所有 blocking job 都必须可传递到 `Tests - Ready`，每个 barrier 都必须 fail-closed；不能让 skipped
  job 被最终 gate 当成成功。
- 不使用 `contains(needs.*.result, 'failure')` 之类会漏掉 skipped/cancelled 的宽松表达式。
- push 汇总名必须是 `Development branch gates`，不能产生 ruleset 所需的 `Required gates`。
- ruleset 不直接引用 matrix 展开后的 job 名。

### 5.4 CodeQL

CodeQL 独立运行在 `main` PR、`main` push、merge queue 和 schedule。为每种实际产品语言选择
正确的 build mode，并用同样的 fail-closed 聚合：

- C/C++、Java/Kotlin、Swift 等 compiled language 使用 repo-owned manual build，只编译真实产品
  target；必要时禁用 build cache 并强制 rerun，让 CodeQL tracer 看到编译。
- JavaScript/TypeScript 等无需编译的语言使用 `none`。
- GitHub Default Setup 与 repo-owned Advanced Setup 二选一，不能同时产生重复或互相冲突的 analysis。
- 不让 CodeQL autobuild 猜测跨平台 aggregate task；它可能选中不兼容 host target 或只命中 cache。

完整安全扫描参考 inventory 为：`Security Scan - C and C++`、
`Security Scan - Java and Kotlin`、`Security Scan - JavaScript and TypeScript`、
`Security Scan - Swift` → `CodeQL gate`。目标仓库只保留实际产品语言，但 compiled-language leaf
必须各自使用兼容 runner 和明确的 product-only manual build；`CodeQL gate` 必须通过 matrix 的整体
result fail closed，不能逐个把易变 leaf name 加入 ruleset。

```yaml
  codeql-gate:
    name: CodeQL gate
    if: ${{ always() }}
    needs: analyze
    runs-on: ubuntu-latest
    permissions:
      contents: read
    steps:
      - env:
          RESULT: ${{ needs.analyze.result }}
        run: test "$RESULT" = success
```

只有 CodeQL analyze job 获得 `security-events: write`。如果仓库或 GitHub plan 不支持某语言，
必须在启用 active ruleset 前解决，不能把 required `CodeQL gate` 留成永远不会出现的 context。

### 5.5 Default-branch ruleset

`main quality gates` 只要求：

- `Required gates`
- `CodeQL gate`

并启用：

- 禁止删除 default branch；
- 禁止 non-fast-forward；
- 所有变更必须通过 PR；
- strict required status checks，即 PR 必须基于最新 default branch；
- 可按团队政策增加通用 approval count、CODEOWNERS、last-push approval 和 thread resolution。

不要把 benchmark leaf、metrics artifact、coverage、release dry run 或易变化的 matrix job 名加入
ruleset；它们通过稳定的 `Required gates` 间接聚合，ruleset 不感知内部拓扑。

### 5.6 可选的 owner approval gate

如果安全目标是“普通协作者不能互相批准后合并，必须由 owner/核心维护者确认”，不要把指定 team
reviewer 直接塞进 `main quality gates`。Ruleset bypass 对整条 ruleset 生效；owner 为了批准自己的
PR 而 bypass 时，可能连同 required CI 一起绕过。

使用独立的 `owner approval gate`：

- 只包含 `pull_request.required_reviewers`；
- `file_patterns: ["*"]` 覆盖所有变更，team 的 `minimum_approvals` 至少为 1；
- reviewer team 必须对 repo 有显式 Write 权限，否则其批准不计数；
- team 只包含真正拥有 veto 权限的人，不能把普通 contributor 加入；
- bypass actor 使用明确的 owner `User`，并限制为 `pull_request`；
- `main quality gates` 继续独立要求 `Required gates` 与 `CodeQL gate`。

多条 ruleset 会叠加执行，所以 owner 只能绕过 approval gate，仍必须通过主 CI。普通 Write/Maintain
成员即使互相 approve，也不能满足 owner team approval。Repository admin、organization owner 或
拥有 edit-rules 权限的人仍能修改控制面；repo-level ruleset 不能防御最高权限账号本身。

## 6. Main CI 与非阻塞观测

同一 `ci.yml` 同时服务 PR、merge queue、default-branch push 和 release 的 `workflow_call`，但不
接受 feature-branch push。这样避免同一 PR commit 重复执行完整 CI，同时让本地命令、PR、main 和
tag 不会形成四套互相漂移的测试定义。

Main CI 的职责是提供 default branch 健康状态和可追溯证据；正式 release 不查询或复用该 run。

可重复且耗时受控的 benchmark 应在 `Build Test` 生成可执行产物，并在 `Test` 与
correctness/conformance 并行执行。每个平台使用独立 runner，Test consumer 只下载并执行，禁止重新
编译；四端汇入 `Benchmarks - Ready`，再由 `Tests - Ready` fail-closed 聚合。具体 median、memory、
binary size 是 informational data，benchmark 进程异常或 workload 缺失则属于 required test failure。

建议把以下任务放在独立 scheduled/manual workflow：

- 长时间 benchmark campaign、长时间 fuzz；
- coverage trend、binary size；
- dependency freshness；
- 跨版本兼容扫描；
- 不稳定或成本很高但尚未声明为 release support 的平台。

### 6.1 Fork-safe PR metrics

需要在 PR 写 comment 时，必须保持 producer/consumer 权限分离：

1. 主 `pull_request` CI 的 benchmark leaves：`contents: read`，运行不可信 PR 代码，benchmark
   成功与否进入 Test gate；数值收集和小型 JSON upload 使用 `continue-on-error`。
2. 监听主 CI 的 `workflow_run` consumer：可以 `pull-requests: write`，**不 checkout、不执行 PR
   代码**，只把
   artifact 当数据处理。

Privileged consumer 下载前必须校验 artifact name、数量、单文件大小、总大小和 expiry；解析后
还要校验 schema、PR 关联、source SHA、允许的平台/metric 枚举与数值范围。Size/perf diff 只能使用
PR 精确 base SHA 对应的成功 default-branch CI artifact；找不到时明确报告 baseline unavailable，不能
退化到任意“最新 main”。Size diff 可视为确定性字节差异；不同 hosted runner run 之间的 perf/memory
diff 只能作为方向性证据。comment 使用隐藏 marker 更新同一条记录，不重复刷屏。任何 artifact 缺失
或非法只产生 notice/warning，不能阻塞 PR。

### 6.2 Dependency graph 与 Dependabot

Dependency submission 是独立的 supply-chain pipeline，不属于 PR runtime gate，也不能直接把 Gradle
解析到的每个 configuration 当成消费者依赖。GitHub 自动 Gradle submission 会把 build 中解析到的
传递依赖提交到一个合成 manifest；Android lint、emulator/UTP、formatter、compiler plugin、Swift
export 等 configuration 可能因此在 UI 中显示为 `settings.gradle.kts` 的 `direct` dependency。它们
不是 release runtime dependency，但仍是 CI/开发机实际执行的工具代码，不能简单标记为历史残留或
`inaccurate`。

推荐把依赖治理拆为两份证据：

1. **Product dependency graph**：由版本化的 `.github/workflows/dependency-submission.yml` 在
   default branch push、schedule 和 manual dispatch 上生成；只 include 真实 publication/runtime
   configurations，并提交给 GitHub Dependency Graph/Dependabot。
2. **Build-tool dependency audit**：独立 scheduled/manual job 解析 plugin classpath、lint、formatter、
   emulator、test platform 和 compiler/export configurations；输出报告并跟踪 advisory，但不把这些
   组件伪装成发布包依赖。

参考 workflow 骨架：

```yaml
name: Dependency Submission

on:
  push:
    branches: [main]
  schedule:
    - cron: "20 13 * * 1"
  workflow_dispatch:

permissions:
  contents: write

concurrency:
  group: dependency-submission-${{ github.ref }}
  cancel-in-progress: true

jobs:
  product-graph:
    name: Security Scan - Dependencies / Product Graph
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@<approved-pin>
      - uses: actions/setup-java@<approved-pin>
        with:
          distribution: temurin
          java-version: "<version>"
      - uses: gradle/actions/dependency-submission@<approved-pin>
        with:
          dependency-graph-include-projects: "<published-project-regex>"
          dependency-graph-include-configurations: "<runtime-or-publication-config-regex>"

  build-tool-audit:
    name: Security Scan - Dependencies / Build Toolchain
    runs-on: ubuntu-latest
    permissions:
      contents: read
    steps:
      - uses: actions/checkout@<approved-pin>
      - run: scripts/repo audit build-tool-dependencies
```

`include-projects`/`include-configurations` 必须从目标 repo 的实际 Gradle configuration inventory 推导，
不能复制其他仓库的 regex。启用手工 submission 前，先在 GitHub Advanced Security 设置中关闭同生态
的 Automatic Dependency Submission，避免两个 snapshot producer 互相覆盖。提交后下载 workflow
生成的 snapshot JSON，确认 product graph 中没有 lint、test、emulator、formatter 或 plugin
classpath，并确认真实 runtime dependency 没有被误删。

告警处理顺序是：先确认 dependency 属于 product runtime、build tool 还是旧 snapshot；product
runtime 按发布风险修复，build tool 优先升级产生它的 AGP/Kotlin/formatter/test-platform 等上游插件，
不要未经兼容验证强制覆盖内部传递版本；只有 advisory-specific reachability 经过记录后，才使用
`vulnerable code is not used` 或有期限的 tolerable-risk disposition。新 snapshot 已不包含且 lockfile/
plugin graph 也不再解析的组件，才属于可关闭的残留。

## 7. Release dry run

`release-dry-run.yml` 在 PR 和手动触发，顶层只授予 `contents: read`，不得声明 `environment`。

建议顺序：

1. 校验 coordinated version、manifest 和 tag namespace，但不要求当前已存在 tag。
2. 显式证明预期 release secret 环境变量为空。
3. 各 runner 独立 stage 真实 release artifact。
4. 上传 artifact；聚合 job 只下载这些 artifact，不从 workspace 偷取已构建输出。
5. 需要签名的生态使用 disposable key 完成 detached signature、checksum 和 bundle audit。
6. 所有 consumer 从 staged artifact 运行。
7. `Release Dry Run - Ready` 使用 `if: always()`，要求每个 stage/aggregate job 为 `success`。

完整 dry-run inventory 应与正式 release 的 artifact graph 一一对应：

| Job | `needs` | 合同 |
| --- | --- | --- |
| `Health Check - Release Dry Run / Version and Credentials` | 无 | 校验 coordinated version，并证明正式 secret/environment 不可见 |
| `Build Release - <Core> / <Host> · Dry Run` | validate | 每个原生 host 独立 stage、audit product-only artifact |
| `Build Release - <Binding> / <Package> · Dry Run` | validate | 真实 pack/archive/publication，并运行该生态 consumer |
| `Assemble Release - <Registry> / Dry Run` | 该 registry 的跨 host producers | 合并 publication，使用 disposable key 签名，递归审计并再次 consumer-test |
| `Release Dry Run - Ready` | validate + 全部 Build Release/Assemble jobs | `if: always()` 且逐项要求 `success` |

参考四端实例包含 C/Linux、C/macOS、Swift product-only source、ES/npm、Kotlin/Linux publications、
Kotlin/macOS publications 六组并行 producers；只有 Maven 一类需要跨 host 聚合的生态增加
`Assemble Release - Maven Central / Dry Run`。不存在跨 host 合并的 artifact 直接进入最终 barrier，
但仍必须在自己的 producer 内完成 package audit 与 consumer。Dry run artifact names、目录结构和
审计 adapter 应与正式 release 相同，仅 signing identity 和 publish side effect 不同。

Dry run 可以证明 artifact graph、签名机械流程、consumer 和内容审计，但不能证明：

- 正式 registry credential 有效；
- protected environment reviewer 流程有效；
- npm/PyPI 等 trusted publisher 配置正确；
- 正式 PGP 身份或 public key 可检索；
- package name/namespace 的外部 ownership 已完成。

## 8. Tag-based release

### 8.1 Tag、版本与 snapshot

workflow trigger 可以使用 GitHub glob `v*.*.*`，但 glob 不是 SemVer regex，因此 validate job
必须再次执行严格检查：

```text
tag == "v" + VERSION
VERSION == strict SemVer accepted by the repository
all package manifests == VERSION
all consumer fixtures == VERSION
docs/releases/VERSION.md exists and is non-empty
tag commit is reachable from origin/<default-branch>
```

Tag 必须禁止 update 和 deletion。失败的 tag 是不可变 release attempt；修复任何字节都使用新
SemVer，不能移动或复用原 tag。

### 8.2 Release DAG

```text
protected vX.Y.Z tag
        │
        ▼
validate exact tag/version/notes
        │
        ▼
reusable full CI on tag snapshot
        │
        ├──────────────┬──────────────┐
        ▼              ▼              ▼
 Build Release A   Build Release B   Build Release registry packages
        └──────────────┴──────────────┘
                       │
                       ▼
       Assemble Release / sign / audit / consumers
                       │
                       ▼
              Release Artifacts - Ready
                       │
            protected release environment
                       │
                       ▼
          Publish Release in explicit order
                       │
                       ▼
       checksums + provenance + GitHub Release
```

关键规则：

- `quality` 通过 `uses: ./.github/workflows/ci.yml` 在 tag checkout 上运行完整 suite。
- CodeQL 保持 PR/default-branch gate；release 不等待或查询历史 CodeQL run。
- stage job 只有 `contents: read`，不进入 release environment。
- 第一个需要 signing/publish credential 的 job 才进入 `release` environment。
- 所有 registry upload 必须位于 `Release Artifacts - Ready` 之后；assemble job 不得产生外部写入。
- npm/PyPI 等支持的生态优先使用 OIDC trusted publishing，不保存长期 write token。
- 只有 npm publish job 获得 `id-token: write`；只有 GitHub Release job 获得
  `contents: write` 和 `attestations: write`。
- release job 下载已 stage artifact，不能重新 build。
- 先验证所有不可逆操作的输入，再开始第一个 publish。
- 多 registry 无法原子提交，因此必须文档化顺序、partial failure 和 recovery。
- release artifact 必须是 product-only：禁止包含 test/benchmark/fixture/consumer source、测试二进制、
  conformance corpus、JUnit/kotlin-test 等测试依赖。source distribution 也不例外；若生态通过 tag
  直接提供完整仓库源码，应将它与额外上传到 Release 的 product-only source artifact 明确区分。
- JAR/AAR/KLIB/source JAR 等容器必须递归检查 entry；AAR 内嵌 JAR 也必须展开检查。只在顶层文件名
  上搜索 `test` 不能证明产物纯净。

完整 tag-release job inventory：

| Phase | 标准 job | `needs` 与权限边界 |
| --- | --- | --- |
| Health Check | `Health Check - Release / Tag and Versions` | 仅 tag push；strict SemVer、coordinated versions、notes、default-branch ancestry |
| Quality Gate | `Quality Gate - Release` | `needs: validate`；本地复用 `ci.yml`，在 tag snapshot 上完整重跑 |
| Build Release | `Build Release - <Core> / <Host>` | `needs: quality`；并行、只读、product-only artifact |
| Build Release | `Build Release - <Binding> / <Package or Publications>` | `needs: quality`；每个兼容 host 独立 stage 和 consumer-test |
| Assemble Release | `Assemble Release - <Registry>` | 只依赖该 registry 的 host producers；合并、签名、递归审计、staged consumer；除签名 secret 外不得外部写入 |
| Artifact barrier | `Release Artifacts - Ready` | `if: always()`；依赖全部独立 artifacts 与 assembled bundles，逐项要求 `success` |
| Publish stage | `Publish Release - <Registry> / Stage` | barrier 后首次 registry 写入；上传并等待 server-side validation |
| Publish OIDC | `Publish Release - <Binding> / <Registry>` | barrier 后，按显式顺序依赖前一不可逆操作；仅此 job 获取 `id-token: write` |
| Publish commit | `Publish Release - <Registry> / Commit` | 发布已验证 deployment，不上传新字节 |
| Publish GitHub | `Publish Release - GitHub` | 所有 registry 已公开后，聚合原 artifacts、checksums、attestation 和 curated notes |
| Recovery | `Resume Release - Published Artifacts` | 只在独立 manual path；验证 protected tag、source run 和 artifact digests 后幂等补齐 |

正式 release 的 phase boundary 是 `validate → quality → Build Release → Assemble Release →
Release Artifacts - Ready → Publish Release`。Registry-specific publish 可以串行，但 Build Release
必须并行；`Publish Release` 不得包含 compiler、test source 或重新打包步骤。若签名必须使用 protected
secret，`Assemble Release` 可以进入 release environment，但首次外部 registry write 仍必须位于
`Release Artifacts - Ready` 之后。

### 8.3 推荐发布顺序

1. Build 全部 product-only artifacts。
2. 聚合跨 host 产物，签名、递归审计 archive 内容和依赖 metadata，运行 staged consumers。
3. 通过 fail-closed `Release Artifacts - Ready` barrier 后，才允许首次外部写入。
4. 对支持“上传后验证、稍后发布”的 registry 先上传并等待 `VALIDATED`。
5. 发布 OIDC registry package。
6. 发布已验证但尚未公开的 deployment，并等待 `PUBLISHED`。
7. 生成总 release checksum 与 provenance attestation。
8. 使用人工维护的 release notes 创建 GitHub Release。

不要自动生成面向用户的 release notes，也不要把内部 phase、acceptance log 或 CI transcript
写入 release notes。

### 8.4 Recovery

若发布可能在部分 registry 成功后失败，workflow 可以提供手动 recovery，但必须同时要求：

- 已存在且受保护的 `release-tag`；
- 在剩余操作可证明幂等时，优先 rerun 原始 tag workflow 的 failed jobs；只有原 run 无法安全
  恢复时才使用 dispatch；
- 使用 dispatch 时，workflow 的触发 ref 本身必须是 `refs/tags/<release-tag>`，并在 job 中显式
  校验；仅在 step 中 checkout tag 不会改变 environment policy 所看到的触发 ref；
- 产生已验证 artifact 的 `source-run-id`；
- source run 的 workflow、event、head SHA、tag 和 artifact allowlist 全部匹配；overall conclusion
  必须符合已记录的失败阶段，所有 artifact-producing jobs 必须成功；
- artifact 未过期，name/count/size 和 digest 满足策略；
- recovery checkout 使用 tag，而不是 default branch；
- 已发布 registry 通过查询确认相同 version/digest 后跳过，不得 republish；
- 所有继续发布动作仍经过 `release` environment reviewer。

任何 artifact 字节变化、签名变化或版本 metadata 变化都不是 recovery，必须发布新 SemVer。

## 9. 权限与 secret 边界

### 9.1 默认权限

仓库 Actions 默认设置为：

```json
{
  "default_workflow_permissions": "read",
  "can_approve_pull_request_reviews": false
}
```

每个 job 只提升必需权限：

| Job | 最小权限 |
| --- | --- |
| build/test/stage | `contents: read` |
| CodeQL analyze | `contents: read`, `actions: read`, `security-events: write` |
| Product dependency submission | `contents: write`，仅 default branch/schedule/manual workflow |
| Build-tool dependency audit | `contents: read` |
| PR metrics commenter | `actions: read`, `contents: read`, `issues: write`, `pull-requests: write` |
| OIDC registry publish | `contents: read`, `id-token: write` |
| GitHub Release/attestation | `contents: write`, `id-token: write`, `attestations: write` |

### 9.2 Secret 分类

| 类型 | 存放位置 | 规则 |
| --- | --- | --- |
| OIDC trusted publisher | registry 外部配置 | GitHub 不保存 write token |
| Registry token | `release` environment secret | 最小 scope、有过期时间、记录 owner 与轮换日 |
| PGP/private signing key | `release` environment secret | passphrase、离线备份、revocation certificate |
| Public key/certificate | 公开 key server/仓库文档 | 发布前验证可检索 |
| GitHub Release token | workflow `GITHUB_TOKEN` | 不创建长期 PAT |

禁止把 secret value 写入 recipe JSON、repo variable、Gradle properties、`.npmrc`、日志、artifact
或文档。Fork PR、普通 CI、dry run 和 stage job 永远不能获得 release environment secret。

## 10. 一键 GitHub 控制面 bootstrap

### 10.1 前置条件

- 目标 repo 已创建，workflow 和 adapter 已通过至少一次手动/PR 验证。
- 已提交 `ci.yml`、`codeql.yml`、`release-dry-run.yml`、`release.yml`；需要 dependency graph 或 PR
  metrics 时，同时提交 `dependency-submission.yml`、`pr-metrics-comment.yml`。Bootstrap 只绑定这些
  workflow 产生的稳定 contexts，不生成业务相关 YAML。
- 执行者拥有 repo admin 权限，已安装并登录 `gh`，本机有 `jq`。
- 已确定 release reviewer；若只有一个 release operator，不能同时开启 self-review prevention。
- 下方脚本只配置 GitHub 控制面，不上传 secret，不配置外部 registry ownership/trusted publisher。
- 可选 owner veto 只适用于 organization-owned repo；启用时必须确认 team 中每个人都应获得 repo
  Write 权限和全仓 approval 权限。
- 对 organization ruleset 或企业策略有额外要求时，应在脚本中扩展 actor 与 scope。
- GitHub 尚无被本脚本依赖的跨 plan、稳定 API 用于安全切换 Automatic Dependency Submission；若
  采用受控手工 submission，先按第 6.2 节在 Advanced Security 设置中关闭自动 submission，再激活
  版本化 workflow，并把这一步记录在 live-policy 验收中。

### 10.2 Bootstrap 脚本

将下列脚本保存为目标仓库的 `scripts/bootstrap-repository.sh`，review 后执行。它会幂等更新同名
ruleset，创建/更新 `release` environment，并确保存在唯一 release tag deployment policy。

```bash
#!/usr/bin/env bash
set -euo pipefail

: "${GH_REPO:?set GH_REPO=owner/repository}"
: "${RELEASE_REVIEWER:?set RELEASE_REVIEWER to a GitHub login}"

DEFAULT_BRANCH=${DEFAULT_BRANCH:-main}
RELEASE_ENVIRONMENT=${RELEASE_ENVIRONMENT:-release}
MAIN_RULESET_NAME=${MAIN_RULESET_NAME:-main quality gates}
OWNER_REVIEW_TEAM=${OWNER_REVIEW_TEAM:-}
OWNER_REVIEW_BYPASS_USER=${OWNER_REVIEW_BYPASS_USER:-$RELEASE_REVIEWER}
OWNER_REVIEW_RULESET_NAME=${OWNER_REVIEW_RULESET_NAME:-owner approval gate}
TAG_RULESET_NAME=${TAG_RULESET_NAME:-release tag protection}
TAG_PATTERN=${TAG_PATTERN:-v*.*.*}
RULESET_ENFORCEMENT=${RULESET_ENFORCEMENT:-evaluate}
PREVENT_SELF_REVIEW=${PREVENT_SELF_REVIEW:-false}

case "$RULESET_ENFORCEMENT" in
  disabled|evaluate|active) ;;
  *) echo "RULESET_ENFORCEMENT must be disabled, evaluate, or active" >&2; exit 2 ;;
esac

case "$PREVENT_SELF_REVIEW" in
  true|false) ;;
  *) echo "PREVENT_SELF_REVIEW must be true or false" >&2; exit 2 ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

reviewer_id=$(gh api "users/$RELEASE_REVIEWER" --jq .id)
repo_owner=${GH_REPO%%/*}
if [ "$repo_owner" = "$GH_REPO" ]; then
  echo "GH_REPO must use owner/repository form" >&2
  exit 2
fi

owner_review_team_id=
owner_review_bypass_id=
if [ -n "$OWNER_REVIEW_TEAM" ]; then
  owner_review_team_id=$(gh api "orgs/$repo_owner/teams/$OWNER_REVIEW_TEAM" --jq .id)
  owner_review_bypass_id=$(gh api "users/$OWNER_REVIEW_BYPASS_USER" --jq .id)
  # Required team approvals count only when the team has explicit Write access.
  gh api --method PUT \
    "orgs/$repo_owner/teams/$OWNER_REVIEW_TEAM/repos/$GH_REPO" \
    -f permission=push >/dev/null
fi

encoded_environment=$(jq -rn --arg value "$RELEASE_ENVIRONMENT" '$value | @uri')
actual_default=$(gh api "repos/$GH_REPO" --jq .default_branch)
if [ "$actual_default" != "$DEFAULT_BRANCH" ]; then
  echo "default branch is '$actual_default', expected '$DEFAULT_BRANCH'" >&2
  exit 1
fi

upsert_ruleset() {
  local name=$1
  local payload=$2
  local ids
  local count

  ids=$(gh api -H "X-GitHub-Api-Version: 2026-03-10" \
    "repos/$GH_REPO/rulesets" --paginate \
    --jq ".[] | select(.name == \"$name\") | .id")
  count=$(printf '%s\n' "$ids" | sed '/^$/d' | wc -l | tr -d ' ')

  if [ "$count" -gt 1 ]; then
    echo "multiple rulesets named '$name'; refusing ambiguous update" >&2
    exit 1
  elif [ "$count" -eq 1 ]; then
    gh api -H "X-GitHub-Api-Version: 2026-03-10" --method PUT \
      "repos/$GH_REPO/rulesets/$ids" --input "$payload" >/dev/null
  else
    gh api -H "X-GitHub-Api-Version: 2026-03-10" --method POST \
      "repos/$GH_REPO/rulesets" --input "$payload" >/dev/null
  fi
}

jq -n '{
  default_workflow_permissions: "read",
  can_approve_pull_request_reviews: false
}' >"$tmp/workflow-permissions.json"
gh api --method PUT "repos/$GH_REPO/actions/permissions/workflow" \
  --input "$tmp/workflow-permissions.json" >/dev/null

jq -n \
  --argjson reviewer_id "$reviewer_id" \
  --argjson prevent_self_review "$PREVENT_SELF_REVIEW" '{
    wait_timer: 0,
    prevent_self_review: $prevent_self_review,
    reviewers: [{type: "User", id: $reviewer_id}],
    deployment_branch_policy: {
      protected_branches: false,
      custom_branch_policies: true
    }
  }' >"$tmp/release-environment.json"
gh api --method PUT \
  "repos/$GH_REPO/environments/$encoded_environment" \
  --input "$tmp/release-environment.json" >/dev/null

matching_policies=$(gh api \
  "repos/$GH_REPO/environments/$encoded_environment/deployment-branch-policies" \
  --jq ".branch_policies[] | select(.name == \"$TAG_PATTERN\" and .type == \"tag\") | .id")
matching_count=$(printf '%s\n' "$matching_policies" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "$matching_count" -eq 0 ]; then
  jq -n --arg name "$TAG_PATTERN" '{name: $name, type: "tag"}' \
    >"$tmp/release-tag-policy.json"
  gh api --method POST \
    "repos/$GH_REPO/environments/$encoded_environment/deployment-branch-policies" \
    --input "$tmp/release-tag-policy.json" >/dev/null
elif [ "$matching_count" -gt 1 ]; then
  echo "multiple identical release tag policies; clean them up before continuing" >&2
  exit 1
fi

policy_count=$(gh api \
  "repos/$GH_REPO/environments/$encoded_environment/deployment-branch-policies" \
  --jq '.branch_policies | length')
if [ "$policy_count" -ne 1 ]; then
  echo "release environment has $policy_count deployment policies; expected exactly one" >&2
  echo "remove unrelated branch/tag policies before activation" >&2
  exit 1
fi

jq -n \
  --arg name "$MAIN_RULESET_NAME" \
  --arg enforcement "$RULESET_ENFORCEMENT" '{
    name: $name,
    target: "branch",
    enforcement: $enforcement,
    conditions: {ref_name: {exclude: [], include: ["~DEFAULT_BRANCH"]}},
    rules: [
      {type: "deletion"},
      {type: "non_fast_forward"},
      {type: "pull_request", parameters: {
        require_code_owner_review: false,
        require_last_push_approval: false,
        dismiss_stale_reviews_on_push: false,
        required_approving_review_count: 0,
        required_review_thread_resolution: false
      }},
      {type: "required_status_checks", parameters: {
        do_not_enforce_on_create: true,
        strict_required_status_checks_policy: true,
        required_status_checks: [
          {context: "Required gates"},
          {context: "CodeQL gate"}
        ]
      }}
    ],
    bypass_actors: []
  }' >"$tmp/main-ruleset.json"
upsert_ruleset "$MAIN_RULESET_NAME" "$tmp/main-ruleset.json"

if [ -n "$OWNER_REVIEW_TEAM" ]; then
  jq -n \
    --arg name "$OWNER_REVIEW_RULESET_NAME" \
    --arg enforcement "$RULESET_ENFORCEMENT" \
    --argjson team_id "$owner_review_team_id" \
    --argjson owner_id "$owner_review_bypass_id" '{
      name: $name,
      target: "branch",
      enforcement: $enforcement,
      conditions: {ref_name: {exclude: [], include: ["~DEFAULT_BRANCH"]}},
      rules: [{type: "pull_request", parameters: {
        require_code_owner_review: false,
        require_last_push_approval: false,
        dismiss_stale_reviews_on_push: false,
        required_approving_review_count: 0,
        required_reviewers: [{
          file_patterns: ["*"],
          minimum_approvals: 1,
          reviewer: {id: $team_id, type: "Team"}
        }],
        required_review_thread_resolution: false
      }}],
      bypass_actors: [{
        actor_id: $owner_id,
        actor_type: "User",
        bypass_mode: "pull_request"
      }]
    }' >"$tmp/owner-review-ruleset.json"
  upsert_ruleset "$OWNER_REVIEW_RULESET_NAME" "$tmp/owner-review-ruleset.json"
fi

jq -n \
  --arg name "$TAG_RULESET_NAME" \
  --arg enforcement "$RULESET_ENFORCEMENT" \
  --arg pattern "refs/tags/$TAG_PATTERN" \
  --argjson reviewer_id "$reviewer_id" '{
    name: $name,
    target: "tag",
    enforcement: $enforcement,
    conditions: {ref_name: {exclude: [], include: [$pattern]}},
    rules: [
      {type: "creation"},
      {type: "update"},
      {type: "deletion"}
    ],
    bypass_actors: [{
      actor_id: $reviewer_id,
      actor_type: "User",
      bypass_mode: "always"
    }]
  }' >"$tmp/release-tag-ruleset.json"
upsert_ruleset "$TAG_RULESET_NAME" "$tmp/release-tag-ruleset.json"

echo "Repository control plane reconciled with enforcement=$RULESET_ENFORCEMENT"
```

首次迁移使用 evaluate 模式：

```sh
GH_REPO=<owner/repo> \
RELEASE_REVIEWER=<login> \
OWNER_REVIEW_TEAM=<team-slug> \
OWNER_REVIEW_BYPASS_USER=<owner-login> \
RULESET_ENFORCEMENT=evaluate \
scripts/bootstrap-repository.sh
```

不需要 owner veto 时省略两个 `OWNER_REVIEW_*` 变量，脚本不会创建或删除 owner gate。提供 team
slug 会显式授予该 team Write 权限；这不是只读 reviewer 标签，执行前必须审计 team membership。

完成第 12 节的远端验证后，用同一命令把已配置 ruleset 切换到 active 目标状态：

```sh
GH_REPO=<owner/repo> \
RELEASE_REVIEWER=<login> \
OWNER_REVIEW_TEAM=<team-slug> \
OWNER_REVIEW_BYPASS_USER=<owner-login> \
RULESET_ENFORCEMENT=active \
scripts/bootstrap-repository.sh
```

如果 GitHub plan 不提供 evaluate 模式，首次迁移使用 `RULESET_ENFORCEMENT=disabled`，完成验证后
直接切换为 active。如果只有 reviewer 本人可以发布，`PREVENT_SELF_REVIEW=true` 会造成死锁；
有独立 reviewer/team 后再开启。脚本不会删除未知 deployment policies，而是在发现额外策略时
fail closed。

## 11. 外部 registry 一次性设置

这一部分不能由 GitHub repo bootstrap 安全代办。

### 11.1 每个 registry 都要记录

- namespace/package owner 与证明方式；
- 发布 workflow 精确文件名、repo、environment；
- credential/trusted publisher 的权限与过期时间；
- signing key UID、fingerprint、expiry、public key URL；
- offline backup 与 revocation certificate owner；
- token/key 轮换和泄漏处置 runbook；
- 首次 bootstrap publish 是否需要人工 2FA，以及完成后如何撤销临时 session/token。

### 11.2 Environment secrets

只为不支持 OIDC 的 registry 设置 secret，且使用目标仓库自己的名称：

```sh
gh secret set <REGISTRY_USERNAME> --repo <owner/repo> --env release
gh secret set <REGISTRY_PASSWORD> --repo <owner/repo> --env release
gh secret set <SIGNING_PRIVATE_KEY> --repo <owner/repo> --env release
gh secret set <SIGNING_PASSWORD> --repo <owner/repo> --env release
gh secret list --repo <owner/repo> --env release
```

不要在 shell command line 直接展开 secret；让 `gh secret set` 从交互输入或 stdin 读取。文档只记录
secret name，不记录 value。

## 12. 迁移与验收 Runbook

### 12.1 本地迁移

- [ ] 填写 binding/target/artifact 清单。
- [ ] 实现 `scripts/repo` 全部适用命令。
- [ ] 固定 toolchain、wrapper、lockfile 和 runner。
- [ ] 从 clean checkout 运行 format、lint、correctness、conformance 和 consumer；C/C++ formatter 若要求
      control-statement braces，必须由 formatter 配置和 CI diff check 自动执行。
- [ ] 记录每个 native/cinterop target 的兼容 host，并确认 release matrix 在原生 host 构建它。
- [ ] 从 staged artifact 运行 consumer，证明没有 workspace fallback。
- [ ] 运行 package audit，检查 licenses、metadata、allowlist/denylist。
- [ ] 运行 release dry run，证明 release secrets 为空且 disposable signing 成功。
- [ ] 审计 workflow permissions、trigger、concurrency、stable gate names 和 release DAG。
- [ ] 枚举 product runtime/publication 与 build-tool Gradle configurations；若使用手工 dependency
      submission，关闭同生态自动 submission 并检查 snapshot JSON。

### 12.2 远端 evaluate 验证

- [ ] 以 `RULESET_ENFORCEMENT=evaluate` 执行 bootstrap。
- [ ] 创建测试 PR，确认只有 PR run 产生 `Required gates`。
- [ ] 向已打开 PR 的 feature branch push，确认只产生 `pull_request` CI，不产生 `push` CI。
- [ ] 合并测试 PR，确认 default-branch push 产生 `Development branch gates`。
- [ ] 确认 `Required gates` 覆盖每个 blocking job，并在 job cancelled/skipped 时失败。
- [ ] 确认全部 CodeQL language 成功后才产生绿色 `CodeQL gate`。
- [ ] 对 compiled language 验证 CodeQL manual build 实际重编产品；关闭会重复或错误 autobuild 的
      Default Setup，Java/Kotlin 不得只命中 build cache。
- [ ] Android build producer 只生成目标 ABI APK、manifest 与 digest；四个独立 test consumers 分别覆盖
      4K/16K correctness/conformance，只安装单一 image 并验证 architecture、KVM 与 acceleration，且
      consumer 不含 Gradle/JDK/NDK/CMake/publication；失败时上传 emulator、adb 与 instrumentation evidence。
- [ ] 若使用 merge queue，实际触发一次 `merge_group` 并确认两个 required contexts。
- [ ] 若启用 owner gate，用非 owner PR 证明只有指定 team approval 能解锁；用 owner PR 证明只能
      bypass approval gate，`Required gates` 与 `CodeQL gate` 仍然必须通过。
- [ ] 确认 benchmark leaves 进入 `Tests - Ready`，但 metrics leaf 名和 dry-run 不在 ruleset 中。
- [ ] 下载四端 metrics JSON，确认 `sourceSha` 是 PR head；基线只能来自 PR 精确 base SHA 的成功
      default-branch CI，首次迁移没有 baseline 时必须明确报告 unavailable。
- [ ] 确认 product dependency graph 不含 lint、formatter、emulator、test platform、compiler plugin，
      build-tool audit 又能单独枚举并跟踪这些实际执行的工具依赖。
- [ ] 确认 fork PR 无 write token、environment secret 或 privileged code execution。
- [ ] 运行远端 release dry run，下载并独立检查 artifact。

### 12.3 Active 与 release 演练

- [ ] 用 `RULESET_ENFORCEMENT=active` 再执行 bootstrap。
- [ ] 证明绕过 PR、缺失 required check、过期 branch 和 force-push 都被阻止。
- [ ] 审计 owner-review team membership 与 repo 权限；确认普通 Write/Maintain actor 不在 bypass list。
- [ ] 证明非 SemVer tag 即使匹配宽松 glob，也会在 validate job 失败。
- [ ] 证明 release environment 只有一个 `v*.*.*` tag policy。
- [ ] 证明普通 branch/manual workflow 不能获得 release environment。
- [ ] 使用未发布测试版本或 disposable registry 完成一次端到端演练。
- [ ] 验证 registry provenance、PGP signature、SHA-256/SHA-512 和 GitHub attestation。
- [ ] 验证 GitHub Release、各 registry 与 source tag 指向相同 commit/version/digest。
- [ ] 演练 partial failure recovery；确认 recovery 以受保护 tag 为触发 ref，且不会临时放宽
      environment 到 default branch、移动 tag、覆盖版本或重建 artifact。

### 12.4 Live policy 查询

```sh
gh api repos/<owner>/<repo>/actions/permissions/workflow
gh api repos/<owner>/<repo>/rulesets
gh api repos/<owner>/<repo>/rules/branches/main
gh api repos/<owner>/<repo>/teams
gh api repos/<owner>/<repo>/environments/release
gh api repos/<owner>/<repo>/environments/release/deployment-branch-policies
gh secret list --repo <owner/repo> --env release
```

Checked-in JSON 是 recipe，GitHub live API 才是实际 enforcement。每次 ruleset、reviewer、tag policy、
workflow permission 或 secret name 变更，都应同步更新 recipe、CI policy audit 与本 runbook。

## 13. 必须由 CI 自审的策略

`scripts/repo audit ci` 至少检查：

- `ci.yml` 同时声明 `workflow_call`、`pull_request`、仅 default branch 的 push、`merge_group`。
- concurrency 包含 event name 和 PR number/ref，且 `cancel-in-progress: true`。
- PR/merge queue 的稳定名为 `Required gates`，push 名为 `Development branch gates`。
- 聚合 job `if: always()` 且覆盖全部 blocking dependencies。
- `CodeQL gate` 存在且 fail closed。
- ruleset required contexts 精确等于 `Required gates`、`CodeQL gate`。
- 若启用 owner veto，独立 ruleset 只含 team reviewer rule，team 至少要求一次全仓 approval，且
  bypass actor 精确为 owner `User` 的 `pull_request` bypass；主 CI ruleset 不包含该 reviewer。
- Android emulator job 固定 runner architecture，安装精确 system images，验证 acceleration，并让
  Gradle device-test adapter 启用 stacktrace。
- 含 cinterop 的 KMP target 禁止伪跨编译时，release/CI 必须在兼容原生 host 提供等价构建证据。
- CodeQL compiled-language entry 使用与项目相符的 manual build；Default/Advanced Setup 不重复运行。
- formatter policy 包含项目要求的结构规则，例如单行 `if/else/for/while` 自动补 braces。
- release dry run 无 environment、无 write permission、无 secret reference。
- release 只接受 tag push；若保留 recovery dispatch，其 job 与正常 publish DAG 严格隔离。
- release validate 严格检查 tag/version/notes/default-branch ancestry。
- release 从本地 reusable CI 运行 tag snapshot，不查询历史 check-runs。
- build/stage 不在 release environment，publish job 才进入。
- OIDC、contents write、attestations write 只出现在需要的 job。
- tag ruleset限制 creation/update/deletion，environment 只接受一个 SemVer tag policy。
- workflow action 引用使用组织批准的 pinning 策略，并由依赖更新工具维护。
- product dependency submission 只在 default branch/schedule/manual 运行，include 规则来自实际
  publication/runtime configurations；同生态不存在互相竞争的自动与手工 snapshot producer。
- metrics producer 的 `sourceSha` 使用 PR head 或当前非 PR SHA；privileged commenter 只接受精确 base
  SHA baseline，不接受临时 merge SHA 或任意最新 main。

策略 audit 应验证安全结果和数据流，不应僵化无关的 YAML 排版、job 顺序或当前 Action major。

## 14. 弯路与设计理由

本节记录的是从真实 setup、PR 验证和 release 演练中得到的因果经验。迁移者可以替换具体
toolchain 和平台，但不应在没有等价证据的情况下删除这些约束。

后续新增经验时，应记录发生日期/阶段、可观察症状、根因、被否决的表面修复、永久规则和回归
验证；详细 run/PR 可以留在项目自己的 migration/ADR 文档，本模板只保留可跨仓复用的结论。
不得在经验记录中复制 token、private key、内部 credential command 或其他 secret。

### 14.1 共享语义合同，不强求 binding 外形对称

**走过的弯路**：为了让多个 binding 看起来统一，把一个平台的目录、API、构建、异常、内存或
发布习惯直接复制到其他平台。这样通常能较快通过 workspace 单元测试，却会在 IDE import、
deployment target、package metadata、native loader、外部 consumer 或 registry publication
阶段失败。

**形成的规则**：跨端只统一公共语义、类型含义、nullability、错误分类、fixture 和版本；每个平台
的公开 API、构建和发布外形必须遵守该生态的最佳实践。

| 平台族 | 应遵守的平台原生实践 | 不能用什么替代 |
| --- | --- | --- |
| C/C++ | CMake configure/build/install/export、CTest suite、shared/static、compiler/OS matrix、sanitizer、真实 link consumer | 只在源码树内 link 私有 target |
| Swift/Apple | SwiftPM package/product identity、声明 deployment target、Swift 原生 value/error/concurrency 模型、macOS/iOS Simulator、外部 package consumer | 把 C pointer/lifetime 暴露给用户，或只在 macOS host 跑测试 |
| Kotlin/KMP | repo-owned Gradle Wrapper、Java toolchain、KMP target publications、Gradle Module Metadata、Android/JVM/Native consumer、IDE model smoke、按 task 延迟 signing/publish 配置 | 只证明 JVM unit test，或让 IDE sync 读取发布 credential |
| ES/TypeScript/WASM | `npm pack` 后安装、严格 `exports`/types/files、Node 与 browser runtime、WASM loader、ESM 语义、OIDC trusted publishing | workspace link、直接运行源码或只做 TypeScript compile |

**验证方法**：每个声明支持的平台至少提供一个使用其标准 package manager/build system 的 clean
consumer；consumer 只能看到 staged/public artifact，不能看到 repo 私有 target 或源码相对路径。

### 14.2 SHA 是审计身份，不是调度身份

**走过的弯路**：本模板来源仓库实际尝试过用 commit SHA 跨 `push`、`pull_request`、
`merge_group` 和 tag event 去重，也尝试过让 release 根据 tag SHA 查询历史 PR/main checks。
结果形成了两类循环：

1. 相同 SHA 的 push run 被取消后仍在 `always()` 汇总或 runner teardown，PR run 因共用
   concurrency group 无法启动，而 ruleset 又等待 PR required check。
2. Release 要求 tag SHA 已有某个 branch/PR check，但 tag event 自身不产生该 context；tag 又是
   启动 release 自证的前提，于是验证依赖一个可能不存在、已过期或属于另一 event 的历史 run。

**形成的规则**：

- SHA 必须写入 artifact metadata、provenance、attestation 和日志，用于证明“验证了哪些字节”。
- SHA 不得作为跨 event concurrency key；使用 `event_name + PR number/ref`。
- Feature branch 不运行 push CI；该 commit 由 `pull_request` 或 `merge_group` 验证，避免重复计算。
- PR/merge queue 才拥有 `Required gates`；push 使用不同的 `Development branch gates`。
- Tag release 从该不可变 tag 直接调用 reusable CI，在当前 snapshot 上重新验证。
- CodeQL 等 merge-time gate 可以留在 PR/main；release 不通过 SHA 查询或等待其历史 run。
- Recovery 可以引用 source run，但必须验证 workflow、event、tag、head SHA、conclusion、artifact
  digest 和 allowlist，而不是“SHA 相同就信任”。

**验证方法**：向已打开 PR 的 feature branch push，只应产生 `pull_request` run；合并后同一 workflow
通过 default-branch push 产生 main CI。主动取消 main、PR、merge queue 或 reusable release 中的一条
run，不应影响其他事件 lane。创建 release candidate 时，隐藏或删除历史 branch check 不应影响 tag
snapshot 的完整 build/test，但 source tag、artifact provenance 仍必须显示精确 SHA。

### 14.3 Stable gate name 是仓库的控制面 API

**走过的弯路**：ruleset 直接引用 matrix leaf jobs，或者 push 和 PR 都产生同名 required context。
增加一个 runner、重命名 binding、matrix fail-fast 或 push cancellation 都可能让合并永久阻塞，
也可能让错误 event 的绿色 check 被误当成 PR 证据。

**形成的规则**：ruleset 只认识 `Required gates` 与 `CodeQL gate`。Leaf jobs 可以演进，但聚合 job
必须 `if: always()`，并要求每一个 blocking dependency 精确为 `success`。稳定 check 名相当于
workflow 与 GitHub ruleset 之间的 API，改名必须按 breaking control-plane change 处理。

**验证方法**：让一个 leaf job 分别 failure、cancelled 和 skipped，三种情况都必须留下失败的稳定
gate；新增 matrix entry 后无需修改 live ruleset。

### 14.4 Build 成功不等于 package 可消费

**走过的弯路**：只执行 workspace build/unit tests，默认认为 package、archive 或二进制一定可用。
这会漏掉 exports、POM/module metadata、source archive、headers、pkg-config/CMake export、native
payload、loader、license 和错误打包私有文件等问题。

**形成的规则**：每个生态都要先 stage/pack/publish 到隔离本地 repository，再从 clean consumer
安装、build/link/load/import 并执行最小公共 API。Package-content audit 与 consumer 是 blocking
证据，不能由 unit test 替代。

**验证方法**：临时移除 workspace/composite dependency 和开发缓存；若 consumer 还能从 staged
artifact 成功运行，并且 package audit 只看到 allowlist 内容，才算通过。

### 14.5 IDE/model load 也是交付面

**走过的弯路**：命令行 compile 绿色就认为 Gradle/KMP、Xcode/SwiftPM 或其他 IDE 工程已正确。
实际上 eager native build、configuration-time signing、缺失 SDK、preview toolchain warning 或读取
release secret 都可能只在 clean import/model load 暴露。

**形成的规则**：IDE sync/model load 必须无 credential、无已构建 native binary、无 publish task
执行即可成功。昂贵 packaging、signing 和 upload 只能在显式 execution task 中配置和执行。发布前
要用支持的稳定 IDE/toolchain 做一次 clean import，并在 CI 保留 headless model smoke。

### 14.6 Dry run 必须逼真，但不能借用正式权限

**走过的弯路**：dry run 进入正式 environment、读取真实 secret，或只调用 registry 的
`--dry-run` 而没有真实 stage、签名、聚合和 consumer。前者扩大 PR 权限面，后者无法证明待发布
字节。

**形成的规则**：dry run 与 release 共用 staging/audit adapter，使用 disposable signing key，
完整生成 artifact、checksum、signature 和 consumer evidence，但顶层只有 `contents: read`，没有
environment 和 secret。Registry ownership、正式 key 身份和 credential 另行演练。

### 14.7 不可信计算与写权限必须分两段

**走过的弯路**：为了给 PR 写 benchmark/size comment，在有 write token 的 workflow 中 checkout
或执行 PR 代码，或者直接信任 PR 上传的 artifact 内容。

**形成的规则**：只读 `pull_request` producer 执行不可信代码；`workflow_run` consumer 只解析
经过 name/count/size/schema/SHA/PR association allowlist 校验的数据，不 checkout、不执行任何
PR 或 artifact 内容。Metrics 永远不进入 required gate。

### 14.8 Release 失败是不可变历史，不是可覆盖草稿

**走过的弯路**：在部分 registry 成功后重新 build、移动 tag、覆盖相同 version，或让 recovery
从 default branch 获取新脚本/新字节。另一个实际出现的陷阱是：从 `main` 触发
`workflow_dispatch`，然后在 step 中 checkout release tag；GitHub environment 仍按 dispatch 的
触发 ref 判断 policy，不会把 checkout ref 当成 tag。为了让 recovery 通过而临时允许 `main` 进入
release environment，会扩大 credential 暴露面。这些做法都会使 provenance、checksum、权限边界
和不同 registry 的内容失去共同身份。

**形成的规则**：失败 tag 保留为不可变 release attempt。Recovery 只复用已验证 artifact；已发布
registry 先核对 version/digest 后跳过。剩余 jobs 可证明幂等时优先 rerun 原始 tag run；必须
dispatch 时，用受保护 tag 作为 workflow dispatch ref，并校验
`github.ref == refs/tags/<release-tag>`。不要临时向 default branch 开放 release environment。任何
byte、metadata 或签名变化都使用新 SemVer。多 registry 发布顺序和不可逆点必须在首次正式发布前
演练。

### 14.9 策略审计应冻结结果，不应冻结偶然实现

**走过的弯路**：policy audit 对 Action major、YAML 行顺序、job 排版或暂时的 toolchain 名称做硬
编码。正常依赖升级会被误报，而真正的权限、trigger、data flow 或 gate 漂移反而可能未被发现。

**形成的规则**：audit 检查安全结果：触发器、权限、environment/secret 边界、stable gates、
fail-closed 聚合、tag snapshot 自证、artifact 流和 live ruleset contexts。Action 引用按组织批准的
pinning 策略管理，但不把当前 major 当成永恒业务规则。

### 14.10 Ruleset 应最后激活

**走过的弯路**：workflow 尚未在远端产生稳定 context 就启用 active ruleset，或者唯一 release
operator 同时开启 prevent self-review，导致 default branch 或 release environment 自锁。

**形成的规则**：先合入 workflow，使用 evaluate/disabled 模式跑真实 PR、merge queue 和 dry run，
再启用 active enforcement。Reviewer、bypass actor 和 self-review policy 必须保证至少存在一条经过
审查的恢复路径。

### 14.11 平台 CI 必须固定执行边界并留下可诊断证据

**Android managed device**：曾出现 Gradle 只报告“找不到 image/device”，但 workflow 安装的 SDK
package、managed-device DSL 最终选择的 ABI、runner architecture 和 AGP setup task 输入并不一致。
设备名称存在不代表对应 image 存在。永久规则是显式映射 host architecture 到 tested ABI，安装精确
API、image source、page-size variant 与 ABI，启动前验证 KVM/acceleration，并让 Gradle device-test
adapter 带 `--stacktrace`。如果插件版本没有把 DSL 属性传入 setup task，只能使用版本受限、带注释
和远端双设备回归的 workaround，不能靠反复 rerun 掩盖。

更深一层的错误是只把两个 emulator 拆成 matrix，却仍让每个 leaf 重复 Gradle build、JNI/全部 ABI、
publication、device setup、correctness 和 conformance。那只是移动故障，不是隔离故障域。永久规则是
host-specific build producers 先并行生成带 manifest/digest 的不可变 test products；然后按
一个显式 artifact-ready barrier 原子放行，再按 platform/device/image/page-size/suite 建独立
`fail-fast: false` consumers。consumer 只能下载 artifact、
启动运行环境并调用 no-build runner；不得出现 compiler、publication 或其他 suite。Android 应由一个
x86_64 APK producer 服务 `{4K,16K} × {correctness,conformance}` 四个无 snapshot consumers；Swift
使用 `--skip-build`/`test-without-building`，C 传递 CTest tree，Kotlin 直接运行 JVM/Native test
products，ES 只运行预构建 dist。cache 不能代替 artifact，单一 runner 也不能跨 host 伪造 native
build。失败后保存 emulator/process/logcat/instrumentation 或对应原生 runner evidence。Ruleset 仍只
引用稳定聚合 gate，不直接引用可演进的 leaf names。

**KMP cinterop**：Linux 配置阶段尝试为 macOS cinterop target 生成 klib 会产生 cross-compilation
misconfiguration；这不等于 Android/JVM artifact 已损坏，也不能通过删除 macOS target“修复”。禁止
不受支持的 klib cross compilation，并在 macOS runner 真实 build/test/stage 该 target；release
consumer 必须证明聚合 publication 同时包含各 host 应负责的 payload。

**CodeQL compiled language**：autobuild 可能选择错误 Gradle task、复用 cache，或触发与产品无关的
cross-platform aggregate build。一个仓库只能有一套有意设计的 CodeQL setup；compiled language
使用 manual build，Java/Kotlin 等需要 `--no-build-cache --rerun-tasks` 或等价机制证明 tracer 看到了
真实编译，脚本语言才使用 `none`。Default Setup 与 repo-owned Advanced Setup 不应重复启用。

**C/C++ hygiene**：只运行默认 clang-format 不会自动建立“所有单行控制语句必须有 braces”的结构
规则。将 `InsertBraces: true` 或目标 formatter 的等价选项写入版本化配置，固定支持该选项的 formatter
版本，对整个受管源码集机械格式化，并让 CI 运行 format check。不要依靠 reviewer 逐行指出漏掉的
`if/else/for/while` braces。

### 14.12 Owner veto 必须与主质量 gate 分离

**走过的弯路**：把 owner team 直接设为主 ruleset 的 required reviewer。这样 owner 自己开的 PR
无法自审；更危险的是，为 owner 添加 ruleset bypass 会同时允许其绕过同一 ruleset 中的 required
CI、force-push 或其他保护。用 CODEOWNERS 代替也会把“安全 veto”误表达成文件 ownership。

**形成的规则**：指定 team reviewer 放入只含 pull-request review 的独立 ruleset；team 获得显式
Write 权限且只包含真正的 veto holders。Owner 以明确 `User` actor 获得 `pull_request` bypass，主
ruleset 继续独立强制 stable CI contexts。Checked-in recipe、live API 和 policy audit 必须同时验证
team ID、file pattern、minimum approvals、owner ID 与 bypass mode。

**验证方法**：用普通 collaborator PR 证明任意非 owner approval 都不能解锁；用 owner PR 证明其可
bypass reviewer gate，但失败或缺失的 `Required gates`/`CodeQL gate` 仍阻止 merge。再次回读所有
作用于 default branch 的有效 rules，确认不是只检查单个 ruleset 的 JSON。

### 14.13 Dependency snapshot 不是 release dependency manifest

**走过的弯路**：启用 Gradle Automatic Dependency Submission 后，Dependabot 把 Netty、BouncyCastle、
logging、HTTP client 等组件都显示为 `settings.gradle.kts` 的 direct dependency，看起来像产品发布包
直接依赖了它们。实际追踪 lockfile/configuration 和提交的 snapshot 后，这些组件来自 Android
lint、UTP/emulator、formatter、compiler/plugin 或 export classpath；最终 POM/module metadata 和
JAR/AAR/archive 中并不存在它们。

**形成的规则**：自动 dependency graph 证明“构建解析/执行过这些代码”，不自动证明“消费者会
安装这些代码”。Product dependency submission 必须限定真实 publication/runtime configurations；
build-tool dependencies 另行审计，不能为了让告警消失而完全隐藏。UI 中 synthetic manifest 或
`direct` relationship 不能替代 configuration、lockfile、published metadata 和 archive entry 四层
证据。

**验证方法**：下载 submission snapshot JSON，逐项追踪到 configuration；扫描最终 POM/module/
package metadata 与递归 archive entries；升级上游 toolchain 后重新提交 product graph。只有新的
snapshot、当前 lockfile/plugin graph 和发布产物都不再出现该组件时，才能判断为已清理的残留。

### 14.14 快速反模式索引

| 错误 | 后果 | 修复 |
| --- | --- | --- |
| 为了跨端一致而复制另一平台的构建/API 外形 | package、IDE 或 runtime 行为不符合生态 | 共享语义 spec，binding follow 平台最佳实践 |
| ruleset 直接要求 matrix job 名 | 平台增减即破坏 merge | 只要求稳定聚合 gate |
| owner team reviewer 与主 CI 放在同一 ruleset | owner bypass 会同时绕过 CI | reviewer 使用独立 ruleset，owner 仅 PR bypass |
| feature branch 同时触发 push 与 PR CI | 同一 commit 重复执行完整 matrix | push 只匹配 default branch，feature branch 由 PR/merge queue 验证 |
| push 和 PR 共用 SHA concurrency | push teardown 可饿死 PR | 按 event + PR/ref 分 lane |
| Android device profile 存在就假设 image 可用 | runner 找不到 ABI/image 或无 acceleration | 安装精确 image，固定 ABI，验证 KVM 并输出 stacktrace |
| Linux 强行 cross compile 含 cinterop 的 macOS klib | target 被禁用或产生误导 warning | 禁止伪跨编译，在 macOS host 真实构建 |
| CodeQL autobuild 扫整个跨平台 aggregate | 选错 task、命中 cache 或构建失败 | compiled language 使用 repo-owned manual build |
| clang-format 未配置结构性 braces | 单行控制语句持续漏 braces | 固定 formatter 并启用 InsertBraces/等价规则 |
| 把 SHA 当成历史 check-run 的授权凭据 | 形成缺失 context 或验证循环 | tag snapshot 重新运行 reusable CI |
| 聚合只检查 `failure` | skipped/cancelled 可能放行 | 要求每个结果精确为 `success` |
| release 查询 main/PR check-run | 发布依赖历史时序和可变状态 | tag snapshot 重新调用 reusable CI |
| dry run 使用正式 environment | PR 可能接触 secret 或等待 reviewer | dry run 只读且 disposable signing |
| publish job 重新 build | 发布字节未被 consumer 验证 | 下载 stage artifact 原样发布 |
| 只跑 workspace consumer | 无法发现 package/link/loader 问题 | clean temp project 消费 staged artifact |
| privileged `workflow_run` checkout PR | 不可信代码获得 write token | privileged side 只解析严格校验的数据 |
| 自动 Gradle dependency snapshot 当成发布 manifest | 工具依赖被误报为消费者依赖，或告警被错误忽略 | 过滤 product configurations，并单独审计 build toolchain |
| `v*.*.*` 被当成 SemVer regex | 非法 tag 可能触发 workflow | validate job 做 exact parser + `vVERSION` |
| tag 可移动或复用 | provenance 与 registry 不再可追溯 | ruleset 禁止 update/delete，新字节新版本 |
| 从 main dispatch 后只在 step checkout tag | tag-only environment 仍看到 main ref | rerun tag run，或以受保护 tag 作为 dispatch ref |
| active ruleset 先于远端演练 | 新 repo 可能被永久阻塞 | evaluate 验证后再 active |
| sole reviewer + prevent self-review | release 永远无法批准 | 增加独立 reviewer/team 或暂时关闭 |

## 15. Definition of Done

只有同时满足以下条件，repo setup 才完成：

- 本地 adapter、PR、main、merge queue 和 tag release 调用同一套可复现命令合同。
- 每个声明支持的 binding/target 都有 correctness、shared conformance 和真实 consumer 证据。
- Native/cinterop 与 emulator target 都在兼容 host/image/ABI 上取得真实执行证据，失败日志可诊断。
- `Required gates` 与 `CodeQL gate` 是 active ruleset 的唯一 required contexts。
- 若启用 owner veto，approval 使用独立 ruleset，owner bypass 不影响主 CI，team membership 已审计。
- 非阻塞观测无法影响 merge，fork PR 无法跨越 trust boundary。
- Benchmark execution 通过 `Benchmarks - Ready` 进入 required DAG；Size/Perf Diff 只比较精确
  head/base SHA，hosted-runner 性能差异保持 informational。
- Product dependency graph 与 build-tool inventory 分离；Dependabot 不把 CI 工具依赖冒充发布依赖，
  工具链 advisory 也没有被静默隐藏。
- Release dry run 不读取 credential，却完整复现 stage/sign/audit/consumer graph。
- 正式 release 在不可变 tag snapshot 上自证，并只发布已验证字节。
- Release environment、tag ruleset、OIDC/secret、signing 和 recovery 均经过实际演练。
- GitHub、所有 registry、checksums、signatures 和 attestations 可追溯到同一 tag commit。
- Bootstrap 可重复执行且不会默默删除未知策略；策略漂移由 CI audit 和 live API 验证发现。

## 16. 当前仓库到通用模板的映射

迁移时应复制“职责”，不应复制当前项目名或 job 数量：

| 通用职责 | 当前仓库实现 | 迁移规则 |
| --- | --- | --- |
| 根级 adapter | `package.json` 的 `pnpm` scripts 加 `scripts/*` | 收敛为目标 repo 的 `scripts/repo`，保留原生构建器 |
| Health checks | `ci.yml` 的 repository/C/ES/Kotlin/Swift health checks | 替换 formatter/linter，保留 frozen install、contract 与 policy audits |
| Package audit | `package-audit` | 对目标生态检查真实 pack/archive/publication 内容 |
| Core matrix | Linux/macOS/Windows C、shared/static、GCC/Clang | 替换为目标 core 的 host/compiler/linkage 支持矩阵 |
| Binding matrix | Swift、Kotlin/KMP/Android、ES/WASM | 只保留目标 repo 声明支持的 binding 和 deployment target |
| Runtime safety | ASan、UBSan、TSan、Android emulator、browser | 按目标语言风险选择，blocking 项必须进入聚合 gate |
| Stable PR gate | `Required gates` | 名称保持不变，内部 `needs` 随目标 repo 改变 |
| Push summary | `Development branch gates` | 名称保持与 required context 隔离 |
| Security gate | 四语言 CodeQL + `CodeQL gate` | matrix 改为目标产品语言，聚合名保持不变 |
| Owner veto | 独立 `owner approval gate` + `nouprax-core` | team/owner ID 必须由目标组织解析；不得与主 CI ruleset 合并 |
| PR observability | metrics producer + `workflow_run` commenter | 可删除；保留时必须维持不可信代码/写权限隔离 |
| Dependency governance | GitHub automatic Gradle snapshot + lockfile/toolchain tracing | 迁移为受控 product graph + 独立 build-tool audit，避免 synthetic manifest 混淆 |
| Dry run | C、Swift source、npm、Maven staged artifacts | 替换 artifact graph，继续禁止 secrets/environment |
| Formal release | tag CI → stage → sign/audit/consumer → publish | registry 可变，tag snapshot 自证与发布已验证字节不变 |
| GitHub policy | checked-in environment/ruleset JSON | actor/team ID、reviewer、repo、tag pattern 必须由 bootstrap 生成 |

当前仓库 release 的具体 registry 顺序是：聚合并签名 Maven bundle、运行 staged consumers、上传
Central 并等待验证、通过 npm OIDC 发布、发布已验证的 Central deployment，最后创建带 checksum
和 attestation 的 GitHub Release。目标仓库可以使用不同 registry，但必须为自己的不可逆操作定义
同样明确的顺序和 partial-failure recovery。
