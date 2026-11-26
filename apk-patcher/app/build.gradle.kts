plugins {
    id("com.android.application")
}

android {
    namespace = "com.sampota.patchapktool"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.sampota.patchapktool"
        minSdk = 23
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        resources {
            excludes += setOf(
                "META-INF/DEPENDENCIES",
                "META-INF/LICENSE",
                "META-INF/LICENSE.txt",
                "META-INF/NOTICE",
                "META-INF/NOTICE.txt",
                "META-INF/INDEX.LIST"
            )
        }
    }
}

dependencies {
    implementation("com.android.tools.build:apksig:8.9.1")
    implementation("org.bouncycastle:bcpkix-jdk18on:1.80")
    implementation("androidx.documentfile:documentfile:1.0.1")
}
