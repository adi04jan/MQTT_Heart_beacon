// :core - pure Kotlin/JVM protocol codec (envelope + BLE frames). No Android dependencies.
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    `java-library`
    alias(libs.plugins.kotlin.jvm)
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions.jvmTarget.set(JvmTarget.JVM_17)
}

// The shared golden vectors (../protocol/vectors.json) go on the test classpath.
sourceSets["test"].resources.srcDir(rootProject.file("../protocol"))

dependencies {
    api(libs.kotlinx.serialization.json)
    testImplementation(libs.junit4)
}
