#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output="$root/build/release-dry-run"

# The dry run has no code path that needs registry credentials. Remove any
# inherited release environment before invoking build tools.
unset MAVEN_CENTRAL_USERNAME MAVEN_CENTRAL_PASSWORD
unset MAVEN_SIGNING_KEY MAVEN_SIGNING_PASSWORD

rm -rf "$output"
mkdir -p "$output/artifacts"
cd "$root"

node scripts/check-release-version.mjs
scripts/stage-c-release.sh "$output/c"
scripts/check-swift-source-archive.sh "$output/swift"
scripts/stage-npm-release.sh "$output/npm"
scripts/stage-maven-publications.sh "$output/maven" host
scripts/sign-maven-publications.sh "$output/maven/repository" --ephemeral
node scripts/audit-maven-publications.mjs "$output/maven/repository" --signed
scripts/check-kotlin-consumers.sh --repository "$output/maven/repository"

find "$output/c" "$output/swift" "$output/npm" -maxdepth 1 -type f \
    -exec cp {} "$output/artifacts" \;
scripts/create-release-checksums.sh "$output/artifacts"

echo "Host release dry run passed. Full Linux/macOS aggregation remains in the Release dry run workflow."
