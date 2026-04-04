<h1 align="center">heic_native</h1>

<p align="center">
<a href="https://flutter.dev"><img src="https://img.shields.io/badge/Platform-Flutter-02569B?logo=flutter" alt="Platform"></a>
<a href="https://dart.dev"><img src="https://img.shields.io/badge/language-Dart-blue" alt="Language: Dart"></a>
<br>
<a href="https://pub.dev/packages/heic_native"><img src="https://img.shields.io/pub/v/heic_native?label=pub.dev&labelColor=333940&logo=dart" alt="Pub Version"></a>
<a href="https://pub.dev/packages/heic_native/score"><img src="https://img.shields.io/pub/points/heic_native?color=2E8B57&label=pub%20points" alt="pub points"></a>
<a href="https://github.com/hugocornellier/heic_native/actions/workflows/ci.yml"><img src="https://github.com/hugocornellier/heic_native/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
<a href="https://github.com/hugocornellier/heic_native/blob/main/LICENSE"><img src="https://img.shields.io/badge/License-MIT-007A88.svg" alt="License"></a>
</p>

A Flutter plugin that converts HEIC/HEIF images to PNG using native platform APIs. 

## Features

- Lossless HEIC/HEIF to PNG conversion
- Convert to file or to in-memory bytes
- Configurable PNG compression level (0-9)
- Metadata preservation (ICC color profiles, EXIF data)
- Cross-platform: iOS, Android, macOS, Windows, Linux
- Uses native APIs on each platform for maximum performance:
  - **iOS / macOS**: CoreGraphics / ImageIO
  - **Android**: BitmapFactory + ExifInterface
  - **Windows**: libheif + libpng
  - **Linux**: libheif + libpng

## Installation

```yaml
dependencies:
  heic_native: ^0.1.1
```

### iOS

No additional setup needed (iOS 12.0+).

### Android

Requires Android 9 (API 28) or later (native HEIF decoder support).

Set `minSdk = 28` in your app's `android/app/build.gradle.kts`:

```kotlin
android {
    defaultConfig {
        minSdk = 28
    }
}
```

### macOS

No additional setup needed (macOS 10.13+).

### Windows

Requires Windows 10 version 1809 or later. The HEIF Image Extensions from the
Microsoft Store may be required on Windows 10; Windows 11 bundles the codec.

### Linux

Install the required native libraries:

```bash
sudo apt install libheif-dev libpng-dev
```

## Usage

### Convert to a file

```dart
import 'package:heic_native/heic_native.dart';

final success = await HeicNative.convert('/path/to/input.heic', '/path/to/output.png');
if (success) {
  print('Converted successfully!');
}
```

### Convert to bytes

```dart
final bytes = await HeicNative.convertToBytes('/path/to/input.heic');
if (bytes != null) {
  // Use the PNG bytes (e.g. display with Image.memory)
}
```

### Compression level

Control how much effort is spent compressing the output PNG. `0` is fastest (largest file), `9` is slowest (smallest file). Default is `6`.

```dart
// Fastest conversion, larger file
await HeicNative.convert(input, output, compressionLevel: 0);

// Maximum compression, slower
await HeicNative.convert(input, output, compressionLevel: 9);

// Also works with convertToBytes
final bytes = await HeicNative.convertToBytes(input, compressionLevel: 9);
```

### Metadata preservation

By default, ICC color profiles and EXIF data (camera info, GPS, orientation, date/time) are preserved from the source HEIC file. Set `preserveMetadata: false` to strip all metadata:

```dart
// Strip all metadata (ICC profile, EXIF, GPS, etc.)
await HeicNative.convert(input, output, preserveMetadata: false);

// Combine options
final bytes = await HeicNative.convertToBytes(
  input,
  compressionLevel: 0,
  preserveMetadata: false,
);
```

## API

| Method | Description |
|--------|-------------|
| `HeicNative.convert(inputPath, outputPath, {compressionLevel, preserveMetadata})` | Converts a HEIC file to PNG and writes it to `outputPath`. Returns `true` on success. |
| `HeicNative.convertToBytes(inputPath, {compressionLevel, preserveMetadata})` | Converts a HEIC file to PNG and returns the bytes as `Uint8List`. Throws `PlatformException` on failure. |

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `inputPath` | `String` | required | Path to the source HEIC/HEIF file. |
| `outputPath` | `String` | required | Path for the output PNG file (convert only). |
| `compressionLevel` | `int` | `6` | PNG compression effort, 0 (fastest) to 9 (smallest). |
| `preserveMetadata` | `bool` | `true` | Preserve ICC color profiles and EXIF data from the source. |

## Platform notes

### Compression level

| Platform | Behavior |
|----------|----------|
| **Linux / Windows** | Maps directly to zlib compression level (0-9). Full granularity. |
| **iOS / macOS** | Controls PNG row filter strategy: `0` = no filter (fastest), `1`-`9` = adaptive filter (best compression). Levels 1-9 produce the same output. |
| **Android** | Accepted but has no effect. Android's `Bitmap.compress(PNG)` uses a fixed internal compression level. |

### Metadata preservation

| Metadata | iOS | Android | macOS | Windows | Linux |
|----------|-----|---------|-------|---------|-------|
| ICC color profiles | Yes | No | Yes | Yes | Yes |
| EXIF data | Yes | Yes | Yes | Yes (libpng >= 1.6.32) | Yes (libpng >= 1.6.32) |

- **iOS / macOS** preserve all metadata (EXIF, ICC, GPS, TIFF tags, XMP) automatically via `CGImageDestinationAddImageFromSource`.
- **Android** transfers EXIF tags (camera info, GPS, orientation, date/time) via AndroidX `ExifInterface`. ICC color profile transfer is not supported. Colors are converted to sRGB during encoding.
- **Windows / Linux** preserve ICC profiles via `png_set_iCCP` and EXIF via `png_set_eXIf_1` (requires libpng 1.6.32+). On older libpng versions, EXIF is silently skipped.

## Example

See the [example app](https://pub.dev/packages/heic_native/example) for a complete working demo.

## License

MIT. See [LICENSE](LICENSE) for details.
