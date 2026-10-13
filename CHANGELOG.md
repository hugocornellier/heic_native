## 1.1.1

* Linux: call `heif_init()` when the plugin registers, as the Windows plugin
  already did. libheif asks for it before any other call so that decoder
  plugins, which is how Debian and Ubuntu package libheif, are registered;
  current libheif releases also load them lazily, so this is hardening rather
  than a fix for a known failure. No `heif_deinit()` is called, for the same
  teardown reason as on Windows.
* Linux: `decode_failed` errors now say which libheif step failed, libheif's own
  message, and the libheif version that was loaded, instead of a bare
  "Failed to decode HEIC file".

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
