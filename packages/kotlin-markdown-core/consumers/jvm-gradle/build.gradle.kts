plugins {
    kotlin("jvm") version "2.4.0"
    application
}

dependencies {
    implementation("com.nouprax:kotlin-markdown-core-jvm:2.0.0")
}

application {
    mainClass.set("consumer.MainKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
