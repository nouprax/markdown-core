plugins {
    alias(libs.plugins.jvm)
    alias(libs.plugins.ktlint)
}

dependencies {
    implementation(libs.kotlin.stdlib)
}

kotlin {
    jvmToolchain(
        libs.versions.jdk
            .get()
            .toInt(),
    )
}

ktlint {
    version.set(libs.versions.ktlintCli)
    outputToConsole.set(true)
    ignoreFailures.set(false)
}

dependencyLocking {
    lockAllConfigurations()
}
