import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'heic_native_platform_interface.dart';

/// Implementation of [HeicNativePlatform] that uses a [MethodChannel].
class MethodChannelHeicNative extends HeicNativePlatform {
  /// The method channel used to communicate with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('heic_native');

  /// Converts a HEIC/HEIF file at [inputPath] to PNG at [outputPath].
  @override
  Future<bool> convert(
    String inputPath,
    String outputPath, {
    int compressionLevel = 6,
    bool preserveMetadata = true,
  }) async {
    final result = await methodChannel.invokeMethod<bool>('convert', {
      'inputPath': inputPath,
      'outputPath': outputPath,
      'compressionLevel': compressionLevel.clamp(0, 9),
      'preserveMetadata': preserveMetadata,
    });
    if (result == null) {
      throw PlatformException(
        code: 'conversion_failed',
        message: 'Platform returned null result',
      );
    }
    return result;
  }

  /// Converts a HEIC/HEIF file at [inputPath] to PNG bytes in memory.
  @override
  Future<Uint8List> convertToBytes(
    String inputPath, {
    int compressionLevel = 6,
    bool preserveMetadata = true,
  }) async {
    final result = await methodChannel
        .invokeMethod<Uint8List>('convertToBytes', {
          'inputPath': inputPath,
          'compressionLevel': compressionLevel.clamp(0, 9),
          'preserveMetadata': preserveMetadata,
        });
    if (result == null) {
      throw PlatformException(
        code: 'conversion_failed',
        message: 'Platform returned null result',
      );
    }
    return result;
  }
}
