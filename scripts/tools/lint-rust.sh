#!/bin/sh
set -eu

# Clippy (the crate denies clippy::pedantic in src/lib.rs) and rustdoc, with
# warnings as errors, from the toolchain packages/markdown-core-rs/
# rust-toolchain.toml pins.
cd packages/markdown-core-rs
cargo clippy --all-targets --locked -- -D warnings
RUSTDOCFLAGS="-D warnings" cargo doc --no-deps --locked
