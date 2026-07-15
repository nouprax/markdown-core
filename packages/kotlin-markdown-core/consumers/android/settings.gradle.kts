pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }
dependencyResolutionManagement {
    repositories {
        google()
        providers.gradleProperty("consumerRepository").orNull?.let { repositoryDirectory ->
            maven { url = uri(repositoryDirectory) }
        } ?: mavenLocal()
        mavenCentral()
    }
}
rootProject.name = "kotlin-markdown-core-android-consumer"
