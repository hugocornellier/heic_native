## 1.2.0

* Android: no longer applies the Kotlin Gradle Plugin (KGP) itself, following
  Flutter's [built-in Kotlin migration for plugin authors](https://docs.flutter.dev/release/breaking-changes/migrate-to-built-in-kotlin/for-plugin-authors).
  Flutter 3.44 and newer compile the plugin's Kotlin themselves (with built-in
  Kotlin, or by applying KGP to the plugin when built-in Kotlin is off), so
  Android builds on Flutter 3.50 no longer warn "Your app uses the following
  plugins that apply Kotlin Gradle Plugin (KGP): heic_native". The plugin's Java
  and Kotlin targets are now 17.
* Requires Flutter 3.44 or newer (Dart 3.12), because the migration needs
  KGP 2.0 or newer, which Flutter enforces from 3.44.
* Example app: Android moved to Gradle 9.5.0, AGP 9.3.1 and Kotlin 2.4.20 (the
  Flutter 3.50 template), and its Material widgets now come from
  `package:material_ui`.
* Linux: the native unit tests built with the example fetch googletest 1.16
  instead of 1.11, which CMake 4 no longer configures.

## 1.1.0

* Complete the AGP 9 fix from 1.0.1. The previous guard applied `kotlin-android`
  only on AGP < 9, which left AGP 9 builds with no Kotlin compiler at all when the
  consumer disables built-in Kotlin via `android.builtInKotlin=false` (the flag the
  Flutter AGP-9 migrator adds to apps). With neither built-in Kotlin nor KGP active,
  the plugin's Kotlin class was never compiled and consumer apps failed at
  GeneratedPluginRegistrant with "cannot find symbol class HeicNativePlugin". The
  Android build script now also applies KGP when built-in Kotlin is disabled (the
  same logic flutter_litert uses), so there is always a Kotlin compiler. Verified
  with a clean `flutter build apk` under AGP 9.2.1 with `builtInKotlin=false`. The
  example app's Kotlin Gradle Plugin application was made conditional the same way.

## 1.0.1

* Fix Android builds under AGP 9 / Flutter 3.44+ with built-in Kotlin. The
  plugin previously applied the legacy Kotlin Gradle Plugin unconditionally,
  which AGP 9 rejects ("The 'org.jetbrains.kotlin.android' plugin is no longer
  required since AGP 9.0"). The Android build script now applies `kotlin-android`
  only on AGP < 9 and lets built-in Kotlin compile the sources on AGP >= 9, with
  the Kotlin JVM target pinned to 8 (unchanged). Verified building under AGP 9.0.1
  with built-in Kotlin enabled. No API or minimum-version changes.

## 1.0.0

* Raise minimum deployment targets to iOS 13.0 / macOS 10.15 to satisfy Swift Package Manager's `FlutterFramework` requirement (fixes SPM build failures).
* Fix Swift Package Manager product names for iOS and macOS so Flutter can resolve the `heic-native` plugin product.

## 0.1.1

* Update documentation

## 0.1.0

* Initial release of `heic_native`, a Flutter plugin that converts HEIC/HEIF images to PNG using native platform APIs.
* Supports all 5 platforms:
  - **iOS** (CoreGraphics/ImageIO, iOS 12.0+)
  - **Android** (libheif + libpng via NDK, API 28+)
  - **macOS** (CoreGraphics/ImageIO, macOS 10.13+)
  - **Windows** (libheif + libpng, Windows 10 1809+)
  - **Linux** (libheif + libpng, requires libheif-dev and libpng-dev)
* Configurable PNG compression level (0-9).
* Metadata preservation (ICC color profiles, EXIF data) enabled by default, with option to strip.
