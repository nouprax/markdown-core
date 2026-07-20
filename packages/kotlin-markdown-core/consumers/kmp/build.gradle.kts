plugins {
    kotlin("multiplatform") version "2.4.0"
}

kotlin {
    jvm()
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:2.0.0")
        }
        commonTest.dependencies { implementation(kotlin("test")) }
    }
}

tasks.withType<Test>().configureEach {
    useJUnitPlatform()
    jvmArgs("--enable-native-access=ALL-UNNAMED")
}
