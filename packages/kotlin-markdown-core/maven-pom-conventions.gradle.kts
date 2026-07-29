import org.gradle.api.publish.PublishingExtension
import org.gradle.api.publish.maven.MavenPublication

// Shared Maven publishing conventions for the Kotlin Markdown Core projects:
// the optional releaseStaging repository and the common POM identity
// (url, licenses, scm, developers). An applying project sets the
// per-publication name and description through the markdownCorePomName and
// markdownCorePomDescription extra properties before applying this script.
val pomName = extra["markdownCorePomName"] as String
val pomDescription = extra["markdownCorePomDescription"] as String

configure<PublishingExtension> {
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
            name.set(pomName)
            description.set(pomDescription)
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
