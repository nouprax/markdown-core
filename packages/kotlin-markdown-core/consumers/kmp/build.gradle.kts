plugins {
    kotlin("multiplatform") version "2.4.0"
}

kotlin {
    jvm()
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:1.0.2")
        }
        commonTest.dependencies { implementation(kotlin("test")) }
    }
}

tasks.withType<Test>().configureEach {
    useJUnitPlatform()
    jvmArgs("--enable-native-access=ALL-UNNAMED")
}
