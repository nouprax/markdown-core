import groovy.json.JsonSlurper
import org.gradle.api.DefaultTask
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.publish.maven.MavenPublication
import org.gradle.api.tasks.CacheableTask
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.OutputFile
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.gradle.jvm.tasks.Jar
import org.gradle.language.jvm.tasks.ProcessResources
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinVersion
import org.jetbrains.kotlin.gradle.plugin.mpp.KotlinNativeTarget
import org.jetbrains.kotlin.gradle.targets.jvm.KotlinJvmTarget
import org.jetbrains.kotlin.gradle.targets.native.tasks.KotlinNativeTest
import java.util.zip.ZipFile

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
                "stripHTMLComments",
                "tables",
                "strikethrough",
                "autolinks",
                "taskLists",
                "formulas",
                "dollarFormulaDelimiters",
                "latexFormulaDelimiters",
                "directives",
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
        for (chunk in value.chunked(30)) {
            lines += "                    append(${kotlinLiteral(chunk)})"
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

plugins {
    alias(libs.plugins.kotlin.multiplatform)
    alias(libs.plugins.android.kotlin.multiplatform.library)
    alias(libs.plugins.ktlint)
    `maven-publish`
}

group = "com.nouprax"
version = rootProject.file("VERSION").readText().trim()

dependencyLocking {
    lockAllConfigurations()
}

val repositoryRoot = rootProject.layout.projectDirectory
val canonicalAstSpecDirectory = repositoryRoot.dir("specs/canonical-ast")
val generatedCanonicalAstSource =
    layout.buildDirectory.file(
        "generated/canonicalAstCommonTest/kotlin/com/nouprax/markdown/core/CanonicalAstCases.kt",
    )
val hostOs = System.getProperty("os.name").lowercase()
val hostArchitecture = System.getProperty("os.arch").lowercase()
val androidManagedDeviceTestAbi =
    when (hostArchitecture) {
        "aarch64", "arm64" -> "arm64-v8a"
        "amd64", "x86_64" -> "x86_64"
        else -> error("Unsupported Android managed-device host architecture: $hostArchitecture")
    }
val jvmNativeBuildDirectory = layout.buildDirectory.dir("native/jvm")
val jvmNativeResourceDirectory = layout.buildDirectory.dir("generated/jvmResources")
val desktopPlatform =
    when {
        System.getProperty("os.name").lowercase().contains("mac") &&
            hostArchitecture in setOf("aarch64", "arm64") -> "macos-arm64"

        System.getProperty("os.name").lowercase().contains("mac") -> "macos-x64"

        System.getProperty("os.name").lowercase().contains("windows") -> "windows-x64"

        else -> "linux-x64"
    }
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

val hostNativeTest =
    when {
        hostOs.contains("mac") -> "macosArm64Test"
        hostOs.contains("linux") -> "linuxX64Test"
        else -> null
    }
val hostNativeConformanceTest =
    when {
        hostOs.contains("mac") -> "macosArm64ConformanceTest"
        hostOs.contains("linux") -> "linuxX64ConformanceTest"
        else -> null
    }

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
    compilerOptions {
        languageVersion.set(KotlinVersion.KOTLIN_2_2)
        apiVersion.set(KotlinVersion.KOTLIN_2_2)
        // CSession is an expect class (one native handle type per
        // platform); the flag silences the expect/actual-classes Beta notice.
        freeCompilerArgs.add("-Xexpect-actual-classes")
    }

    jvm {
        compilerOptions.jvmTarget.set(JvmTarget.JVM_17)
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
    }

    macosArm64 {
        configureNativeBridge()
        testRuns.create("conformance")
    }
    linuxX64 {
        configureNativeBridge()
        testRuns.create("conformance")
    }

    sourceSets {
        commonMain.dependencies {
            api(libs.kotlin.stdlib)
            api(libs.kotlinx.coroutines.core)
        }
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
        .resolve(
            if (desktopPlatform.startsWith("macos")) {
                "libmarkdown_core_kotlin.dylib"
            } else if (desktopPlatform.startsWith("windows")) {
                "markdown_core_kotlin.dll"
            } else {
                "libmarkdown_core_kotlin.so"
            },
        ).absolutePath
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
        from(repositoryRoot.file("docs/specs/canonical-ast.md"))
        from(layout.projectDirectory.file("README.md"))
    }

publishing {
    repositories {
        providers.gradleProperty("releaseRepositoryDir").orNull?.let { repositoryDirectory ->
            maven {
                name = "releaseStaging"
                url = uri(repositoryDirectory)
            }
        }
    }
    publications.withType<MavenPublication>().configureEach {
        pom {
            name.set("Kotlin Markdown Core")
            description.set("Immutable Kotlin Multiplatform AST bindings for Markdown Core.")
            url.set("https://github.com/nouprax/markdown-core")
            licenses {
                license {
                    name.set("BSD-2-Clause")
                    url.set("https://github.com/nouprax/markdown-core/blob/main/COPYING")
                }
            }
            scm {
                connection.set("scm:git:https://github.com/nouprax/markdown-core.git")
                developerConnection.set("scm:git:ssh://git@github.com/nouprax/markdown-core.git")
                url.set("https://github.com/nouprax/markdown-core")
            }
            developers {
                developer {
                    id.set("nouprax")
                    name.set("Nouprax")
                    url.set("https://github.com/nouprax")
                }
            }
        }
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

tasks.register("kotlinTest") {
    group = "verification"
    description = "Runs JVM, Android host, and the current host's Kotlin/Native correctness suites."
    dependsOn(listOfNotNull("jvmTest", "testAndroidHostTest", hostNativeTest, "verifyKotlinNativePackaging"))
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
    inputs.file(runtimeAarFile)

    doLast {
        val expectedAndroidEntries =
            setOf("arm64-v8a", "armeabi-v7a", "x86", "x86_64").map {
                "jni/$it/libmarkdown_core_kotlin.so"
            }
        ZipFile(runtimeAarFile).use { archive ->
            val entries =
                archive
                    .entries()
                    .asSequence()
                    .map { it.name }
                    .toSet()
            check(entries.containsAll(expectedAndroidEntries)) {
                "Android runtime AAR is missing: ${expectedAndroidEntries - entries}"
            }
        }

        val desktopLibrary =
            when {
                expectedDesktopPlatform.startsWith("macos") -> "libmarkdown_core_kotlin.dylib"
                expectedDesktopPlatform.startsWith("windows") -> "markdown_core_kotlin.dll"
                else -> "libmarkdown_core_kotlin.so"
            }
        check(desktopOutputDirectory.resolve(desktopLibrary).isFile) {
            "JVM native payload is missing for $expectedDesktopPlatform"
        }
    }
}

tasks.withType<org.gradle.api.publish.maven.tasks.PublishToMavenLocal>().configureEach {
    when {
        name.contains("LinuxX64") && !hostOs.contains("linux") -> enabled = false
        name.contains("MacosArm64") && !hostOs.contains("mac") -> enabled = false
    }
}

tasks.register("publishKotlinToMavenLocal") {
    group = "publishing"
    description = "Publishes KMP metadata and all target artifacts buildable on this host."
    dependsOn(
        "publishKotlinMultiplatformPublicationToMavenLocal",
        "publishJvmPublicationToMavenLocal",
        "publishAndroidPublicationToMavenLocal",
        if (hostOs.contains("mac")) {
            "publishMacosArm64PublicationToMavenLocal"
        } else {
            "publishLinuxX64PublicationToMavenLocal"
        },
        ":packages:kotlin-markdown-core:android-runtime:publishToMavenLocal",
    )
}
