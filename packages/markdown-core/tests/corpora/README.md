# Vendored test corpora

This directory is the only place external test or benchmark corpora may
live.  It is currently empty by design: every correctness suite and
benchmark workload runs on tracked fixtures, tracked sample documents, or
data generated deterministically in-process.

## Frozen policy

- Correctness, benchmark, CI, IDE, and normal build/test commands are fully
  offline.  Nothing may clone, download, or update a corpus at runtime.
- A corpus enters the repository only through a one-shot, explicitly invoked
  maintenance import.  If the import needs a clone, it happens at a pinned
  revision in a temporary directory outside the repository; only the files
  declared in the manifest are copied, and the checkout, archives, caches,
  and intermediate outputs are deleted.  Source checkouts must never appear
  in the repository root.
- Every vendored corpus lives at `packages/markdown-core/tests/corpora/<name>/`
  and must contain:
  - `MANIFEST.json` — canonical source URL, immutable commit/tag, imported
    paths, original and normalized SHA-256 per file, byte sizes, the import
    command and tool versions, the update procedure, and copyright and
    attribution notes.
  - `LICENSE` — the complete license text.  Only licenses that clearly allow
    commercial use, modification, repository redistribution, and automated
    testing, and that are compatible with the project licensing policy, may
    be imported.  Prefer project-authored, CC0/public-domain, MIT, BSD, or
    Apache-2.0 content.
  - `SHA256SUMS` — checksums for every imported file, verified by
    `scripts/audit-test-topology.sh`.
- Corpora are excluded from release packages.
- The `packaging_corpus_guard` / `benchmark_corpus_guard` CTest tests fail
  any run that finds an unmanaged checkout, loose generated input, or a
  corpus directory missing the files above.
