#!/bin/sh
set -eu

# rustfmt from the toolchain packages/markdown-core-rs/rust-toolchain.toml
# pins, with the repository's rustfmt.toml. The fuzz crate is a workspace of
# its own, so it is formatted by its own manifest.
case "${1:-}" in
    --check)
        cargo_fmt_args="--all --check"
        ;;
    "")
        cargo_fmt_args="--all"
        ;;
    *)
        echo "Usage: $0 [--check]" >&2
        exit 2
        ;;
esac

cd packages/markdown-core-rs
# shellcheck disable=SC2086
cargo fmt $cargo_fmt_args
# shellcheck disable=SC2086
cargo fmt --manifest-path fuzz/Cargo.toml $cargo_fmt_args
