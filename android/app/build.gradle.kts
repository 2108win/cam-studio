plugins {
    id("com.android.application")
}

android {
    namespace = "vn.camstudio.phone"
    compileSdk = 36

    defaultConfig {
        applicationId = "vn.camstudio.phone"
        minSdk = 26
        targetSdk = 36
        versionCode = 4
        versionName = "0.1.2"
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
