import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:heic_native/heic_native_method_channel.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  MethodChannelHeicNative platform = MethodChannelHeicNative();
  const MethodChannel channel = MethodChannel('heic_native');

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  test('convert returns true', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
          if (methodCall.method == 'convert') {
            expect(methodCall.arguments['inputPath'], '/input.heic');
            expect(methodCall.arguments['outputPath'], '/output.png');
            return true;
          }
          return null;
        });

    expect(await platform.convert('/input.heic', '/output.png'), true);
  });

  test('convert passes compression level and metadata flag', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
          if (methodCall.method == 'convert') {
            expect(methodCall.arguments['compressionLevel'], 9);
            expect(methodCall.arguments['preserveMetadata'], false);
            return true;
          }
          return null;
        });

    expect(
      await platform.convert(
        '/input.heic',
        '/output.png',
        compressionLevel: 9,
        preserveMetadata: false,
      ),
      true,
    );
  });

  test('convert clamps compression level', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
          if (methodCall.method == 'convert') {
            expect(methodCall.arguments['compressionLevel'], 9);
            return true;
          }
          return null;
        });

    expect(
      await platform.convert(
        '/input.heic',
        '/output.png',
        compressionLevel: 42,
      ),
      true,
    );
  });

  test('convertToBytes returns bytes', () async {
    final expectedBytes = Uint8List.fromList([1, 2, 3]);

    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
          if (methodCall.method == 'convertToBytes') {
            expect(methodCall.arguments['inputPath'], '/input.heic');
            return expectedBytes;
          }
          return null;
        });

    expect(await platform.convertToBytes('/input.heic'), equals(expectedBytes));
  });

  test('convertToBytes passes options', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
          if (methodCall.method == 'convertToBytes') {
            expect(methodCall.arguments['compressionLevel'], 0);
            expect(methodCall.arguments['preserveMetadata'], false);
            return Uint8List.fromList([1]);
          }
          return null;
        });

    expect(
      await platform.convertToBytes(
        '/input.heic',
        compressionLevel: 0,
        preserveMetadata: false,
      ),
      isNotNull,
    );
  });

  test('convertToBytes throws on null result', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
          return null;
        });

    expect(
      () => platform.convertToBytes('/input.heic'),
      throwsA(isA<PlatformException>()),
    );
  });
}
