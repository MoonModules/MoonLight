plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "org.moonmodules.moonlight"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.moonmodules.moonlight"
        minSdk = 28        // Android 9, the plan's floor: old enough for a 2018 tablet
        targetSdk = 35
        // The release's version, which build_android.py passes, so the app and the program name one version.
        versionName = (project.findProperty("mmVersion") as String?) ?: "0.0.0-dev"
        versionCode = ((project.findProperty("mmVersionCode") as String?) ?: "1").toInt()
        ndk { abiFilters += "arm64-v8a" }
    }

    // The program, built by build_android.py with the NDK, rather than a second CMake build inside Gradle.
    sourceSets["main"].jniLibs.srcDirs("../../build/android-arm64/jniLibs")
    // The launcher icon, made from the installer's favicon by build_android.py rather than kept as images here.
    sourceSets["main"].res.srcDirs("../../build/android-arm64/res")

    // The release key, from the environment CI sets from the repository's secrets; without it the release build is debug-signed, which installs but cannot update an app the release key signed.
    val keystore = System.getenv("MM_ANDROID_KEYSTORE")
    signingConfigs {
        if (keystore != null) create("release") {
            storeFile = file(keystore)
            storePassword = System.getenv("MM_ANDROID_KEYSTORE_PASSWORD")
            keyAlias = System.getenv("MM_ANDROID_KEY_ALIAS")
            keyPassword = System.getenv("MM_ANDROID_KEY_PASSWORD")
        }
    }
    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (keystore != null) "release" else "debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
