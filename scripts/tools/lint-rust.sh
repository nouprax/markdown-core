#!/bin/sh
set -eu

# Clippy (the crate denies clippy::pedantic in src/lib.rs) and rustdoc, with
# warnings as errors, from the toolchain packages/markdown-core-rs/
# rust-toolchain.toml pins; clippy again with the coverage feature, which
# scripts/correctness/cover.mjs builds.
cd packages/markdown-core-rs
cargo clippy --all-targets --locked -- -D warnings
cargo clippy --all-targets --locked --features cover -- -D warnings
RUSTDOCFLAGS="-D warnings" cargo doc --no-deps --locked
