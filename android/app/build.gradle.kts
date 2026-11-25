// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

// The Nakagawa Android app: the launcher (the player, libmain.so) and the runtime libraries of the
// titles bundled with it (libnk_title_<id>.so). docs/ANDROID_DEVELOPMENT.md has the commands.
//
// Properties (-P on the command line or ~/.gradle/gradle.properties; nothing machine-specific is
// kept in the repository):
//   nk.demos    a showcase demos folder (tests/fixtures/showcase/showcase.py build --target
//               android writes it: packages/<disc>/, images/<disc>.iso, data/), bundled whole as
//               demos/. The app unpacks it into its files folder, where the launcher's showcase
//               discovery finds and validates it as it does on the desktop, and Play starts each
//               package's own lib<game>.so. arm64-v8a only.
//   nk.titles   <title-id>=<package-dir>[;...]  packages built on a PC whose generated C is
//               compiled into libnk_title_<title-id>.so; each package's package.json and runtime
//               image are bundled as titles/<title-id>/{package.json,<title-id>_image.bin}, for
//               the debug route that starts TitleActivity from adb. <title-id> is the id the
//               package records (package.json title.id, the catalogue's, e.g. showcase-scene-v1).
//   Without either the app has the launcher only.
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
/** A package's package.json, or a refusal naming the property, the package and the reason. */
fun readPackage(property: String, dir: File): Map<*, *> {
    val file = File(dir, "package.json")
    require(file.isFile) { "$property: $dir holds no package.json; build the package there" }
    val parsed = try {
        groovy.json.JsonSlurper().parse(file)
    } catch (e: Exception) {
        throw IllegalArgumentException("$property: $file is not readable JSON: ${e.message}")
    }
    require(parsed is Map<*, *>) { "$property: $file is not a package description" }
    return parsed
}

/** The executable a package records (package.json executable.path). */
fun packageExecutable(property: String, dir: File, pkg: Map<*, *>): String =
    ((pkg["executable"] as? Map<*, *>)?.get("path") as? String)?.takeIf { it.isNotEmpty() }
        ?: throw IllegalArgumentException("$property: $dir/package.json records no executable.path")

/**
 * A title bundled by nk.titles: its catalogue id (the name of everything the APK holds for it),
 * the package folder, the package's runtime image, and the name its generated C files carry.
 */
data class BundledTitle(val id: String, val dir: File, val image: File, val codeName: String)

val titles: List<BundledTitle> = ((findProperty("nk.titles") as String?) ?: "")
    .split(";").map { it.trim() }.filter { it.isNotEmpty() }.map { entry ->
        val at = entry.indexOf('=')
        require(at > 0) { "nk.titles entry '$entry' is not <title-id>=<package-dir>" }
        val id = entry.substring(0, at)
        require(Regex("[A-Za-z0-9._-]+").matches(id)) { "nk.titles '$id' is not a title id" }
        val dir = File(entry.substring(at + 1))
        val pkg = readPackage("nk.titles '$id'", dir)
        val recorded = (pkg["title"] as? Map<*, *>)?.get("id") as? String
        require(recorded == id) {
            "nk.titles '$id': $dir holds the package of '$recorded'; name it by the title id its package.json records"
        }
        // Every package reader takes the runtime image beside the executable as
        // <executable stem>_image.bin: showcase-scene_image.bin for a host package's
        // showcase-scene(.exe), libshowcase-scene_image.bin for an Android package's
        // libshowcase-scene.so. The generated C is named for the game: showcase-scene_recomp*.c.
        val executable = File(packageExecutable("nk.titles '$id'", dir, pkg)).name
        val stem = executable.substringBeforeLast('.').takeIf { executable.contains('.') && it.isNotEmpty() }
            ?: executable
        val codeName = if (executable.endsWith(".so") && stem.startsWith("lib")) stem.removePrefix("lib") else stem
        val image = File(dir, "${stem}_image.bin")
        // stageBundledAssets copies the image; a Sync skips a missing source without an error,
        // so an APK would ship the title's library with no image to run it.
        require(image.isFile) { "nk.titles '$id': $dir holds no ${image.name}; build the title's package there" }
        BundledTitle(id, dir, image, codeName)
    }

/** The ELF machine of a library: 183 is AArch64, the only ABI an nk.demos package may target. */
fun elfMachine(file: File): Int? {
    if (!file.isFile) return null
    val header = ByteArray(20)
    file.inputStream().use { if (it.readNBytes(header, 0, header.size) != header.size) return null }
    if (header[0] != 0x7f.toByte() || header[1] != 'E'.code.toByte() ||
        header[2] != 'L'.code.toByte() || header[3] != 'F'.code.toByte()) return null
    return (header[18].toInt() and 0xff) or ((header[19].toInt() and 0xff) shl 8)
}

val demos: File? = (findProperty("nk.demos") as String?)?.let { File(it) }?.also { root ->
    require(abis == listOf("arm64-v8a")) {
        "nk.demos packages are built for arm64-v8a only; set -Pnk.abis=arm64-v8a"
    }
    val packages = File(root, "packages").listFiles()?.filter { it.isDirectory }.orEmpty()
    require(packages.isNotEmpty()) {
        "nk.demos '$root' holds no packages/<disc>; run showcase.py build --target android with that folder as NK_SHOWCASE_DEMO_ROOT"
    }
    for (dir in packages) {
        val property = "nk.demos package ${dir.name}"
        val executable = packageExecutable(property, dir, readPackage(property, dir))
        require(executable.startsWith("lib") && executable.endsWith(".so") && !executable.contains('/')) {
            "$property is built for a desktop ($executable), not Android; build it with showcase.py build --target android"
        }
        require(elfMachine(File(dir, executable)) == 183) {
            "$property: $executable is missing or not an arm64-v8a library"
        }
        require(File(root, "images/${dir.name}.iso").isFile) {
            "nk.demos '$root' holds no images/${dir.name}.iso for package ${dir.name}"
        }
    }
}
val nativeTargets: List<String> =
    listOf("nakagawa_player", "SDL3-shared", "SDL3_ttf-shared") + titles.map { "nk_title_${it.id}" }
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
    for (title in titles) {
        from(title.image) { into("app/titles/${title.id}"); rename { "${title.id}_image.bin" } }
        from(File(title.dir, "package.json")) { into("app/titles/${title.id}") }
    }
    // Every file as the package's completion-manifest.json lists it: the launcher validates each.
    demos?.let { root -> from(root) { into("app/demos") } }
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
                    "-DNK_ANDROID_TITLES=" + titles.joinToString(";") {
                        "${it.id}=${it.codeName}=${it.dir.invariantSeparatorsPath}"
                    },
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

    androidResources {
        // aapt's default pattern also leaves out every name starting with "." and every folder
        // starting with "_". A demo package lists such files (.game-inputs and the build's other
        // stamps) in its completion manifest, and the launcher refuses a package missing any of
        // them; only version-control and editor files stay out.
        ignoreAssetsPattern = "!.svn:!.git:!.ds_store:!*.scc:!CVS:!thumbs.db:!picasa.ini:!*~"
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
