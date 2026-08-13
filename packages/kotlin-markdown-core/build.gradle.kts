import groovy.json.JsonSlurper
import org.gradle.api.DefaultTask
import org.gradle.api.file.ConfigurableFileCollection
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.provider.Property
import org.gradle.api.provider.Provider
import org.gradle.api.publish.maven.MavenPublication
import org.gradle.api.tasks.CacheableTask
import org.gradle.api.tasks.Classpath
import org.gradle.api.tasks.CompileClasspath
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.InputFile
import org.gradle.api.tasks.InputFiles
import org.gradle.api.tasks.Nested
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.OutputFile
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.gradle.jvm.tasks.Jar
import org.gradle.jvm.toolchain.JavaCompiler
import org.gradle.jvm.toolchain.JavaLanguageVersion
import org.gradle.jvm.toolchain.JavaToolchainService
import org.gradle.language.jvm.tasks.ProcessResources
import org.gradle.process.ExecOperations
import org.gradle.testing.jacoco.plugins.JacocoTaskExtension
import org.gradle.testing.jacoco.tasks.JacocoReport
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinVersion
import org.jetbrains.kotlin.gradle.dsl.abi.ExperimentalAbiValidation
import org.jetbrains.kotlin.gradle.plugin.mpp.KotlinNativeTarget
import org.jetbrains.kotlin.gradle.targets.jvm.KotlinJvmTarget
import org.jetbrains.kotlin.gradle.targets.native.tasks.KotlinNativeTest
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.File
import java.util.Properties
import java.util.SortedSet
import java.util.zip.ZipFile
import javax.inject.Inject

@CacheableTask
abstract class GenerateCanonicalAstFixtures : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val specDirectory: DirectoryProperty

    @get:OutputFile
    abstract val outputFile: RegularFileProperty

    @TaskAction
    fun generate() {
        val root =
            specDirectory
                .get()
                .asFile.canonicalFile
                .toPath()
        val manifestFile = root.resolve("manifest.json").toFile()
        val manifest = JsonSlurper().parse(manifestFile) as? Map<*, *> ?: error("manifest must be an object")
        require((manifest["schemaVersion"] as? Number)?.toInt() == 1) {
            "shared canonical AST manifest must use schemaVersion 1"
        }
        val cases = manifest["cases"] as? List<*> ?: error("manifest cases must be an array")
        require(cases.isNotEmpty()) { "shared canonical AST manifest must contain at least one case" }

        fun caseText(relativePath: String): String {
            val path =
                root
                    .resolve(relativePath)
                    .normalize()
                    .toFile()
                    .canonicalFile
                    .toPath()
            require(path.startsWith(root)) { "canonical AST case path escapes the spec directory: $relativePath" }
            return path.toFile().readText()
        }

        val optionNames =
            listOf(
                "smartPunctuation",
                "footnotes",
                "tables",
                "strikethrough",
                "autolinks",
                "taskLists",
                "formulas",
                "directives",
                "crossLinks",
                "embeds",
            )
        val lines =
            mutableListOf(
                "package com.nouprax.markdown.core",
                "",
                "internal data class CanonicalAstCase(",
                "    val name: String,",
                "    val source: String,",
                "    val expected: String,",
                "    val options: ParseOptions,",
                ")",
                "",
                "internal val canonicalAstCases: kotlin.collections.List<CanonicalAstCase> =",
                "    listOf(",
            )

        for (rawCase in cases) {
            val testCase = rawCase as? Map<*, *> ?: error("every manifest case must be an object")
            val name = testCase["name"] as? String ?: error("case name must be a string")
            val input = testCase["input"] as? String ?: error("$name input must be a string")
            val expected = testCase["expected"] as? String ?: error("$name expected must be a string")
            val parseOptions = testCase["parseOptions"] as? Map<*, *> ?: error("$name parseOptions must be an object")
            require(parseOptions.keys.toList() == optionNames) {
                "$name parseOptions must list the frozen option inventory in order"
            }

            lines += "        CanonicalAstCase("
            lines += "            name = ${kotlinLiteral(name)},"
            appendStringProperty(lines, "source", caseText(input))
            appendStringProperty(lines, "expected", caseText(expected))
            lines += "            options ="
            lines += "                ParseOptions("
            for (optionName in optionNames) {
                val value = parseOptions[optionName] as? Boolean ?: error("$name $optionName must be boolean")
                lines += "                    $optionName = $value,"
            }
            lines += "                ),"
            lines += "        ),"
        }
        lines += "    )"
        lines += ""

        val destination = outputFile.get().asFile
        destination.parentFile.mkdirs()
        destination.writeText(lines.joinToString("\n"))
    }

    private fun appendStringProperty(
        lines: MutableList<String>,
        name: String,
        value: String,
    ) {
        lines += "            $name ="
        lines += "                buildString {"
        // Chunks split on code-point boundaries, never inside a surrogate
        // pair: a lone surrogate cannot survive the UTF-8 encoding of the
        // generated source file.
        var start = 0
        while (start < value.length) {
            var end = minOf(start + 30, value.length)
            if (end < value.length && value[end - 1].isHighSurrogate() && value[end].isLowSurrogate()) {
                end += 1
            }
            lines += "                    append(${kotlinLiteral(value.substring(start, end))})"
            start = end
        }
        lines += "                },"
    }

    private fun kotlinLiteral(value: String): String =
        buildString {
            append('"')
            for (character in value) {
                when (character) {
                    '\\' -> {
                        append("\\\\")
                    }

                    '"' -> {
                        append("\\\"")
                    }

                    '\n' -> {
                        append("\\n")
                    }

                    '\r' -> {
                        append("\\r")
                    }

                    '\t' -> {
                        append("\\t")
                    }

                    '$' -> {
                        append('\\')
                        append('$')
                    }

                    else -> {
                        append(character)
                    }
                }
            }
            append('"')
        }
}

private object JavaAbi {
    fun surface(roots: Set<File>): kotlin.collections.List<String> {
        val lines = mutableListOf<String>()
        for (root in roots.sortedBy(File::getAbsolutePath)) {
            if (root.isDirectory) {
                for (classFile in root.walkTopDown().filter { it.isFile && it.extension == "class" }) {
                    lines += classSurface(classFile.readBytes())
                }
            } else {
                ZipFile(root).use { archive ->
                    for (entry in archive.entries()) {
                        if (entry.name.endsWith(".class") && entry.name.startsWith("com/nouprax/")) {
                            lines += classSurface(archive.getInputStream(entry).use { it.readBytes() })
                        }
                    }
                }
            }
        }
        return lines
            .filter {
                it.startsWith("class com/nouprax/") ||
                    it.startsWith("  field com/nouprax/") ||
                    it.startsWith("  method com/nouprax/")
            }.sorted()
    }

    /**
     * The classes Java source can actually USE: public, and carrying at least
     * one member Java can name.
     *
     * The member requirement is not a nicety. Kotlin emits a public class for
     * a `@JvmMultifileClass` facade whether or not anything public lands in
     * it, so a facade holding only `@JvmSynthetic` internals is a public class
     * name with nothing behind it — javac can import it and then do nothing
     * with it. Counting it as surface would force it into the public API dump,
     * which would be a lie about what the library offers.
     */
    fun classes(surface: kotlin.collections.List<String>): Set<String> {
        val withMembers =
            surface
                .asSequence()
                .filter { it.startsWith("  ") }
                .map { it.trim().substringAfter(' ').substringBefore('.') }
                .toSet()
        return surface
            .asSequence()
            .filter { it.startsWith("class ") }
            .map { it.substringAfter("class ").substringBefore(' ') }
            .filter { it in withMembers }
            .toSortedSet()
    }

    fun officialClasses(apiFile: File): Set<String> =
        apiFile
            .readLines()
            .asSequence()
            .filter { it.startsWith("public ") && " class " in it }
            .map { it.substringAfter(" class ").substringBefore(' ') }
            .toSortedSet()

    private fun classSurface(bytes: ByteArray): kotlin.collections.List<String> {
        val input = DataInputStream(bytes.inputStream())
        require(input.readInt() == -0x35014542) { "not a class file" }
        input.readUnsignedShort()
        input.readUnsignedShort()
        val poolCount = input.readUnsignedShort()
        val pool = arrayOfNulls<Any?>(poolCount)
        val classRefs = IntArray(poolCount)
        var slot = 1
        while (slot < poolCount) {
            when (val tag = input.readUnsignedByte()) {
                1 -> {
                    pool[slot] = input.readUTF()
                }

                7 -> {
                    classRefs[slot] = input.readUnsignedShort()
                }

                8, 16, 19, 20 -> {
                    input.skipBytes(2)
                }

                15 -> {
                    input.skipBytes(3)
                }

                3, 4, 9, 10, 11, 12, 17, 18 -> {
                    input.skipBytes(4)
                }

                5, 6 -> {
                    input.skipBytes(8)
                    slot += 1
                }

                else -> {
                    error("unsupported constant pool tag $tag")
                }
            }
            slot += 1
        }

        fun utf8At(index: Int): String = pool[index] as? String ?: error("constant pool index $index is not utf8")

        val access = input.readUnsignedShort()
        val thisClass = input.readUnsignedShort()
        val className = utf8At(classRefs[thisClass])
        val public = (access and 0x0001) != 0
        val synthetic = (access and 0x1000) != 0
        if (!public || synthetic) {
            return emptyList()
        }
        val superName =
            input.readUnsignedShort().let { index ->
                if (index == 0) "-" else utf8At(classRefs[index])
            }
        val interfaces =
            (0 until input.readUnsignedShort())
                .map { utf8At(classRefs[input.readUnsignedShort()]) }
                .sorted()
        val members = mutableListOf<String>()
        for (section in listOf("field", "method")) {
            repeat(input.readUnsignedShort()) {
                val memberAccess = input.readUnsignedShort()
                val name = utf8At(input.readUnsignedShort())
                val descriptor = utf8At(input.readUnsignedShort())
                repeat(input.readUnsignedShort()) {
                    input.skipBytes(2)
                    input.skipBytes(input.readInt())
                }
                val visible = (memberAccess and 0x0005) != 0
                val synthetic = (memberAccess and 0x1000) != 0
                if (visible && !synthetic) {
                    members += "  $section $className.$name $descriptor"
                }
            }
        }
        return listOf(
            "class $className extends $superName implements ${interfaces.joinToString(",")}",
        ) + members
    }
}

@CacheableTask
abstract class VerifyJavaImplementationHidden : DefaultTask() {
    @get:Nested
    abstract val javaCompiler: Property<JavaCompiler>

    // JavaCompiler's documented nested metadata does not expose the concrete
    // vendor and full runtime build as stable inputs. Both can still change
    // javac's output, so record them explicitly in this cacheable task.
    @get:Input
    abstract val javaCompilerVendor: Property<String>

    @get:Input
    abstract val javaCompilerRuntimeVersion: Property<String>

    @get:Input
    abstract val javaRelease: Property<Int>

    @get:Classpath
    abstract val libraryClasses: ConfigurableFileCollection

    @get:InputFile
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val officialApiFile: RegularFileProperty

    @get:CompileClasspath
    abstract val compileClasspath: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @get:Inject
    abstract val execOperations: ExecOperations

    @TaskAction
    fun verify() {
        val outputRoot = outputDirectory.get().asFile
        check(!outputRoot.exists() || outputRoot.deleteRecursively()) {
            "could not clear ${outputRoot.absolutePath}"
        }
        val sourceDirectory = outputRoot.resolve("source").apply { mkdirs() }
        val classesDirectory = outputRoot.resolve("classes").apply { mkdirs() }
        val classpath = (libraryClasses + compileClasspath).filter(File::exists).asPath

        val actualClasses = JavaAbi.classes(JavaAbi.surface(libraryClasses.files))
        val officialClasses = JavaAbi.officialClasses(officialApiFile.get().asFile)
        check(actualClasses == officialClasses) {
            val undocumented = (actualClasses - officialClasses).joinToString("\n")
            val missing = (officialClasses - actualClasses).joinToString("\n")
            "$path Java classes differ from the official API dump.\n" +
                "Undocumented:\n$undocumented\nMissing:\n$missing"
        }

        fun compile(
            name: String,
            source: String,
        ): Pair<Int, String> {
            val sourceFile = sourceDirectory.resolve("$name.java")
            sourceFile.writeText(source)
            val output = ByteArrayOutputStream()
            val result =
                execOperations.exec {
                    executable(javaCompiler.get().executablePath.asFile)
                    args(
                        "--release",
                        javaRelease.get().toString(),
                        "-proc:none",
                        "-classpath",
                        classpath,
                        "-d",
                        classesDirectory.absolutePath,
                        sourceFile.absolutePath,
                    )
                    standardOutput = output
                    errorOutput = output
                    isIgnoreExitValue = true
                }
            return result.exitValue to output.toString(Charsets.UTF_8)
        }

        val (positiveResult, positiveOutput) =
            compile(
                "PublicApiProbe",
                """
                import com.nouprax.markdown.core.Commit;
                import com.nouprax.markdown.core.Diff;
                import com.nouprax.markdown.core.Document;
                import java.util.List;

                final class PublicApiProbe {
                    Document parse() {
                        return new Document("visible");
                    }

                    List<Diff> edit(Document document) {
                        Commit commit = document.edit("visible again");
                        commit.getDocument().close();
                        return commit.getDelta().getDiffs();
                    }

                    boolean retired(Diff diff) {
                        return diff.getParts().isRetired();
                    }
                }
                """.trimIndent(),
            )
        check(positiveResult == 0) {
            "Java public-API control failed to compile:\n$positiveOutput"
        }

        val hiddenTypes =
            listOf(
                "Built",
                "DesktopNativeLoader",
                "HostNativeLibrary",
                "JvmNative",
                "MarkdownCoreKt__CBridgeKt",
                "MarkdownCoreKt__CBridge_androidKt",
                "MarkdownCoreKt__CBridge_jvmKt",
                "MarkdownCoreKt__CBridge_jvmSharedKt",
                "MarkdownCoreKt__CommitKt",
                "MarkdownCoreKt__DiagnosticKt",
                "MarkdownCoreKt__DocumentKt",
                "MarkdownCoreKt__HTMLBlockKt",
                "MarkdownCoreKt__ImmutableListKt",
                "MarkdownCoreKt__MarkupDumperKt",
                "MarkdownCoreKt__MarkupKt",
                "MarkdownCoreKt__ParseOptionsKt",
                "MarkdownCoreKt__WireDecoderKt",
                "MarkupDumper${'$'}WhenMappings",
                "MarkupDumperKt",
                "MarkupKt",
                "ParseOptionsKt",
                "RootSink",
                "WireKind",
                "WireReader",
            )
        for (type in hiddenTypes) {
            val (result, output) =
                compile(
                    "Hidden${type}Probe",
                    """
                    import com.nouprax.markdown.core.$type;

                    final class Hidden${type}Probe {
                        $type value;
                    }
                    """.trimIndent(),
                )
            check(result != 0) {
                "Java source unexpectedly imported implementation type $type"
            }
            check(output.contains(type)) {
                "Java rejected $type for an unrelated reason:\n$output"
            }
        }

        val hiddenMembers =
            listOf(
                Triple(
                    "DocumentHandleProbe",
                    "getHandle",
                    """
                    import com.nouprax.markdown.core.Document;

                    final class DocumentHandleProbe {
                        Object handle(Document document) {
                            return document.getHandle();
                        }
                    }
                    """.trimIndent(),
                ),
                Triple(
                    "BridgeFunctionProbe",
                    "cOpen",
                    """
                    import com.nouprax.markdown.core.MarkdownCoreKt;
                    import com.nouprax.markdown.core.ParseOptions;

                    final class BridgeFunctionProbe {
                        Object open() {
                            return MarkdownCoreKt.cOpen(new byte[0], new ParseOptions());
                        }
                    }
                    """.trimIndent(),
                ),
            )
        for ((name, expectedSymbol, source) in hiddenMembers) {
            val (result, output) = compile(name, source)
            check(result != 0) {
                "Java source unexpectedly called synthetic implementation member from $name"
            }
            check(output.contains(expectedSymbol)) {
                "Java rejected $name for an unrelated reason:\n$output"
            }
        }
    }
}

plugins {
    alias(libs.plugins.kotlin.multiplatform)
    alias(libs.plugins.android.kotlin.multiplatform.library)
    alias(libs.plugins.ktlint)
    alias(libs.plugins.dokka)
    jacoco
    `maven-publish`
}

group = "com.nouprax"
version = rootProject.file("VERSION").readText().trim()

dokka {
    dokkaSourceSets.configureEach {
        // The entire public API lives in commonMain; the platform source
        // sets hold internal actuals only (and the two Kotlin/Native sets
        // share one source root, which Dokka rejects outright).
        if (name != "commonMain") {
            suppress.set(true)
        }
    }
}

dependencyLocking {
    lockAllConfigurations()
}

val repositoryRoot = rootProject.layout.projectDirectory
val canonicalAstSpecDirectory = repositoryRoot.dir("specs/canonical-ast")
val generatedCanonicalAstSource =
    layout.buildDirectory.file(
        "generated/canonicalAstCommonTest/kotlin/com/nouprax/markdown/core/CanonicalAstCases.kt",
    )

// The one closed model of supported build hosts. Every host-dependent
// decision — JNI resource path, native library file name, Kotlin/Native
// test target, managed-device ABI, publish-local target — derives from this
// object, and a host outside the support matrix stops configuration here
// instead of silently producing artifacts labeled for another platform.
data class HostTriple(
    val os: String,
    val architecture: String,
) {
    val platform: String get() = "$os-$architecture"
    val kotlinNativeTarget: String get() = if (os == "macos") "macosArm64" else "linuxX64"
    val managedDeviceAbi: String get() = if (architecture == "arm64") "arm64-v8a" else "x86_64"
    val nativeLibraryFileName: String
        get() = if (os == "macos") "libmarkdown_core_kotlin.dylib" else "libmarkdown_core_kotlin.so"
}

val supportedHostTriples =
    setOf(
        HostTriple("macos", "arm64"),
        HostTriple("linux", "x64"),
    )

val hostTriple =
    run {
        val osName = System.getProperty("os.name").lowercase()
        val architectureName = System.getProperty("os.arch").lowercase()
        val os =
            when {
                osName.contains("mac") -> "macos"
                osName.contains("linux") -> "linux"
                osName.contains("windows") -> "windows"
                else -> osName
            }
        val architecture =
            when (architectureName) {
                "aarch64", "arm64" -> "arm64"
                "amd64", "x86_64" -> "x64"
                else -> architectureName
            }
        val triple = HostTriple(os, architecture)
        require(triple in supportedHostTriples) {
            "Unsupported build host: $osName/$architectureName. Supported hosts: " +
                supportedHostTriples.joinToString { it.platform } + "."
        }
        triple
    }

val androidManagedDeviceTestAbi = hostTriple.managedDeviceAbi
val jvmNativeBuildDirectory = layout.buildDirectory.dir("native/jvm")
val jvmNativeResourceDirectory = layout.buildDirectory.dir("generated/jvmResources")
val desktopPlatform = hostTriple.platform
val nativeOutputDirectory =
    jvmNativeResourceDirectory.map {
        it.dir("com/nouprax/markdown/core/native/$desktopPlatform")
    }
val androidRuntimeAar =
    project(":packages:kotlin-markdown-core:android-runtime")
        .layout.buildDirectory
        .file("outputs/aar/android-runtime-release.aar")

val generateCanonicalAstCommonTest =
    tasks.register<GenerateCanonicalAstFixtures>("generateCanonicalAstCommonTest") {
        group = "verification"
        description = "Generates common-test conformance data from the root canonical AST manifest."
        specDirectory.set(canonicalAstSpecDirectory)
        outputFile.set(generatedCanonicalAstSource)
    }

fun KotlinNativeTarget.configureNativeBridge() {
    val capitalizedTarget = name.replaceFirstChar { it.uppercase() }
    val buildDirectory = layout.buildDirectory.dir("native/$name")
    val archiveDirectory = layout.buildDirectory.dir("native/$name/archives")
    val generatedDefinitionDirectory = layout.buildDirectory.dir("generated/cinterop/$name")
    val configureTask =
        tasks.register<Exec>("configure${capitalizedTarget}NativeBridge") {
            inputs.files(
                repositoryRoot.files("CMakeLists.txt"),
                repositoryRoot.dir("packages/markdown-core/core"),
                repositoryRoot.dir("packages/markdown-core/extensions"),
                layout.projectDirectory.dir("src/native"),
            )
            outputs.dir(buildDirectory)
            commandLine(
                "cmake",
                "-S",
                repositoryRoot.asFile.absolutePath,
                "-B",
                buildDirectory.get().asFile.absolutePath,
                "-DMARKDOWN_CORE_TESTS=OFF",
                "-DMARKDOWN_CORE_SHARED=OFF",
                "-DMARKDOWN_CORE_STATIC=ON",
                "-DMARKDOWN_CORE_KOTLIN_NATIVE=ON",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=${archiveDirectory.get().asFile.absolutePath}",
            )
        }
    val buildTask =
        tasks.register<Exec>("build${capitalizedTarget}NativeBridge") {
            dependsOn(configureTask)
            inputs.files(
                repositoryRoot.dir("packages/markdown-core/core"),
                repositoryRoot.dir("packages/markdown-core/extensions"),
                layout.projectDirectory.dir("src/native"),
            )
            outputs.dir(archiveDirectory)
            commandLine(
                "cmake",
                "--build",
                buildDirectory.get().asFile.absolutePath,
                "--config",
                "Release",
                "--target",
                "markdown_core_kotlin_native",
                "--parallel",
            )
        }
    val generateDefinition =
        tasks.register<Copy>("generate${capitalizedTarget}NativeDefinition") {
            from(layout.projectDirectory.file("src/nativeInterop/cinterop/markdown_core_kotlin.def"))
            into(generatedDefinitionDirectory)
            filter { line: String ->
                line.replace("@LIBRARY_PATH@", archiveDirectory.get().asFile.absolutePath)
            }
        }

    compilations.getByName("main").cinterops.create("markdownCoreKotlin") {
        definitionFile.set(generatedDefinitionDirectory.map { it.file("markdown_core_kotlin.def") })
        compilerOpts("-I${layout.projectDirectory.dir("src/native").asFile.absolutePath}")
        tasks.named(interopProcessingTaskName).configure {
            dependsOn(buildTask, generateDefinition)
            inputs.dir(archiveDirectory)
        }
    }
}

val hostNativeTest = "${hostTriple.kotlinNativeTarget}Test"
val hostNativeConformanceTest = "${hostTriple.kotlinNativeTarget}ConformanceTest"

val configureJvmNative =
    tasks.register<Exec>("configureJvmNative") {
        inputs.files(
            repositoryRoot.files("CMakeLists.txt"),
            repositoryRoot.dir("packages/markdown-core/core"),
            repositoryRoot.dir("packages/markdown-core/extensions"),
            layout.projectDirectory.dir("src/native"),
        )
        outputs.dir(jvmNativeBuildDirectory)
        commandLine(
            "cmake",
            "-S",
            repositoryRoot.asFile.absolutePath,
            "-B",
            jvmNativeBuildDirectory.get().asFile.absolutePath,
            "-DMARKDOWN_CORE_TESTS=OFF",
            "-DMARKDOWN_CORE_SHARED=OFF",
            "-DMARKDOWN_CORE_STATIC=ON",
            "-DMARKDOWN_CORE_KOTLIN_JNI=ON",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${nativeOutputDirectory.get().asFile.absolutePath}",
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${nativeOutputDirectory.get().asFile.absolutePath}",
        )
    }

val buildJvmNative =
    tasks.register<Exec>("buildJvmNative") {
        dependsOn(configureJvmNative)
        inputs.files(
            repositoryRoot.dir("packages/markdown-core/core"),
            repositoryRoot.dir("packages/markdown-core/extensions"),
            layout.projectDirectory.dir("src/native"),
        )
        outputs.dir(nativeOutputDirectory)
        commandLine(
            "cmake",
            "--build",
            jvmNativeBuildDirectory.get().asFile.absolutePath,
            "--config",
            "Release",
            "--target",
            "markdown_core_kotlin_jni",
            "--parallel",
        )
    }

kotlin {
    explicitApi()
    // Kotlin's metadata-aware gate covers the supported Kotlin/JVM and KLIB
    // surfaces. Preserve reference dumps for targets unavailable on this host;
    // the bytecode-level JVM gate below remains intentionally complementary.
    @OptIn(ExperimentalAbiValidation::class)
    abiValidation {
        keepLocallyUnsupportedTargets.set(true)
    }
    compilerOptions {
        languageVersion.set(KotlinVersion.KOTLIN_2_2)
        apiVersion.set(KotlinVersion.KOTLIN_2_2)
    }

    jvm {
        compilerOptions {
            jvmTarget.set(JvmTarget.JVM_17)
            // jvmTarget only pins the classfile version; -Xjdk-release also
            // pins the JDK API surface, so a reference to a post-17 Java API
            // fails compilation instead of shipping silently.
            freeCompilerArgs.add("-Xjdk-release=17")
        }
        withSourcesJar(publish = true)
        testRuns["test"].executionTask.configure {
            useJUnitPlatform()
            filter.excludeTestsMatching("*AstTest*")
        }
        testRuns.create("conformance") {
            executionTask.configure {
                useJUnitPlatform()
                filter.includeTestsMatching("*AstTest*")
            }
        }
    }

    android {
        namespace = "com.nouprax.markdown.core"
        compileSdk =
            libs.versions.android.compile.sdk
                .get()
                .toInt()
        minSdk =
            libs.versions.android.min.sdk
                .get()
                .toInt()
        withJava()
        withHostTestBuilder {}.configure {}
        withDeviceTestBuilder { sourceSetTreeName = "test" }.configure {
            instrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
            managedDevices {
                localDevices {
                    create("markdownCoreApi36Page4k") {
                        device = "Pixel 10 Pro XL"
                        apiLevel = 36
                        systemImageSource = "google"
                        require64Bit = true
                        pageAlignment =
                            com.android.build.api.dsl.ManagedVirtualDevice.PageAlignment
                                .FORCE_4KB_PAGES
                    }
                    create("markdownCoreApi36Page16k") {
                        device = "Pixel 10 Pro XL"
                        apiLevel = 36
                        systemImageSource = "google"
                        require64Bit = true
                        pageAlignment =
                            com.android.build.api.dsl.ManagedVirtualDevice.PageAlignment
                                .FORCE_16KB_PAGES
                    }
                    configureEach { testedAbi = androidManagedDeviceTestAbi }
                }
                groups {
                    create("markdownCoreAndroidPageSizes") {
                        targetDevices.add(localDevices["markdownCoreApi36Page4k"])
                        targetDevices.add(localDevices["markdownCoreApi36Page16k"])
                    }
                }
            }
        }
        compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
        optimization {
            consumerKeepRules.apply {
                publish = true
                file("consumer-rules.pro")
            }
        }
    }

    macosArm64 {
        configureNativeBridge()
        testRuns.create("conformance")
    }
    linuxX64 {
        configureNativeBridge()
        testRuns.create("conformance")
    }

    // A custom JVM/Android source set disables automatic application of the
    // default Native hierarchy unless the template is reapplied explicitly.
    // Keep the standard nativeMain/nativeTest graph, then add only the one
    // reviewed JVM-bytecode edge below.
    applyDefaultHierarchyTemplate()

    sourceSets {
        commonMain.dependencies {
            api(libs.kotlin.stdlib)
        }
        // Both the desktop JVM and Android targets compile to JVM bytecode
        // against the same JNI bridge; raw-handle operations and the
        // host-library extraction helper live once here, leaving only an
        // expect/actual loader per target.
        val jvmSharedMain =
            create("jvmSharedMain") {
                dependsOn(commonMain.get())
            }
        jvmMain.get().dependsOn(jvmSharedMain)
        getByName("androidMain").dependsOn(jvmSharedMain)
        commonTest {
            kotlin.srcDir(layout.buildDirectory.dir("generated/canonicalAstCommonTest/kotlin"))
            dependencies {
                implementation(kotlin("test"))
                implementation(libs.kotlinx.coroutines.test)
            }
        }
        jvmTest.dependencies { implementation(kotlin("test-junit5")) }
        getByName("androidDeviceTest").dependencies {
            implementation("androidx.test.ext:junit:1.3.0")
            implementation("androidx.test:runner:1.7.0")
        }
        androidMain.dependencies {
            implementation(
                project.dependencies.project(":packages:kotlin-markdown-core:android-runtime"),
            )
        }
        // The Kotlin classes and the JNI payload they call are one artifact
        // published as two modules, so a consumer must not be able to pair
        // halves from different releases — highest-version-wins, a BOM, or a
        // `force` would otherwise do it silently. A project dependency
        // publishes only `requires`, so the strict version travels as a
        // constraint, and Gradle fails the resolution instead.
        project.dependencies.constraints.add(
            "androidMainImplementation",
            "com.nouprax:kotlin-markdown-core-android-runtime",
        ) {
            version { strictly(project.version.toString()) }
            because("the Kotlin classes and the JNI payload are one artifact split in two")
        }
        macosArm64Main { kotlin.srcDir("src/nativePlatformMain/kotlin") }
        linuxX64Main { kotlin.srcDir("src/nativePlatformMain/kotlin") }
    }
}

tasks.matching { it.name.startsWith("compile") && it.name.contains("Test") }.configureEach {
    dependsOn(generateCanonicalAstCommonTest)
}
tasks.matching { it.name.startsWith("runKtlint") && it.name.contains("CommonTest") }.configureEach {
    dependsOn(generateCanonicalAstCommonTest)
}

tasks.named<ProcessResources>("jvmProcessResources") {
    dependsOn(buildJvmNative)
    from(jvmNativeResourceDirectory)
}

val jvmTarget = kotlin.targets.getByName("jvm") as KotlinJvmTarget
val jvmMainCompilation = jvmTarget.compilations.getByName("main")
val jvmTestCompilation = jvmTarget.compilations.getByName("test")
val androidMainCompilation =
    kotlin.targets
        .getByName("android")
        .compilations
        .getByName("main")
val jvmLibraryJar = tasks.named<Jar>("jvmJar").flatMap { it.archiveFile }
tasks.register<Sync>("stageJvmTestArtifact") {
    dependsOn("jvmTestClasses", "jvmProcessResources")
    into(layout.buildDirectory.dir("ci-test-artifact/jvm"))
    from(jvmMainCompilation.output.allOutputs) { into("classes") }
    from(jvmTestCompilation.output.allOutputs) { into("classes") }
    from(jvmTestCompilation.runtimeDependencyFiles.filter(File::isFile)) { into("lib") }
}

val stageAndroidHostTestArtifact =
    tasks.register<Sync>("stageAndroidHostTestArtifact") {
        dependsOn(buildJvmNative, "compileAndroidHostTest")
        duplicatesStrategy = DuplicatesStrategy.EXCLUDE
        into(layout.buildDirectory.dir("ci-test-artifact/android-host"))
        from(nativeOutputDirectory) { into("native") }
    }
val benchmarkCompilation =
    jvmTarget.compilations.create("benchmark") {
        associateWith(jvmTarget.compilations.getByName("main"))
    }

tasks.register<JavaExec>("kotlinBenchmark") {
    group = "benchmark"
    description = "Runs Kotlin/JNI parse and immutable AST copy performance workloads."
    dependsOn(benchmarkCompilation.compileTaskProvider, "jvmProcessResources")
    classpath =
        benchmarkCompilation.output.allOutputs +
        jvmTarget.compilations
            .getByName("main")
            .output.allOutputs +
        benchmarkCompilation.runtimeDependencyFiles
    mainClass.set("com.nouprax.markdown.core.benchmark.BenchmarkKt")
    jvmArgs("--enable-native-access=ALL-UNNAMED")
}

tasks.register<Sync>("stageJvmBenchmarkArtifact") {
    dependsOn(benchmarkCompilation.compileTaskProvider, "jvmProcessResources")
    into(layout.buildDirectory.dir("ci-test-artifact/jvm-benchmark"))
    from(jvmMainCompilation.output.allOutputs) { into("classes") }
    from(benchmarkCompilation.output.allOutputs) { into("classes") }
    from(benchmarkCompilation.runtimeDependencyFiles.filter(File::isFile)) { into("lib") }
}

tasks.withType<Test>().configureEach {
    jvmArgs("--enable-native-access=ALL-UNNAMED")
}

// JVM-target coverage for the repository's one coverage gate.
//
// The Android and Native targets compile the same commonMain sources, so
// one instrumented JVM run measures the shared binding logic once rather than
// reporting every common file four times under four toolchains. The
// target-specific sources each keep their own gate through their own platform.
//
// Kover would be the idiomatic choice and is deliberately not used: it refuses
// to configure against the AGP Kotlin-multiplatform Android target this module
// declares, because it looks for the legacy `android` extension that target
// does not create.
jacoco {
    toolVersion = libs.versions.jacoco.get()
}

// Instrumentation is off unless the coverage run asks for it. Applying the
// JaCoCo plugin otherwise attaches the agent to every Test task, which would
// silently change what `pnpm test:kotlin-jvm` executes — the frozen test
// architecture owns that path, and a coverage tool is not entitled to alter
// it.
val coverageRequested = providers.gradleProperty("markdownCoreCoverage").isPresent()
tasks.withType<Test>().configureEach {
    extensions.configure<JacocoTaskExtension> { isEnabled = coverageRequested }
}

tasks.register<JacocoReport>("jvmCoverageReport") {
    group = "verification"
    description = "Aggregates JVM correctness and conformance coverage into one JaCoCo XML report."
    val correctness = tasks.named<Test>("jvmTest")
    val conformance = tasks.named<Test>("jvmConformanceTest")
    dependsOn(correctness, conformance)
    // Captured as a local so the task action holds a boolean rather than a
    // reference to this build script, which the configuration cache rejects.
    val instrumented = coverageRequested
    // Declaring the switch as an input is what makes the guard below reachable:
    // without it the task stays up to date across a toggle and would hand back
    // a report produced by an earlier instrumented run.
    inputs.property("markdownCoreCoverage", instrumented)
    doFirst {
        check(instrumented) {
            "jvmCoverageReport needs instrumentation: run it with -PmarkdownCoreCoverage, " +
                "or through scripts/coverage-kotlin-jvm.sh, which passes it."
        }
    }
    executionData.setFrom(
        correctness.map { it.extensions.getByType<JacocoTaskExtension>().destinationFile!! },
        conformance.map { it.extensions.getByType<JacocoTaskExtension>().destinationFile!! },
    )
    // Every source set that compiles into the measured classes, jvmSharedMain
    // included: it holds the JNI entry points and the loader, and leaving it
    // out did not stop JaCoCo analysing its classes — it only left the report
    // naming a file the gate could not resolve.
    sourceDirectories.setFrom(
        files("src/commonMain/kotlin", "src/jvmSharedMain/kotlin", "src/jvmMain/kotlin"),
    )
    classDirectories.setFrom(jvmMainCompilation.output.classesDirs)
    reports {
        xml.required.set(true)
        html.required.set(false)
        csv.required.set(false)
    }
}

for (target in listOf("macosArm64", "linuxX64")) {
    tasks.named<KotlinNativeTest>("${target}Test") {
        filter.excludeTestsMatching("*AstTest*")
    }
    tasks.named<KotlinNativeTest>("${target}ConformanceTest") {
        filter.includeTestsMatching("*AstTest*")
    }
}

val hostNativeLibraryPath =
    nativeOutputDirectory
        .get()
        .asFile
        .resolve(hostTriple.nativeLibraryFileName)
        .absolutePath
tasks.withType<Test>().matching { it.name == "testAndroidHostTest" }.configureEach {
    dependsOn(buildJvmNative)
    filter.excludeTestsMatching("*AstTest*")
    systemProperty("markdown.core.hostNativeLibrary", hostNativeLibraryPath)
}
val androidHostConformanceTest =
    tasks.register<Test>("androidHostConformanceTest") {
        group = "verification"
        description = "Runs Android host public-contract conformance checks."
        dependsOn(buildJvmNative, "compileAndroidHostTest")
        filter.includeTestsMatching("*AstTest*")
        systemProperty("markdown.core.hostNativeLibrary", hostNativeLibraryPath)
    }
afterEvaluate {
    val sourceTask = tasks.named<Test>("testAndroidHostTest").get()
    androidHostConformanceTest.configure {
        testClassesDirs = sourceTask.testClassesDirs
        classpath = sourceTask.classpath
    }
    stageAndroidHostTestArtifact.configure {
        from(sourceTask.testClassesDirs) { into("classes") }
        from(sourceTask.classpath.filter(File::isDirectory)) { into("classes") }
        from(sourceTask.classpath.filter(File::isFile)) { into("lib") }
    }
}

val javadocJar =
    tasks.register<Jar>("javadocJar") {
        archiveClassifier.set("javadoc")
        // Real generated API reference first; the canonical AST spec and
        // README ride along as supplementary content.
        from(tasks.named("dokkaGeneratePublicationHtml"))
        from(repositoryRoot.file("docs/specs/canonical-ast.md"))
        from(layout.projectDirectory.file("README.md"))
    }

extra["markdownCorePomName"] = "Kotlin Markdown Core"
extra["markdownCorePomDescription"] = "Immutable Kotlin Multiplatform AST bindings for Markdown Core."
apply(from = "maven-pom-conventions.gradle.kts")

publishing {
    publications.withType<MavenPublication>().configureEach {
        artifact(javadocJar)
    }
}

ktlint {
    version.set(libs.versions.ktlintCli)
    android.set(true)
    outputToConsole.set(true)
    ignoreFailures.set(false)
}

// AGP 9.2.1 exposed testedAbi in the managed-device DSL but its setup-task
// CreationAction did not copy that value into the task input. Keep the public
// DSL declaration above and the explicit task input on AGP 9.3.0 until the
// two-device remote smoke proves the upstream assignment on both page sizes.
tasks.withType<com.android.build.gradle.internal.tasks.ManagedDeviceInstrumentationTestSetupTask>().configureEach {
    testedAbi.set(androidManagedDeviceTestAbi)
}

// Complements Kotlin's metadata-aware ABI dumps by comparing the classes
// ordinary Java source can use in the JVM and Android outputs against that
// dump's inventory. javac deliberately hides ACC_SYNTHETIC classes/members;
// explicit negative compile probes below pin that boundary as well.
// Module-internal top-level declarations live in package-private multifile
// parts, while their synthetic forwarders share one MarkdownCoreKt owner
// carrying no member Java can name.
val javaToolchains = extensions.getByType<JavaToolchainService>()
val javaProbeCompiler =
    javaToolchains.compilerFor {
        languageVersion.set(JavaLanguageVersion.of(libs.versions.jdk.get()))
    }

fun VerifyJavaImplementationHidden.useJavaCompiler(
    compiler: Provider<JavaCompiler>,
    release: Provider<String>,
) {
    javaCompiler.set(compiler)
    javaCompilerVendor.set(compiler.map { it.metadata.vendor })
    javaCompilerRuntimeVersion.set(compiler.map { it.metadata.javaRuntimeVersion })
    javaRelease.set(release.map { it.toInt() })
}

/**
 * Fails when the Android classes reference a platform type the floor they
 * declare did not have.
 *
 * Nothing else catches this. ktlint reads formatting; the host tests run on a
 * real JDK, where every `java.*` type exists whatever `minSdk` says; and every
 * managed device sits above the floor — which is how a call to an API-33 class
 * shipped in an artifact advertising API 21. The SDK ships the answer in
 * `api-versions.xml`, so the check is a lookup, not a heuristic.
 *
 * Two sources, because a type can reach the pool by either. A call, a field
 * access, a `new`, a cast and a supertype all become a class entry — that is
 * what the runtime failure looks like, a class the verifier cannot resolve.
 * A type named only in a descriptor never becomes one: a method that returns a
 * newer platform type and whose body returns null puts that type in a UTF-8
 * entry and nowhere else, and it is still API the artifact declares. So the
 * UTF-8 entries are read for descriptor forms too. Matching against the SDK's
 * own class list is what keeps that from over-reaching: a UTF-8 run has to
 * name a real platform type to count.
 */
@CacheableTask
abstract class VerifyAndroidApiFloor : DefaultTask() {
    private companion object {
        /**
         * The one family this reads that never reaches a device.
         *
         * Kotlin compiles lambdas and string concatenation to `invokedynamic`,
         * whose bootstrap metadata names `MethodHandle`, `MethodType` and
         * `CallSite` — all API 26 — in almost every class here. D8 rewrites
         * every one of them into concrete classes below API 26, and does so
         * unconditionally rather than as the core-library desugaring this
         * project does not enable. Reading JVM bytecode rather than DEX is
         * what makes them visible at all.
         *
         * The cost is real and bounded: an explicit `MethodHandles.lookup()`
         * would be waved through. Nothing here has one, and the families D8
         * only desugars on request — `java.time`, `java.util.stream`,
         * `java.util.function` — stay checked.
         */
        const val DESUGARED_PREFIX = "java/lang/invoke/"
    }

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val libraryClasses: ConfigurableFileCollection

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val apiVersions: RegularFileProperty

    @get:Input
    abstract val minSdk: Property<Int>

    @get:OutputFile
    abstract val reportFile: RegularFileProperty

    @TaskAction
    fun verify() {
        val floor = minSdk.get()
        val introducedIn = platformApiLevels(apiVersions.get().asFile)
        val violations = sortedMapOf<String, SortedSet<String>>()
        var scanned = 0
        for (root in libraryClasses.files) {
            if (!root.isDirectory) {
                continue
            }
            root
                .walkTopDown()
                .filter { it.isFile && it.extension == "class" }
                .forEach { classFile ->
                    scanned += 1
                    val owner =
                        classFile
                            .toRelativeString(root)
                            .removeSuffix(".class")
                            .replace(File.separatorChar, '/')
                    for (referenced in referencedClasses(classFile.readBytes())) {
                        if (referenced.startsWith(DESUGARED_PREFIX)) {
                            continue
                        }
                        val level = introducedIn[referenced] ?: continue
                        if (level > floor) {
                            violations
                                .getOrPut("$referenced (API $level)") { sortedSetOf() }
                                .add(owner)
                        }
                    }
                }
        }
        check(scanned > 0) { "no Android classes to check; the compilation produced nothing" }
        val report =
            buildString {
                appendLine("minSdk=$floor classes=$scanned violations=${violations.size}")
                violations.forEach { (reference, owners) ->
                    appendLine("$reference used by ${owners.joinToString(", ")}")
                }
            }
        reportFile
            .get()
            .asFile
            .apply { parentFile.mkdirs() }
            .writeText(report)
        check(violations.isEmpty()) {
            "the Android artifact declares minSdk $floor and references newer platform types:\n$report"
        }
        logger.lifecycle("Android API floor verified: $scanned classes against minSdk $floor.")
    }

    /** `<class name="…" since="N">` rows; a class with no `since` has been
     * there since API 1. */
    private fun platformApiLevels(file: File): Map<String, Int> {
        val row = Regex("""<class name="([^"]+)"(?:[^>]*\ssince="(\d+)")?""")
        val levels = HashMap<String, Int>()
        file.forEachLine { line ->
            val match = row.find(line) ?: return@forEachLine
            levels[match.groupValues[1]] = match.groupValues[2].toIntOrNull() ?: 1
        }
        check(levels.isNotEmpty()) { "no class rows in ${file.absolutePath}" }
        return levels
    }

    private fun referencedClasses(bytes: ByteArray): Set<String> {
        val input = DataInputStream(bytes.inputStream())
        require(input.readInt() == -0x35014542) { "not a class file" }
        input.readUnsignedShort()
        input.readUnsignedShort()
        val poolCount = input.readUnsignedShort()
        val text = arrayOfNulls<String>(poolCount)
        // A class entry may name a slot the walk has not reached, so the names
        // are resolved after the pool is complete.
        val nameSlots = mutableListOf<Int>()
        var slot = 1
        while (slot < poolCount) {
            when (val tag = input.readUnsignedByte()) {
                1 -> {
                    text[slot] = input.readUTF()
                }

                7 -> {
                    nameSlots += input.readUnsignedShort()
                }

                8, 16, 19, 20 -> {
                    input.skipBytes(2)
                }

                15 -> {
                    input.skipBytes(3)
                }

                3, 4, 9, 10, 11, 12, 17, 18 -> {
                    input.skipBytes(4)
                }

                5, 6 -> {
                    input.skipBytes(8)
                    slot += 1
                }

                else -> {
                    error("unsupported constant pool tag $tag")
                }
            }
            slot += 1
        }
        val referenced = mutableSetOf<String>()
        nameSlots
            .mapNotNull { text[it] }
            // An array type carries its own class entry; the platform answers
            // for the element type.
            .map { it.trimStart('[') }
            .mapTo(referenced) {
                if (it.startsWith("L") && it.endsWith(";")) it.substring(1, it.length - 1) else it
            }
        // Descriptors and generic signatures live in UTF-8 entries and name
        // their types the same way: `Lpkg/Name;`, or `Lpkg/Name<` once type
        // arguments follow.
        val descriptor = Regex("L([A-Za-z0-9_/\$]+)[;<]")
        for (entry in text) {
            if (entry == null) {
                continue
            }
            descriptor.findAll(entry).forEach { referenced += it.groupValues[1] }
        }
        return referenced
    }
}

/** The SDK the Android tooling would use, from the same places it looks. */
val androidSdkDirectory: File? =
    sequenceOf(
        providers.gradleProperty("sdk.dir").orNull,
        rootProject.file("local.properties").takeIf { it.isFile }?.let { file ->
            val properties = Properties()
            file.inputStream().use { properties.load(it) }
            properties.getProperty("sdk.dir")
        },
        providers.environmentVariable("ANDROID_HOME").orNull,
        providers.environmentVariable("ANDROID_SDK_ROOT").orNull,
    ).filterNotNull()
        .map(::File)
        .firstOrNull(File::isDirectory)

val verifyAndroidApiFloor =
    tasks.register<VerifyAndroidApiFloor>("verifyAndroidApiFloor") {
        group = "verification"
        description = "Proves the Android classes reference no platform type newer than minSdk."
        libraryClasses.from(androidMainCompilation.output.classesDirs)
        minSdk.set(
            libs.versions.android.min.sdk
                .map(String::toInt),
        )
        val compileSdk =
            libs.versions.android.compile.sdk
                .get()
        val sdk =
            androidSdkDirectory
                ?: error(
                    "no Android SDK found for verifyAndroidApiFloor; set ANDROID_HOME or sdk.dir in local.properties",
                )
        val versions = File(sdk, "platforms/android-$compileSdk/data/api-versions.xml")
        check(versions.isFile) { "missing ${versions.absolutePath}; install the android-$compileSdk platform" }
        apiVersions.set(versions)
        reportFile.set(layout.buildDirectory.file("verification/android-api-floor.txt"))
    }

val verifyJvmImplementationHidden =
    tasks.register<VerifyJavaImplementationHidden>("verifyJvmImplementationHidden") {
        group = "verification"
        description = "Proves ordinary Java source cannot use Kotlin/JVM implementation types or members."
        useJavaCompiler(javaProbeCompiler, libs.versions.jvm.bytecode)
        libraryClasses.from(jvmLibraryJar)
        officialApiFile.set(layout.projectDirectory.file("api/jvm/kotlin-markdown-core.api"))
        compileClasspath.from(jvmMainCompilation.compileDependencyFiles)
        outputDirectory.set(layout.buildDirectory.dir("verification/jvm-implementation-hidden"))
    }

val verifyAndroidImplementationHidden =
    tasks.register<VerifyJavaImplementationHidden>("verifyAndroidImplementationHidden") {
        group = "verification"
        description = "Proves ordinary Java source cannot use Kotlin/Android implementation types or members."
        useJavaCompiler(javaProbeCompiler, libs.versions.jvm.bytecode)
        libraryClasses.from(androidMainCompilation.output.classesDirs)
        officialApiFile.set(layout.projectDirectory.file("api/jvm/kotlin-markdown-core.api"))
        compileClasspath.from(androidMainCompilation.compileDependencyFiles)
        outputDirectory.set(layout.buildDirectory.dir("verification/android-implementation-hidden"))
    }

tasks.register("kotlinTest") {
    group = "verification"
    description = "Runs JVM, Android host, Native correctness, packaging, and ABI checks."
    dependsOn(
        listOfNotNull(
            "jvmTest",
            "testAndroidHostTest",
            hostNativeTest,
            "verifyKotlinNativePackaging",
            verifyJvmImplementationHidden,
            verifyAndroidImplementationHidden,
            verifyAndroidApiFloor,
            "checkKotlinAbi",
        ),
    )
}

tasks.register("allKotlinTests") {
    group = "verification"
    description =
        "Runs all Kotlin correctness and conformance suites supported by this host, " +
        "including both Android managed devices."
    dependsOn(
        listOfNotNull(
            "jvmTest",
            "jvmConformanceTest",
            "testAndroidHostTest",
            "androidHostConformanceTest",
            hostNativeTest,
            hostNativeConformanceTest,
            "markdownCoreApi36Page4kAndroidDeviceTest",
            "markdownCoreApi36Page16kAndroidDeviceTest",
        ),
    )
}

tasks.register("verifyKotlinNativePackaging") {
    group = "verification"
    description = "Verifies the current desktop JNI payload and all supported Android ABIs."
    dependsOn("jvmJar", ":packages:kotlin-markdown-core:android-runtime:assembleRelease")
    val runtimeAarFile = androidRuntimeAar.get().asFile
    val desktopOutputDirectory = nativeOutputDirectory.get().asFile
    val expectedDesktopPlatform = desktopPlatform
    val desktopLibrary = hostTriple.nativeLibraryFileName
    val expectedDesktopArchitecture =
        if (hostTriple.os == "macos") "macho-${hostTriple.architecture}" else "elf-${hostTriple.architecture}"
    inputs.file(runtimeAarFile)

    doLast {
        // Reads enough of an ELF or Mach-O header to name the machine architecture,
        // so packaging checks verify what a native library is, not merely that a
        // file with the right name exists.
        fun binaryArchitecture(bytes: ByteArray): String {
            require(bytes.size >= 20) { "native library header is truncated" }

            fun u16(offset: Int) = (bytes[offset].toInt() and 0xff) or ((bytes[offset + 1].toInt() and 0xff) shl 8)

            fun u32(offset: Int) =
                (bytes[offset].toInt() and 0xff) or ((bytes[offset + 1].toInt() and 0xff) shl 8) or
                    ((bytes[offset + 2].toInt() and 0xff) shl 16) or ((bytes[offset + 3].toInt() and 0xff) shl 24)
            return when {
                bytes[0] == 0x7f.toByte() && bytes[1] == 'E'.code.toByte() &&
                    bytes[2] == 'L'.code.toByte() && bytes[3] == 'F'.code.toByte() -> {
                    when (u16(18)) {
                        0x3e -> "elf-x64"
                        0xb7 -> "elf-arm64"
                        0x28 -> "elf-arm32"
                        0x03 -> "elf-x86"
                        else -> "elf-unknown-${u16(18)}"
                    }
                }

                u32(0) == 0xfeedfacf.toInt() -> {
                    when (u32(4)) {
                        0x0100000c -> "macho-arm64"
                        0x01000007 -> "macho-x64"
                        else -> "macho-unknown-${u32(4)}"
                    }
                }

                else -> {
                    "unknown"
                }
            }
        }

        val expectedAndroidArchitectures =
            mapOf(
                "arm64-v8a" to "elf-arm64",
                "armeabi-v7a" to "elf-arm32",
                "x86" to "elf-x86",
                "x86_64" to "elf-x64",
            )
        ZipFile(runtimeAarFile).use { archive ->
            for ((abi, expectedArchitecture) in expectedAndroidArchitectures) {
                val entry =
                    archive.getEntry("jni/$abi/libmarkdown_core_kotlin.so")
                        ?: error("Android runtime AAR is missing jni/$abi/libmarkdown_core_kotlin.so")
                val header = archive.getInputStream(entry).use { it.readNBytes(20) }
                val architecture = binaryArchitecture(header)
                check(architecture == expectedArchitecture) {
                    "Android $abi payload has architecture $architecture, expected $expectedArchitecture"
                }
            }
        }

        val desktopFile = desktopOutputDirectory.resolve(desktopLibrary)
        check(desktopFile.isFile) {
            "JVM native payload is missing for $expectedDesktopPlatform"
        }
        val desktopArchitecture = binaryArchitecture(desktopFile.inputStream().use { it.readNBytes(20) })
        check(desktopArchitecture == expectedDesktopArchitecture) {
            "JVM native payload for $expectedDesktopPlatform has architecture " +
                "$desktopArchitecture, expected $expectedDesktopArchitecture"
        }
    }
}

tasks.withType<org.gradle.api.publish.maven.tasks.PublishToMavenLocal>().configureEach {
    when {
        name.contains("LinuxX64") && hostTriple.os != "linux" -> enabled = false
        name.contains("MacosArm64") && hostTriple.os != "macos" -> enabled = false
    }
}

tasks.register("publishKotlinToMavenLocal") {
    group = "publishing"
    description = "Publishes KMP metadata and all target artifacts buildable on this host."
    dependsOn(
        "publishKotlinMultiplatformPublicationToMavenLocal",
        "publishJvmPublicationToMavenLocal",
        "publishAndroidPublicationToMavenLocal",
        "publish${hostTriple.kotlinNativeTarget.replaceFirstChar { it.uppercase() }}PublicationToMavenLocal",
        ":packages:kotlin-markdown-core:android-runtime:publishToMavenLocal",
    )
}
