#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
repository="$root/build/kotlin-consumer-repository"
gradle="$root/scripts/gradle.sh"
property="-Dmaven.repo.local=$repository"
publish=true

if [ "${1:-}" = "--repository" ]; then
    repository=${2:?--repository requires a staged Maven repository path}
    case "$repository" in
        /*) ;;
        *) repository="$root/$repository" ;;
    esac
    property="-Dmaven.repo.local=$repository"
    publish=false
fi

if [ -z "${JAVA_HOME:-}" ] && [ -x "/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home/bin/java" ]; then
    export JAVA_HOME="/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home"
elif [ -z "${JAVA_HOME:-}" ] && [ -x "/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin/java" ]; then
    export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
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
    -p packages/kotlin-markdown-core/consumers/android assembleDebug
MAVEN_USER_HOME="$root/build/maven-user-home" \
    MAVEN_OPTS="${MAVEN_OPTS:+$MAVEN_OPTS }--enable-native-access=ALL-UNNAMED" \
    "$root/mvnw" --batch-mode --no-transfer-progress \
    -Dmaven.repo.local="$repository" \
    -f packages/kotlin-markdown-core/consumers/jvm-maven/pom.xml \
    verify
