plugins {
    id("com.android.application")
}

val repoRoot = rootProject.projectDir.parentFile
val sdlSource = file((project.findProperty("sdlSource") as String?) ?: "${repoRoot}/build/android-deps/sdl3-src")
val o1Sources = (project.findProperty("gungnir.o1Sources") as String?) ?: ""
check(sdlSource.isDirectory) {
    "sdl_source_missing: $sdlSource (run scripts/android/build.sh)"
}

android {
    namespace = "com.gungnir.recomp"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.gungnir.recomp"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk {
            abiFilters += setOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments.addAll(listOf(
                    "-DGUNGNIR_ROOT=${repoRoot.absolutePath}",
                    "-DGUNGNIR_SDL3_DIR=${sdlSource.absolutePath}",
                    "-DGUNGNIR_ANDROID_O1_SOURCES=$o1Sources",
                    "-DANDROID_STL=c++_static",
                    "-DCMAKE_CXX_STANDARD=20"))
            }
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDir("${sdlSource}/android-project/app/src/main/java")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}
