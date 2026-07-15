#!/usr/bin/env bash
set -euo pipefail

repository=${1:?usage: sign-maven-publications.sh REPOSITORY [--ephemeral]}
mode=${2:-release}

if ! command -v gpg >/dev/null 2>&1; then
    echo "gpg is required to sign Maven publications" >&2
    exit 1
fi

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
export GNUPGHOME="$temporary/gnupg"
mkdir -m 700 "$GNUPGHOME"
passphrase_file="$temporary/passphrase"
key_file="$temporary/signing-key.asc"
umask 077

case "$mode" in
    --ephemeral)
        if [ -n "${MAVEN_SIGNING_KEY:-}" ] || [ -n "${MAVEN_SIGNING_PASSWORD:-}" ]; then
            echo "ephemeral dry-run signing refuses release signing secrets" >&2
            exit 1
        fi
        : >"$passphrase_file"
        gpg --batch --pinentry-mode loopback --passphrase '' \
            --quick-generate-key \
            'Markdown Core release dry run <release-dry-run@nouprax.invalid>' \
            rsa3072 sign 1d >/dev/null
        ;;
    release)
        [ -n "${MAVEN_SIGNING_KEY:-}" ] || { echo "MAVEN_SIGNING_KEY is required" >&2; exit 1; }
        [ -n "${MAVEN_SIGNING_PASSWORD:-}" ] || { echo "MAVEN_SIGNING_PASSWORD is required" >&2; exit 1; }
        printf '%s' "$MAVEN_SIGNING_KEY" >"$key_file"
        printf '%s' "$MAVEN_SIGNING_PASSWORD" >"$passphrase_file"
        gpg --batch --import "$key_file" >/dev/null
        ;;
    *)
        echo "unknown signing mode: $mode" >&2
        exit 2
        ;;
esac

# Gradle file repositories may contain checksums produced before host artifacts
# are aggregated. Recreate every sidecar only after the final bytes are fixed.
find "$repository" -type f \
    \( -name '*.asc' -o -name '*.md5' -o -name '*.sha1' -o -name '*.sha256' -o -name '*.sha512' \) \
    -delete

find "$repository" -type f \
    ! -name '*.asc' ! -name '*.md5' ! -name '*.sha1' ! -name '*.sha256' ! -name '*.sha512' \
    -print | LC_ALL=C sort | while IFS= read -r artifact; do
        gpg --batch --yes --pinentry-mode loopback \
            --passphrase-file "$passphrase_file" \
            --armor --detach-sign --output "$artifact.asc" "$artifact"
    done

find "$repository" -type f \
    ! -name '*.md5' ! -name '*.sha1' ! -name '*.sha256' ! -name '*.sha512' \
    -print | LC_ALL=C sort | while IFS= read -r artifact; do
        openssl dgst -md5 -r "$artifact" | awk '{ print $1 }' >"$artifact.md5"
        openssl dgst -sha1 -r "$artifact" | awk '{ print $1 }' >"$artifact.sha1"
        openssl dgst -sha256 -r "$artifact" | awk '{ print $1 }' >"$artifact.sha256"
        openssl dgst -sha512 -r "$artifact" | awk '{ print $1 }' >"$artifact.sha512"
    done

find "$repository" -type f -name '*.asc' -print | while IFS= read -r signature; do
    gpg --batch --verify "$signature" "${signature%.asc}" >/dev/null
done

echo "Signed Maven publications and generated SHA-256/SHA-512 checksums in $repository"
