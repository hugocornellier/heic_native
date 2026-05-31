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
* Configurable PNG compression level (0–9).
* Metadata preservation (ICC color profiles, EXIF data) enabled by default, with option to strip.
