#!/usr/bin/env bash
# What ORDINARY JAVA can see of the Kotlin binding.
#
# `internal` has no JVM equivalent. A declaration reached from another
# compilation unit lowers to `public final`, so implementation classes become
# callable from Java without one line of Kotlin saying so -- and
# `checkKotlinAbi` CANNOT SEE THIS: its dump is the Kotlin API, and every one of
# these is absent from it by construction.
#
# So this compares two sets that no other gate compares:
#
#   the JVM-public classes in the compiled jar
#   MINUS the classes the Kotlin ABI dump declares
#   == what Java can reach and nobody meant to publish
#
# The remainder is pinned below. It may SHRINK freely; anything new fails, which
# is the point -- a new leak is otherwise invisible in review.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
. "$root/scripts/lib/discover-toolchain.sh"

javap="${JAVA_HOME:?JAVA_HOME is required}/bin/javap"
[ -x "$javap" ] || { echo "audit-kotlin-jvm-surface: no javap at $javap" >&2; exit 1; }

classes="packages/kotlin-markdown-core/build/classes/kotlin/jvm/main"
dump="packages/kotlin-markdown-core/api/jvm/kotlin-markdown-core.api"
pinned="specs/kotlin/jvm-visible-surface.txt"

"$root/scripts/gradle.sh" --quiet :packages:kotlin-markdown-core:jvmMainClasses >/dev/null
[ -d "$classes" ] || { echo "audit-kotlin-jvm-surface: no compiled JVM classes at $classes" >&2; exit 1; }
[ -f "$dump" ] || { echo "audit-kotlin-jvm-surface: no ABI dump at $dump; run updateKotlinAbi" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

find "$classes" -name '*.class' | sed "s#^$classes/##; s#\.class\$##; s#/#.#g" | sort >"$work/all.txt"
while IFS= read -r fqcn; do
    if "$javap" -cp "$classes" "$fqcn" 2>/dev/null | sed -n '2p' | grep -q '^public'; then
        printf '%s\n' "${fqcn##*.}"
    fi
done <"$work/all.txt" | sort -u >"$work/jvm-public.txt"

grep -oE '^public [a-z ]*(class|interface) [^ ]+' "$dump" | sed 's#.*/##' | sort -u >"$work/kotlin-api.txt"
comm -23 "$work/jvm-public.txt" "$work/kotlin-api.txt" >"$work/leaked.txt"

grep -vE '^\s*(#|$)' "$pinned" | sort -u >"$work/pinned.txt"

if ! added=$(comm -23 "$work/leaked.txt" "$work/pinned.txt") || [ -n "$added" ]; then
    echo "Kotlin JVM surface audit failed: these are visible to Java and not pinned:" >&2
    printf '%s\n' "$added" >&2
    echo "Each one is callable from Java. Make it unreachable, or pin it in $pinned with a reason." >&2
    exit 1
fi

# THE ONES THAT WERE CLOSED RATHER THAN PINNED, and the claim is mechanised:
# every JNI entry point must stay hidden from javac. JNI resolves by name and
# descriptor and ignores ACC_SYNTHETIC, so this costs the binding nothing. A
# missing method fails too - if the session surface changes, this list is
# part of the change.
"$javap" -v -cp "$classes" com.nouprax.markdown.core.JvmNative >"$work/jvmnative.txt" 2>/dev/null
for entry in sessionNew sessionFeed sessionAdvance sessionFinish sessionFree; do
    if ! grep -A3 -E "native .*\b$entry\(" "$work/jvmnative.txt" | grep -q ACC_SYNTHETIC; then
        echo "Kotlin JVM surface audit failed: JvmNative.$entry is resolvable from Java." >&2
        echo "It needs @JvmSynthetic: internal is public on the JVM." >&2
        exit 1
    fi
done

printf 'Kotlin JVM surface audit passed: %s Java-visible classes, %s in the Kotlin API, %s pinned as unreachable-by-intent.\n' \
    "$(wc -l <"$work/jvm-public.txt" | tr -d ' ')" \
    "$(wc -l <"$work/kotlin-api.txt" | tr -d ' ')" \
    "$(wc -l <"$work/leaked.txt" | tr -d ' ')"
