#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

fail() {
    echo "Public surface audit failed: $1" >&2
    exit 1
}

public_headers=$(find packages/markdown-core/include -maxdepth 1 -type f -print | sort)
if [ "$public_headers" != "packages/markdown-core/include/markdown_core.h" ]; then
    printf '%s\n' "$public_headers" >&2
    fail "the C package must install exactly one facade header"
fi

test -f packages/markdown-core/core/extension.h \
    || fail "the internal parser-extension descriptor header is missing"
test ! -e packages/markdown-core/core/syntax_extension.h \
    || fail "the retired syntax_extension.h header still exists"
if grep -R -n -E \
    'markdown_core_syntax_extension|markdown_core_[a-z0-9_]*_syntax_extensions?|syntax_extension\.h' \
    packages/markdown-core scripts --exclude-dir=build --exclude=audit-public-surface.sh; then
    fail "the retired syntax-extension identifier family still exists"
fi
grep -q 'typedef struct markdown_core_extension markdown_core_extension;' \
    packages/markdown-core/core/markdown-core.h \
    || fail "the parser-extension descriptor does not use markdown_core_extension"
grep -q 'markdown_core_parser_attach_extension' \
    packages/markdown-core/core/markdown-core-extension-api.h \
    || fail "the parser-extension attachment API was not renamed coherently"
if grep -R -n 'markdown_core_map_entry' packages/markdown-core --exclude-dir=build; then
    fail "the retired map-entry type still exists"
fi
grep -q 'typedef struct markdown_core_map_record markdown_core_map_record;' \
    packages/markdown-core/core/map.h \
    || fail "the normalized-label map record does not use markdown_core_map_record"

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

node - packages/markdown-core/include/markdown_core.h \
    packages/markdown-core/core/exports/markdown_core.map \
    packages/markdown-core/core/exports/markdown_core.exports "$temp_dir" <<'NODE'
import fs from "node:fs";
import path from "node:path";

const [, , headerPath, mapPath, machOPath, outputDirectory] = process.argv;
const header = fs.readFileSync(headerPath, "utf8");
const map = fs.readFileSync(mapPath, "utf8");
// THE MACH-O LIST IS THE ONE THAT LINKS ON macOS, and until Step 13 nothing
// compared it with anything: a symbol added to the header and the ELF version
// script and forgotten here left this audit green and the macOS shared library
// short one symbol, which surfaces as a link error in a binding build.
const machO = fs.readFileSync(machOPath, "utf8");
const declared = [
    ...header.matchAll(/MARKDOWN_CORE_API[\s\S]*?\b(markdown_core_[a-z0-9_]+)\s*\(/g)
].map((match) => match[1]).sort();
const exported = [...map.matchAll(/^\s+(markdown_core_[a-z0-9_]+);$/gm)]
    .map((match) => match[1])
    .sort();
const machOExported = [...machO.matchAll(/^_(markdown_core_[a-z0-9_]+)$/gm)]
    .map((match) => match[1])
    .sort();
fs.writeFileSync(path.join(outputDirectory, "declared.txt"), `${declared.join("\n")}\n`);
fs.writeFileSync(path.join(outputDirectory, "exported.txt"), `${exported.join("\n")}\n`);
if (declared.join("\n") !== exported.join("\n")) {
    throw new Error("C header declarations and export allowlist differ");
}
if (declared.join("\n") !== machOExported.join("\n")) {
    throw new Error("C header declarations and the Mach-O export list differ");
}
for (const symbol of declared) {
    if (
        /_(?:set|insert|append|prepend|replace|unlink|new|render)(?:_|$)/.test(symbol) ||
        /_(?:feed|stream|edit|session|snapshot|delta|diagnostic)(?:_|$)/.test(symbol) ||
        /_parser_(?:new|new_with_mem|feed|finish|free)$/.test(symbol) ||
        /_parse_file$/.test(symbol)
    ) {
        throw new Error(`Retired or mutable C symbol is public: ${symbol}`);
    }
}
NODE

# THE DIALECT HAS NO SWITCHES. No surface names a parse option and the
# installed CLI takes no flag that would select a language: not the facade
# header, not the export lists, not a binding.
if grep -n -E 'parse_options|smart_punctuation|strip_html_comments' \
    packages/markdown-core/include/markdown_core.h \
    packages/markdown-core/core/exports/markdown_core.map \
    packages/markdown-core/core/exports/markdown_core.exports; then
    fail "the C facade still publishes a parse option"
fi
if grep -n -E '"--profile"|"--smart"|"--extension"|"-e"' packages/markdown-core/core/main.c; then
    fail "the installed CLI still exposes a language switch"
fi
if grep -R -n -E 'ParseOptions|parseOptions|smartPunctuation|stripHTMLComments' \
    packages/swift-markdown-core/Sources packages/kotlin-markdown-core/src/commonMain \
    packages/kotlin-markdown-core/src/jvmMain packages/kotlin-markdown-core/src/androidMain \
    packages/kotlin-markdown-core/src/nativePlatformMain packages/es-markdown-core/src; then
    fail "a binding still publishes a parse option"
fi
if grep -n 'parseOptions' specs/canonical-ast/manifest.json; then
    fail "the canonical manifest still names a parse option"
fi
# ONE LANGUAGE. The library builds every parser the same way: no entry point
# takes a feature set, no registry names one, and no test parses a part of the
# dialect that nothing ships.
if grep -R -n -E 'markdown_core_document_parse_features|feature-registry|markdown_core_feature_' \
    packages/markdown-core --include='*.c' --include='*.h' --include='CMakeLists.txt'; then
    fail "a second parse entry or a feature registry has returned; the dialect is one language"
fi

# These are API identifier checks, not prose checks.
retired_surface_terms='render|feed|stream|edit|session|snapshot|delta|diagnostic|concrete|Concrete|CST|ConcreteSyntax|Token|Trivia|Recovery|Walker|WalkEvent'
# Mutation verbs end at an identifier or camel/snake-case word boundary;
# a noun sharing a lowercase prefix does not name a mutation operation.
mutation_surface_terms='set[A-Z]|(insert|append|prepend|replace|unlink)([A-Z_]|\b)'

CLANG_MODULE_CACHE_PATH="$temp_dir/swift-module-cache" \
    swift package --disable-sandbox dump-package >"$temp_dir/swift-package.json"
node - "$temp_dir/swift-package.json" <<'NODE'
import fs from "node:fs";

const manifest = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const products = manifest.products.map((product) => `${product.name}:${product.targets.join(",")}`);
if (products.join("\n") !== "MarkdownCore:MarkdownCore") {
    throw new Error(`Unexpected SwiftPM products: ${products.join(", ")}`);
}
NODE

if grep -R -n -E \
    "public (class|struct|enum|protocol|typealias|func|var|let|static func).*\\b(${retired_surface_terms}|${mutation_surface_terms}|nativeHandle|pointer|memory|wasm)" \
    packages/swift-markdown-core/Sources/MarkdownCore; then
    fail "Swift exports a retired API, mutation, or native implementation detail"
fi
grep -q 'public enum TreeDumper' packages/swift-markdown-core/Sources/MarkdownCore/Visitor/TreeDumper.swift \
    && grep -q 'public static func dump' packages/swift-markdown-core/Sources/MarkdownCore/Visitor/TreeDumper.swift \
    && grep -q 'func dump() -> String' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Markup.swift \
    || fail "Swift does not expose the reviewed Markup debug dump API"
grep -q 'public struct TableRow: Markup' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Table.swift \
    && grep -q 'public struct TableCell: Markup' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Table.swift \
    && grep -q 'visit(_ node: TableRow)' packages/swift-markdown-core/Sources/MarkdownCore/Visitor/MarkupVisitor.swift \
    && grep -q 'visit(_ node: TableCell)' packages/swift-markdown-core/Sources/MarkdownCore/Visitor/MarkupVisitor.swift \
    || fail "Swift table rows and cells are not first-class Markup visitor nodes"
# The kind count is the CONTRACT's, not a number written here. It was 28 in
# three places until Step 7 added a 29th kind and all three said the same wrong
# thing at once.
kind_count=$(node -e 'process.stdout.write(String(JSON.parse(require("node:fs").readFileSync("docs/specs/canonical-ast.json", "utf8")).kinds.length))')

if grep -R -n 'defaultVisit' packages/swift-markdown-core/Sources/MarkdownCore; then
    fail "Swift MarkupVisitor exposes a catch-all fallback"
fi
test "$(grep -c 'mutating func visit' packages/swift-markdown-core/Sources/MarkdownCore/Visitor/MarkupVisitor.swift)" -eq "$kind_count" \
    || fail "Swift MarkupVisitor is not exhaustive over all $kind_count Markup kinds"
grep -q 'public enum WalkPhase' \
    packages/swift-markdown-core/Sources/MarkdownCore/Visitor/MarkupWalkingVisitor.swift \
    && grep -q 'public protocol MarkupWalkingVisitor' \
        packages/swift-markdown-core/Sources/MarkdownCore/Visitor/MarkupWalkingVisitor.swift \
    && grep -q 'public func walk<Visitor: MarkupWalkingVisitor>' \
        packages/swift-markdown-core/Sources/MarkdownCore/Visitor/MarkupWalkingVisitor.swift \
    || fail "Swift does not expose the typed walking visitor contract"
# A walking visitor names every Markup kind through a `node` entry and every
# scoped value of the contract (M4) through exactly one `value` entry; the two
# are counted apart so a value entry can neither stand in for a kind nor go
# missing.
scoped_values=$(node -e 'const contract = JSON.parse(require("node:fs").readFileSync("docs/specs/canonical-ast.json", "utf8")); process.stdout.write(Object.entries(contract.values).filter(([, value]) => value.scoped && value.walk !== false).map(([name]) => name).join(" "))')
test "$(awk '/public protocol MarkupWalkingVisitor/{inside=1; next} inside && /^}/{exit} inside && /mutating func visit\(_ node:/{count++} END{print count+0}' packages/swift-markdown-core/Sources/MarkdownCore/Visitor/MarkupWalkingVisitor.swift)" -eq "$kind_count" \
    || fail "Swift MarkupWalkingVisitor is not exhaustive over all $kind_count Markup kinds"
for value in $scoped_values; do
    test "$(grep -c "mutating func visit(_ value: $value, phase: WalkPhase)" packages/swift-markdown-core/Sources/MarkdownCore/Visitor/MarkupWalkingVisitor.swift)" -eq 1 \
        || fail "Swift MarkupWalkingVisitor does not name the scoped value $value exactly once"
done

grep -q 'explicitApi()' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin explicit API mode is disabled"
if grep -R -n -E \
    "public (class|data class|sealed class|enum class|object|interface|typealias|fun|val|var).*\\b(${retired_surface_terms}|${mutation_surface_terms}|nativeHandle|pointer|memory|wasm)" \
    packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin exports a retired API, mutation, or native implementation detail"
fi
grep -q 'public object TreeDumper' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/TreeDumper.kt \
    && grep -q 'public fun dump(root: Markup): String' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/TreeDumper.kt \
    && grep -q 'public fun dump(): String' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Markup.kt \
    || fail "Kotlin does not expose the reviewed Markup debug dump API"
grep -q 'visitor.visitTableRow(this)' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Table.kt \
    && grep -q 'visitor.visitTableCell(this)' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Table.kt \
    && grep -q 'visitTableRow' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/Visitor.kt \
    && grep -q 'visitTableCell' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/Visitor.kt \
    || fail "Kotlin table rows and cells are not first-class Markup visitor nodes"
if grep -R -n 'defaultVisit' packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin Visitor exposes a catch-all fallback"
fi
test "$(grep -c 'public fun visit' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/Visitor.kt)" -eq "$kind_count" \
    || fail "Kotlin Visitor is not exhaustive over all $kind_count Markup kinds"
grep -q 'public enum class WalkPhase' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/WalkingVisitor.kt \
    && grep -q 'public interface WalkingVisitor' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/WalkingVisitor.kt \
    && grep -q 'public fun Markup.walk(visitor: WalkingVisitor)' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/WalkingVisitor.kt \
    || fail "Kotlin does not expose the typed walking visitor contract"
test "$(awk '/public interface WalkingVisitor/{inside=1; next} inside && /^}/{exit} inside && /^        node: /{count++} END{print count+0}' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/WalkingVisitor.kt)" -eq "$kind_count" \
    || fail "Kotlin WalkingVisitor is not exhaustive over all $kind_count Markup kinds"
for value in $scoped_values; do
    test "$(awk -v value="$value" '/public interface WalkingVisitor/{inside=1; next} inside && /^}/{exit} inside && $0 == "    public fun visit" value "(" {found++} END{print found+0}' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/WalkingVisitor.kt)" -eq 1 \
        || fail "Kotlin WalkingVisitor does not name the scoped value $value exactly once"
done
grep -q '^headers = markdown_core.h$' \
    packages/kotlin-markdown-core/src/nativeInterop/cinterop/markdown_core_kotlin.def \
    && grep -q '^package = com.nouprax.markdown.core.internal.capi$' \
        packages/kotlin-markdown-core/src/nativeInterop/cinterop/markdown_core_kotlin.def \
    && grep -q '^staticLibraries = libmarkdown-core-extensions.a libmarkdown-core.a$' \
        packages/kotlin-markdown-core/src/nativeInterop/cinterop/markdown_core_kotlin.def \
    && grep -q 'markdown_core_document_parse' \
        packages/kotlin-markdown-core/src/nativePlatformMain/kotlin/com/nouprax/markdown/core/PlatformParser.native.kt \
    && ! grep -R -q 'markdown_core_kotlin_jni_' \
        packages/kotlin-markdown-core/src/nativePlatformMain \
        packages/kotlin-markdown-core/src/nativeInterop \
    || fail "Kotlin/Native must cinterop the C facade directly, independently of JNI"
if find packages/kotlin-markdown-core/src -type f -name 'NativeBridge*' | grep -q . \
    || grep -R -q -E '\bnativeParse\b|internal\.nativebridge' packages/kotlin-markdown-core/src; then
    fail "the retired cross-target NativeBridge abstraction still exists"
fi
if find packages/kotlin-markdown-core/src/commonMain packages/kotlin-markdown-core/src/nativePlatformMain \
    -type f \( -name 'JniPayloadDecoder.kt' -o -name 'JniMarkupDecoder.kt' -o -name 'JniNodeKind.kt' \) | grep -q .; then
    fail "the JVM/Android JNI wire protocol leaked into a Kotlin/Native source set"
fi
grep -q 'JVM/Android-only JNI payload encoder' \
    packages/kotlin-markdown-core/src/native/markdown_core_kotlin_jni_payload.h \
    && ! grep -q 'markdown_core_kotlin_jni_payload' \
        packages/kotlin-markdown-core/src/nativeInterop/cinterop/markdown_core_kotlin.def \
    || fail "the JNI payload encoder leaked into the Kotlin/Native adapter"
grep -qx '_JNI_OnLoad' packages/kotlin-markdown-core/src/native/markdown_core_kotlin.exports \
    && grep -qx '    JNI_OnLoad' packages/kotlin-markdown-core/src/native/markdown_core_kotlin.def \
    && test "$(grep -cE '^        [A-Za-z0-9_]+;' packages/kotlin-markdown-core/src/native/markdown_core_kotlin.map)" -eq 1 \
    && grep -q '^        JNI_OnLoad;$' packages/kotlin-markdown-core/src/native/markdown_core_kotlin.map \
    || fail "Kotlin JNI export allowlists must contain only JNI_OnLoad"

if grep -R -E -n 'readonly children' packages/es-markdown-core/src/model; then
    fail "ES exposes generic children"
fi
if grep -R -n -E \
    "^export (declare )?(class|interface|type|enum|function|const).*\\b(${retired_surface_terms}|${mutation_surface_terms}|nativeHandle|pointer|memory|wasm)" \
    packages/es-markdown-core/src; then
    fail "ES exports a retired API, mutation, or native implementation detail"
fi
grep -q 'TableRow extends MarkupBase<"tableRow">' packages/es-markdown-core/src/model/table.ts \
    && grep -q 'TableCell extends MarkupBase<"tableCell">' packages/es-markdown-core/src/model/table.ts \
    && grep -q 'visitTableRow(this:' packages/es-markdown-core/src/visitor.ts \
    && grep -q 'visitTableCell(this:' packages/es-markdown-core/src/visitor.ts \
    || fail "ES table rows and cells are not first-class Markup visitor nodes"
if grep -R -E -n 'defaultVisit|visit[A-Z][A-Za-z]+\?' packages/es-markdown-core/src; then
    fail "ES Visitor exposes a catch-all or optional typed handlers"
fi
test "$(grep -c '^    visit[A-Z].*(this:' packages/es-markdown-core/src/visitor.ts)" -eq "$kind_count" \
    || fail "ES Visitor is not exhaustive over all $kind_count Markup kinds"
grep -q 'export type WalkPhase = "entering" | "exiting"' \
    packages/es-markdown-core/src/walking-visitor.ts \
    && grep -q 'export interface WalkingVisitor' packages/es-markdown-core/src/walking-visitor.ts \
    && grep -q 'export function walk(root: Markup, walkingVisitor: WalkingVisitor)' \
        packages/es-markdown-core/src/walking-visitor.ts \
    || fail "ES does not expose the typed walking visitor contract"
test "$(grep -c '^    visit[A-Z].*(this: void, node:' packages/es-markdown-core/src/walking-visitor.ts)" -eq "$kind_count" \
    || fail "ES WalkingVisitor is not exhaustive over all $kind_count Markup kinds"
for value in $scoped_values; do
    test "$(grep -c "^    visit$value(this: void, value: $value, phase: WalkPhase): void;" packages/es-markdown-core/src/walking-visitor.ts)" -eq 1 \
        || fail "ES WalkingVisitor does not name the scoped value $value exactly once"
done

node - packages/es-markdown-core/package.json packages/es-markdown-core/src/index.ts <<'NODE'
import fs from "node:fs";

const [, , manifestPath, runtimePath] = process.argv;
const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
const rootExport = manifest.exports?.["."];
const exportKeys = Object.keys(manifest.exports ?? {}).sort();
if (exportKeys.join("\n") !== ".\n./markdown-core.wasm") {
    throw new Error(`Unexpected npm export paths: ${exportKeys.join(", ")}`);
}
if (rootExport?.types !== "./dist/index.d.ts" || rootExport?.import !== "./dist/index.js") {
    throw new Error("npm root export does not point at the reviewed ESM and declaration files");
}
const runtime = fs.readFileSync(runtimePath, "utf8");
const runtimeExports = [
    ...[...runtime.matchAll(/^export (?:class|const) ([A-Za-z0-9_]+)/gm)].map(
        (match) => match[1]
    ),
    ...[...runtime.matchAll(/^export \{ ([^}]+) \} from /gm)].flatMap((match) =>
        match[1].split(",").map((name) => name.trim())
    )
].sort();
const expectedRuntime = ["Attributes", "Document", "ParseError", "TreeDumper", "visit", "walk"].sort();
if (runtimeExports.join("\n") !== expectedRuntime.join("\n")) {
    throw new Error(`Unexpected ES runtime exports: ${runtimeExports.join(", ")}`);
}
NODE

if grep -q '"paths"' packages/es-markdown-core/tests/types/tsconfig.json \
    || grep -R -n -E '(\.\./)+dist/index\.d\.ts' packages/es-markdown-core/tests/types; then
    fail "ES type consumer bypasses installed-package exports.types resolution"
fi

grep -q '^group = "com.nouprax"$' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin group coordinate drifted"
grep -q 'artifactId = "kotlin-markdown-core-android-runtime"' \
    packages/kotlin-markdown-core/android-runtime/build.gradle.kts \
    || fail "internal Android runtime coordinate drifted"

cmp LICENSE packages/es-markdown-core/LICENSE >/dev/null \
    || fail "npm package license attribution differs from the repository license"

echo "Public surface audit passed."
