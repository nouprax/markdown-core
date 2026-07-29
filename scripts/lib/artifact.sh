# Shared verify/extract preamble and manifest/SHA256SUMS epilogue helpers for
# the CI artifact adapter scripts (scripts/build-*-artifact.sh and
# scripts/run-*-artifact.sh).
#
# This file is sourced, never executed. The adapters run under
# `set -euo pipefail` bash; every helper preserves that contract. Stock macOS
# ships `shasum` but not `sha256sum`; both tools read and write the same
# `<hash>  <file>` line format, so the fallback keeps SHA256SUMS byte-identical
# across hosts.

# artifact_sha256_write <directory> <file>...
# Write SHA256SUMS for the given files inside <directory>.
artifact_sha256_write() {
    local directory=$1
    shift
    (
        cd "$directory"
        if command -v sha256sum >/dev/null 2>&1; then
            sha256sum "$@" >SHA256SUMS
        else
            shasum -a 256 "$@" >SHA256SUMS
        fi
    )
}

# artifact_sha256_check <directory>
# Verify SHA256SUMS inside <directory>.
artifact_sha256_check() {
    (
        cd "$1"
        if command -v sha256sum >/dev/null 2>&1; then
            sha256sum --check SHA256SUMS
        else
            shasum -a 256 --check SHA256SUMS
        fi
    )
}

# artifact_source_sha <root>
# The commit an artifact derives from: the CI-provided source SHA, or the
# checkout HEAD for local runs.
artifact_source_sha() {
    echo "${GITHUB_SHA:-$(git -C "$1" rev-parse HEAD)}"
}

# artifact_verify <artifact-dir> <kind>
# Consumer preamble: checksum integrity, the manifest kind, and (in CI) that
# the artifact was produced from the same commit this consumer checked out.
artifact_verify() {
    local artifact_dir=$1
    local kind=$2
    test -d "$artifact_dir"
    artifact_sha256_check "$artifact_dir"
    grep -Fxq "kind=$kind" "$artifact_dir/manifest.txt"
    if [ -n "${GITHUB_SHA:-}" ]; then
        grep -Fxq "source_sha=$GITHUB_SHA" "$artifact_dir/manifest.txt"
    fi
}

# artifact_extract <artifact-dir> <tarball> <destination>
# Unpack a verified artifact tree into <destination>.
artifact_extract() {
    tar -xzf "$1/$2" -C "$3"
}
