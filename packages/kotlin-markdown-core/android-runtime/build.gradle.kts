import org.gradle.api.component.SoftwareComponent
import org.gradle.api.publish.maven.MavenPublication
import org.gradle.jvm.tasks.Jar

plugins {
    alias(libs.plugins.android.library)
    `maven-publish`
}

group = "com.nouprax"
val releaseVersion = rootProject.file("VERSION").readText().trim()
version = releaseVersion

// WHICH ABIs THE NATIVE PAYLOAD IS BUILT FOR, narrowed by the caller.
// `scripts/build-kotlin-android-test-artifact.sh` passes
// `-PmarkdownCore.android.abis=x86_64` and then REFUSES an artifact carrying
// anything else, because an instrumentation APK for one emulator has no use for
// the other three and pays their build time and size. Unset, every ABI is built,
// which is what a release needs.
val requestedAndroidAbis =
    providers
        .gradleProperty("markdownCore.android.abis")
        .orNull
        ?.split(',')
        ?.map(String::trim)
        ?.filter(String::isNotEmpty)

dependencyLocking {
    lockAllConfigurations()
}

val sourcesJar =
    tasks.register<Jar>("sourcesJar") {
        archiveClassifier.set("sources")
        from("src/main")
        from(project(":packages:kotlin-markdown-core").file("src/native")) {
            into("jni")
        }
    }
val javadocJar =
    tasks.register<Jar>("javadocJar") {
        archiveClassifier.set("javadoc")
        from(project(":packages:kotlin-markdown-core").file("README.md"))
        from(rootProject.file("docs/releases/$releaseVersion.md"))
    }

android {
    namespace = "com.nouprax.markdown.core.android.runtime"
    compileSdk = libs.versions.android.compile.sdk.get().toInt()

    defaultConfig {
        minSdk = libs.versions.android.min.sdk.get().toInt()
        requestedAndroidAbis?.let { abis ->
            ndk { abiFilters += abis }
        }
        externalNativeBuild {
            cmake { arguments += "-DANDROID_STL=none" }
        }
    }

    // Keep the real JNI target in the IDE model. Android Studio and IntelliJ
    // therefore see the same production parser sources SwiftPM exposes, plus
    // the JNI adapter that owns this runtime. The target deliberately excludes
    // the C CLI, tests, fixtures, fuzzers, and benchmarks.
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    publishing { singleVariant("release") }
}

components.withType<SoftwareComponent>().matching { it.name == "release" }.all {
    val releaseComponent = this
    publishing.publications.register<MavenPublication>("release") {
        from(releaseComponent)
        artifactId = "kotlin-markdown-core-android-runtime"
        artifact(sourcesJar)
        artifact(javadocJar)

        pom {
            name.set("Kotlin Markdown Core Android runtime")
            description.set("Android JNI runtime used by the Kotlin Multiplatform Android publication.")
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
    }
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
}
