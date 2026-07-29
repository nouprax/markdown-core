#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/discover-toolchain.sh"
repository="$root/build/kotlin-consumer-repository"
gradle="$root/scripts/gradle.sh"
property="-Dmaven.repo.local=$repository"
maven_repository_arg=
publish=true

if [ "${1:-}" = "--repository" ]; then
    repository=${2:?--repository requires a staged Maven repository path}
    case "$repository" in
        /*) ;;
        *) repository="$root/$repository" ;;
    esac
    consumer_local_repository="$root/build/kotlin-consumer-maven-local"
    rm -rf "$consumer_local_repository"
    mkdir -p "$consumer_local_repository"
    property="-Dmaven.repo.local=$consumer_local_repository"
    maven_repository_arg="-Dmarkdown.core.consumer.repository=file://$repository"
    publish=false
fi

cd "$root"
if [ "$publish" = true ]; then
    rm -rf "$repository"
    "$gradle" --warning-mode=fail "$property" :packages:kotlin-markdown-core:publishKotlinToMavenLocal
fi

"$gradle" --warning-mode=fail "$property" "-PconsumerRepository=$repository" \
    -p packages/kotlin-markdown-core/consumers/kmp jvmTest
"$gradle" --warning-mode=fail "$property" "-PconsumerRepository=$repository" \
    -p packages/kotlin-markdown-core/consumers/jvm-gradle run
"$gradle" --warning-mode=fail "$property" "-PconsumerRepository=$repository" \
    -p packages/kotlin-markdown-core/consumers/android assembleRelease assembleUnused

version=$(tr -d '[:space:]' <"$root/VERSION")
android_aar="$repository/com/nouprax/kotlin-markdown-core-android/$version/kotlin-markdown-core-android-$version.aar"
android_consumer_build="$root/packages/kotlin-markdown-core/consumers/android/build"
android_mapping="$android_consumer_build/outputs/mapping/release/mapping.txt"
android_configuration="$android_consumer_build/outputs/mapping/release/configuration.txt"
android_apk="$android_consumer_build/outputs/apk/release/kotlin-markdown-core-android-consumer-release-unsigned.apk"
android_unused_configuration="$android_consumer_build/outputs/mapping/unused/configuration.txt"
android_unused_apk="$android_consumer_build/outputs/apk/unused/kotlin-markdown-core-android-consumer-unused-unsigned.apk"
for artifact in \
    "$android_aar" \
    "$android_mapping" \
    "$android_configuration" \
    "$android_apk" \
    "$android_unused_configuration" \
    "$android_unused_apk"; do
    if [ ! -f "$artifact" ]; then
        echo "Android release-shrinking artifact is missing: $artifact" >&2
        exit 1
    fi
done

android_audit_dir=$(mktemp -d)
trap 'rm -rf "$android_audit_dir"' EXIT
published_consumer_rules="$android_audit_dir/published-consumer-rules.pro"
unzip -p "$android_aar" proguard.txt >"$published_consumer_rules"
if ! cmp -s \
    "$root/packages/kotlin-markdown-core/consumer-rules.pro" \
    "$published_consumer_rules"; then
    echo "Published Android AAR consumer rules differ from their reviewed source" >&2
    exit 1
fi
unzip -q "$android_apk" 'classes*.dex' -d "$android_audit_dir/used-dex"
unzip -q "$android_unused_apk" 'classes*.dex' -d "$android_audit_dir/unused-dex"
node "$root/scripts/verify-android-jni-shrinking.mjs" \
    "$published_consumer_rules" \
    "$android_mapping" \
    "$android_configuration" \
    "$android_unused_configuration" \
    "$root/packages/kotlin-markdown-core/src/native/markdown_core_kotlin.map" \
    "$android_audit_dir/used-dex" \
    "$android_audit_dir/unused-dex"

# The Maven consumer runs on the advertised JVM floor when the caller
# provides one (CI passes a JDK 17 home); Gradle consumers keep the
# toolchain JDK for AGP.
if [ -n "${MARKDOWN_CORE_MAVEN_CONSUMER_JAVA_HOME:-}" ]; then
    export JAVA_HOME="$MARKDOWN_CORE_MAVEN_CONSUMER_JAVA_HOME"
    "$JAVA_HOME/bin/java" -version 2>&1 | head -1
fi
MAVEN_USER_HOME="$root/build/maven-user-home" \
    MAVEN_OPTS="${MAVEN_OPTS:+$MAVEN_OPTS }--enable-native-access=ALL-UNNAMED" \
    "$root/mvnw" --batch-mode --no-transfer-progress \
    "$property" \
    ${maven_repository_arg:+"$maven_repository_arg"} \
    -f packages/kotlin-markdown-core/consumers/jvm-maven/pom.xml \
    verify
