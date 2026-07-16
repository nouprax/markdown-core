import org.gradle.api.component.SoftwareComponent
import org.gradle.api.publish.maven.MavenPublication
import org.gradle.jvm.tasks.Jar

plugins {
    alias(libs.plugins.android.library)
    `maven-publish`
}

group = "com.nouprax"
version = rootProject.file("VERSION").readText().trim()

val isIdeSync =
    providers
        .systemProperty("idea.sync.active")
        .map(String::toBoolean)
        .getOrElse(false)
val requestedAndroidAbis =
    providers.gradleProperty("markdownCore.android.abis").orNull
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
            into("native-bridge")
        }
    }
val javadocJar =
    tasks.register<Jar>("javadocJar") {
        archiveClassifier.set("javadoc")
        from(project(":packages:kotlin-markdown-core").file("README.md"))
        from(rootProject.file("docs/migration/2026-07-12-phase-12-kotlin-binding.md"))
    }

android {
    namespace = "com.nouprax.markdown.core.android.runtime"
    compileSdk = libs.versions.android.compile.sdk.get().toInt()

    defaultConfig {
        minSdk = libs.versions.android.min.sdk.get().toInt()
        requestedAndroidAbis?.let { abis ->
            ndk { abiFilters += abis }
        }
        if (!isIdeSync) {
            externalNativeBuild {
                cmake { arguments += "-DANDROID_STL=none" }
            }
        }
    }

    // This CMake target intentionally compiles sources from both the C and
    // Kotlin packages. Importing that native workspace makes Android Studio
    // claim their common `packages/` ancestor as one native content root,
    // which hides the KMP source-set modules. IDE sync only needs the Gradle
    // and KMP models; real builds (including builds launched by the IDE) do
    // not set idea.sync.active and retain the complete JNI build graph.
    if (!isIdeSync) {
        externalNativeBuild {
            cmake {
                path = file("src/main/cpp/CMakeLists.txt")
                version = "3.22.1"
            }
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
