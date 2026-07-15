plugins {
    id("com.android.application") version "9.3.0"
}

android {
    namespace = "consumer"
    compileSdk = 36
    defaultConfig {
        applicationId = "consumer.markdowncore"
        minSdk = 21
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }
}

dependencies {
    implementation("com.nouprax:kotlin-markdown-core:1.0.1")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("androidx.test:runner:1.7.0")
}
