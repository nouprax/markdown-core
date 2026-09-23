#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
output="$root/build/release-dry-run"

# The dry run has no code path that needs registry credentials. Remove any
# inherited release environment before invoking build tools.
unset MAVEN_CENTRAL_USERNAME MAVEN_CENTRAL_PASSWORD
unset MAVEN_SIGNING_KEY MAVEN_SIGNING_PASSWORD

rm -rf "$output"
mkdir -p "$output/artifacts"
cd "$root"

node scripts/release/check-version.mjs
scripts/release/stage-c.sh "$output/c"
scripts/release/check-swift-source-archive.sh "$output/swift"
scripts/release/stage-npm.sh "$output/npm"
scripts/release/stage-maven-publications.sh "$output/maven" host
scripts/release/sign-maven-publications.sh "$output/maven/repository" --ephemeral
scripts/conformance/check-kotlin-consumers.sh --repository "$output/maven/repository"
node scripts/audit/check-maven-publications.mjs "$output/maven/repository" --signed

find "$output/c" "$output/swift" "$output/npm" -maxdepth 1 -type f \
    -exec cp {} "$output/artifacts" \;
scripts/release/create-checksums.sh "$output/artifacts"

echo "Host release dry run passed. Full Linux/macOS aggregation remains in the Release dry run workflow."
