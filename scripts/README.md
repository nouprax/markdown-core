# Repository scripts

Scripts are grouped by responsibility. Public `pnpm` commands remain the stable
entry points; CI, CMake and Gradle invoke the same implementations directly.

| Directory | Responsibility |
| --- | --- |
| `audit/` | Repository, source, ownership, packaging and pipeline policy checks. |
| `benchmark/` | Corpus proofs, generation, measurement, profiling and numeric reports. |
| `build/` | Produce immutable product/test artifacts and check deployment builds. |
| `conformance/` | Canonical contracts, consumer compatibility and build-model contracts. |
| `correctness/` | External parser parity, composition checks and differential fuzzing. |
| `release/` | Validate, stage, sign and publish release artifacts. |
| `shared/` | Modules and artifact runners used by multiple responsibilities. |
| `tooling/` | Toolchain setup, generators, formatting, linting and local gate orchestration. |

Executable names use an action followed by its object in lowercase kebab-case:
`audit/check-ci-policy.sh`, `build/create-c-product.sh`,
`correctness/check-upstream-parity.mjs`. A directory's sole main operation can
use a short action name, such as `benchmark/run.mjs` or `release/dry-run.sh`.
Reusable modules use object names: `benchmark/corpus.mjs`,
`benchmark/coverage.mjs`, `shared/element-inventory.mjs`. Directory responsibilities
are not repeated as `audit-`, `benchmark-` or `grammar-` filename prefixes.

Tests live in the owning directory's `tests/` folder and use the corresponding
module or entry-point basename plus `.test.mjs`. Shared helpers move to `shared/`
only when callers cross responsibility boundaries. Java support classes retain
Java's required class/file naming convention.

The artifact test runners in `shared/` accept correctness or conformance suite
selection; both pipelines use that one execution mechanism. Native correctness
does not become a benchmark admission gate through this organization.

The parse benchmark starts with `pnpm benchmark` (or
`node scripts/benchmark/run.mjs`). It uses one corpus and measures each named
input once per applicable engine/revision. Historical script paths have no
forwarding wrappers; update callers when relocating an implementation.
