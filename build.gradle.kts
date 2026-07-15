import org.gradle.api.artifacts.VersionCatalogsExtension

plugins {
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.android.kotlin.multiplatform.library) apply false
    alias(libs.plugins.jvm) apply false
    alias(libs.plugins.kotlin.multiplatform) apply false
    alias(libs.plugins.ktlint)
}

ktlint {
    version.set(libs.versions.ktlintCli)
    android.set(true)
    outputToConsole.set(true)
    ignoreFailures.set(false)
    filter {
        exclude("**/build/**")
        exclude("**/generated/**")
    }
}

dependencyLocking {
    lockAllConfigurations()
}

val stableKotlinVersion =
    extensions
        .getByType<VersionCatalogsExtension>()
        .named("libs")
        .findVersion("kotlin")
        .get()
        .requiredVersion

allprojects {
    configurations.configureEach {
        resolutionStrategy.eachDependency {
            if (requested.group == "org.jetbrains.kotlin" && requested.version?.contains('-') == true) {
                useVersion(stableKotlinVersion)
                because("repository policy allows stable Kotlin toolchain components only")
            }
        }
    }
}

tasks.register("allKotlinTests") {
    group = "verification"
    description = "Runs every Kotlin correctness and conformance suite supported by this host."
    dependsOn(":packages:kotlin-markdown-core:allKotlinTests")
}
