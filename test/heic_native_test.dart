import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:heic_native/heic_native.dart';
import 'package:heic_native/heic_native_platform_interface.dart';
import 'package:heic_native/heic_native_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockHeicNativePlatform
    with MockPlatformInterfaceMixin
    implements HeicNativePlatform {
  int? lastCompressionLevel;
  bool? lastPreserveMetadata;

  @override
  Future<bool> convert(
    String inputPath,
    String outputPath, {
    int compressionLevel = 6,
    bool preserveMetadata = true,
  }) {
    lastCompressionLevel = compressionLevel;
    lastPreserveMetadata = preserveMetadata;
    return Future.value(true);
  }

  @override
  Future<Uint8List> convertToBytes(
    String inputPath, {
    int compressionLevel = 6,
    bool preserveMetadata = true,
  }) {
    lastCompressionLevel = compressionLevel;
    lastPreserveMetadata = preserveMetadata;
    return Future.value(Uint8List.fromList([1, 2, 3]));
  }
}

void main() {
  final HeicNativePlatform initialPlatform = HeicNativePlatform.instance;

  test('$MethodChannelHeicNative is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelHeicNative>());
  });

  group('with mock platform', () {
    late MockHeicNativePlatform fakePlatform;

    setUp(() {
      fakePlatform = MockHeicNativePlatform();
      HeicNativePlatform.instance = fakePlatform;
    });

    test('convert returns true', () async {
      expect(await HeicNative.convert('/input.heic', '/output.png'), true);
    });

    test('convertToBytes returns bytes', () async {
      final result = await HeicNative.convertToBytes('/input.heic');
      expect(result, equals(Uint8List.fromList([1, 2, 3])));
    });

    test('convert passes default options', () async {
      await HeicNative.convert('/input.heic', '/output.png');
      expect(fakePlatform.lastCompressionLevel, 6);
      expect(fakePlatform.lastPreserveMetadata, true);
    });

    test('convert passes custom compression level', () async {
      await HeicNative.convert(
        '/input.heic',
        '/output.png',
        compressionLevel: 0,
      );
      expect(fakePlatform.lastCompressionLevel, 0);
    });

    test('convert passes custom preserveMetadata', () async {
      await HeicNative.convert(
        '/input.heic',
        '/output.png',
        preserveMetadata: false,
      );
      expect(fakePlatform.lastPreserveMetadata, false);
    });

    test('convertToBytes passes default options', () async {
      await HeicNative.convertToBytes('/input.heic');
      expect(fakePlatform.lastCompressionLevel, 6);
      expect(fakePlatform.lastPreserveMetadata, true);
    });

    test('convertToBytes passes custom options', () async {
      await HeicNative.convertToBytes(
        '/input.heic',
        compressionLevel: 9,
        preserveMetadata: false,
      );
      expect(fakePlatform.lastCompressionLevel, 9);
      expect(fakePlatform.lastPreserveMetadata, false);
    });
  });
}
