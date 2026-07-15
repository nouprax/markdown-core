#!/usr/bin/env bash
set -euo pipefail

directory=${1:?usage: create-release-checksums.sh ARTIFACT_DIRECTORY}
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT

find "$directory" -maxdepth 1 -type f \
    ! -name SHA256SUMS ! -name SHA512SUMS -print | LC_ALL=C sort | \
    while IFS= read -r artifact; do
        name=$(basename "$artifact")
        printf '%s  %s\n' "$(openssl dgst -sha256 -r "$artifact" | awk '{ print $1 }')" "$name"
    done >"$temporary/SHA256SUMS"
find "$directory" -maxdepth 1 -type f \
    ! -name SHA256SUMS ! -name SHA512SUMS -print | LC_ALL=C sort | \
    while IFS= read -r artifact; do
        name=$(basename "$artifact")
        printf '%s  %s\n' "$(openssl dgst -sha512 -r "$artifact" | awk '{ print $1 }')" "$name"
    done >"$temporary/SHA512SUMS"

test -s "$temporary/SHA256SUMS"
test -s "$temporary/SHA512SUMS"
mv "$temporary/SHA256SUMS" "$directory/SHA256SUMS"
mv "$temporary/SHA512SUMS" "$directory/SHA512SUMS"

verify_manifest() {
    algorithm=$1
    manifest=$2
    while IFS= read -r entry; do
        expected=${entry%% *}
        name=${entry#*  }
        [ -n "$name" ] || { echo "invalid checksum entry in $manifest" >&2; exit 1; }
        [ "$name" != "$entry" ] || { echo "invalid checksum entry in $manifest" >&2; exit 1; }
        actual=$(openssl dgst "-$algorithm" -r "$directory/$name" | awk '{ print $1 }')
        [ "$actual" = "$expected" ] || {
            echo "$algorithm checksum mismatch: $name" >&2
            exit 1
        }
    done <"$directory/$manifest"
}

verify_manifest sha256 SHA256SUMS
verify_manifest sha512 SHA512SUMS
echo "Created release checksum manifests in $directory"
