// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

// The Nakagawa Android app: the launcher (the player, libmain.so) and the runtime libraries of the
// titles bundled with it (libnk_title_<id>.so). docs/ANDROID_DEVELOPMENT.md has the commands.
//
// Properties (-P on the command line or ~/.gradle/gradle.properties; nothing machine-specific is
// kept in the repository):
//   nk.titles   <title-id>=<package-dir>[;...]  packages built on a PC whose generated C is
//               compiled into libnk_title_<id>.so; each package's <id>_image.bin and package.json
//               are bundled too. Without it the app has the launcher only.
//   nk.abis     comma-separated ABIs, default arm64-v8a,x86_64.
//   nk.depsDir  where the pinned SDL3 and SDL3_ttf sources live, default <repo>/build/android-deps.
//   nk.toolchain the --out folder of tools/android_toolchain.py: the on-device clang, lld, make and
//               Python, their sysroot, standard library and notices (android/toolchain/PROVENANCE.md),
//               bundled for arm64-v8a. Without it the app carries no toolchain.

import java.io.File

plugins {
    id("com.android.application")
}

val repoRoot: File = rootDir.parentFile
val depsDir: String = File((findProperty("nk.depsDir") as String?) ?: File(repoRoot, "build/android-deps").path)
    .invariantSeparatorsPath
val abis: List<String> = ((findProperty("nk.abis") as String?) ?: "arm64-v8a,x86_64")
    .split(",").map { it.trim() }.filter { it.isNotEmpty() }
val titles: List<Pair<String, File>> = ((findProperty("nk.titles") as String?) ?: "")
    .split(";").map { it.trim() }.filter { it.isNotEmpty() }.map { entry ->
        val at = entry.indexOf('=')
        require(at > 0) { "nk.titles entry '$entry' is not <title-id>=<package-dir>" }
        entry.substring(0, at) to File(entry.substring(at + 1))
    }
val nativeTargets: List<String> =
    listOf("nakagawa_player", "SDL3-shared", "SDL3_ttf-shared") + titles.map { "nk_title_${it.first}" }
val bundledAssets: File = layout.buildDirectory.dir("generated/nk-assets").get().asFile
val toolchain: File? = (findProperty("nk.toolchain") as String?)?.let { File(it) }?.also { dir ->
    require(File(dir, "toolchain-manifest.json").isFile) {
        "nk.toolchain '$dir' holds no toolchain-manifest.json; run tools/android_toolchain.py --out there"
    }
    require(abis == listOf("arm64-v8a")) { "nk.toolchain is built for arm64-v8a only; set -Pnk.abis=arm64-v8a" }
}

// SDL's android-project Java classes are compiled from the pinned checkout, so it must exist
// before Java compiles; the native build reuses the same checkout.
val fetchNativeDeps = tasks.register<Exec>("fetchNativeDeps") {
    description = "Fetches the pinned SDL3 and SDL3_ttf sources"
    commandLine("cmake", "-DNK_ANDROID_DEPS_DIR=$depsDir", "-P", File(projectDir, "deps.cmake").path)
}

// The files the app unpacks on first start (LauncherActivity): the UI fonts, the launcher music's
// track (#2207), the bundled Redump PSP list the disc check reads, and for each bundled title its
// flat image and package description.
val stageBundledAssets = tasks.register<Sync>("stageBundledAssets") {
    into(bundledAssets)
    from(File(repoRoot, "assets/fonts/ui")) { into("app/assets/fonts/ui") }
    from(File(repoRoot, "assets/music/launcher/ChillLofi.ogg")) { into("app/assets/music/launcher") }
    from(File(repoRoot, "assets/redump")) { into("app/assets/redump") }
    for ((id, dir) in titles) {
        from(File(dir, "${id}_image.bin")) { into("app/titles/$id") }
        from(File(dir, "package.json")) { into("app/titles/$id") }
    }
}

android {
    namespace = "io.github.jstar269.nakagawa"
    compileSdk {
        version = release(37) { minorApiLevel = 2 }
    }
    ndkVersion = "30.0.16248370"
    buildToolsVersion = "37.0.0"

    defaultConfig {
        applicationId = "io.github.jstar269.nakagawa"
        // API 29: ELF TLS for the runtime's _Thread_local state, Vulkan 1.1 on 64-bit devices,
        // scoped storage. 32-bit ABIs are out of scope (#360).
        minSdk = 29
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0-dev"
        ndk {
            abiFilters += abis
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DNK_ANDROID_DEPS_DIR=$depsDir",
                    "-DNK_ANDROID_TITLES=" + titles.joinToString(";") { "${it.first}=${it.second.invariantSeparatorsPath}" },
                    // SDL3's HID layer and HarfBuzz are C++; each library links the static
                    // runtime, and no C++ type crosses a library boundary.
                    "-DANDROID_STL=c++_static",
                )
                targets += nativeTargets
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "4.1.2"
        }
    }

    sourceSets {
        getByName("main") {
            java.directories.add("$depsDir/SDL/android-project/app/src/main/java")
            assets.directories.add(bundledAssets.path)
            if (toolchain != null) {
                // jniLibs/arm64-v8a/libnk_*.so and the CPython libraries; assets/toolchain/.
                jniLibs.directories.add(File(toolchain, "jniLibs").path)
                assets.directories.add(File(toolchain, "assets").path)
            }
        }
    }

    packaging {
        jniLibs {
            // Extracted to the native library folder, so the launcher can name a title's library
            // by path (as the desktop names an executable), and so the toolchain's programs
            // (libnk_clang.so and the rest) can be executed from there.
            useLegacyPackaging = true
            // The toolchain's libraries ship as staged: its own are linked stripped, with a build
            // ID that names those bytes, and CPython's are the release's. The NDK strip would
            // rewrite them.
            toolchain?.let { dir ->
                File(dir, "jniLibs/arm64-v8a").listFiles()?.forEach { keepDebugSymbols += "**/${it.name}" }
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
        }
    }
}

tasks.named("preBuild") {
    dependsOn(fetchNativeDeps, stageBundledAssets)
}
