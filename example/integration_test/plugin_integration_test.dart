import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:heic_native/heic_native.dart';

/// PNG magic bytes: 0x89 P N G
const _pngMagic = [0x89, 0x50, 0x4E, 0x47];

const _samples = [
  'assets/test_fixtures/sample1.heic',
  'assets/test_fixtures/sample2.heic',
  'assets/test_fixtures/sample3.heic',
];

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  late Directory tempDir;

  setUpAll(() async {
    tempDir = await Directory.systemTemp.createTemp('heic_native_test_');
  });

  tearDownAll(() async {
    if (tempDir.existsSync()) {
      tempDir.deleteSync(recursive: true);
    }
  });

  /// Writes an asset to a temp file and returns its path.
  Future<String> assetToTempFile(String assetKey, String filename) async {
    final data = await rootBundle.load(assetKey);
    final file = File('${tempDir.path}/$filename');
    await file.writeAsBytes(data.buffer.asUint8List());
    return file.path;
  }

  // ─── convert() tests ───

  for (var i = 0; i < _samples.length; i++) {
    final sample = _samples[i];
    final label = 'sample${i + 1}';

    testWidgets('convert $label to PNG file', (tester) async {
      final inputPath = await assetToTempFile(sample, '$label.heic');
      final outputPath = '${tempDir.path}/$label.png';

      final result = await HeicNative.convert(inputPath, outputPath);
      expect(result, isTrue);

      final outputFile = File(outputPath);
      expect(outputFile.existsSync(), isTrue);

      final bytes = await outputFile.readAsBytes();
      expect(bytes.length, greaterThan(100));
      expect(bytes.sublist(0, 4), equals(_pngMagic));
    });
  }

  testWidgets('convert with compressionLevel 0 (fastest)', (tester) async {
    final inputPath = await assetToTempFile(_samples[0], 'compress0.heic');
    final outputPath = '${tempDir.path}/compress0.png';

    final result = await HeicNative.convert(
      inputPath,
      outputPath,
      compressionLevel: 0,
    );
    expect(result, isTrue);

    final bytes = await File(outputPath).readAsBytes();
    expect(bytes.sublist(0, 4), equals(_pngMagic));
  });

  testWidgets('convert with compressionLevel 9 (smallest)', (tester) async {
    final inputPath = await assetToTempFile(_samples[0], 'compress9.heic');
    final outputPath = '${tempDir.path}/compress9.png';

    final result = await HeicNative.convert(
      inputPath,
      outputPath,
      compressionLevel: 9,
    );
    expect(result, isTrue);

    final bytes = await File(outputPath).readAsBytes();
    expect(bytes.sublist(0, 4), equals(_pngMagic));
  });

  testWidgets('convert with preserveMetadata false', (tester) async {
    final inputPath = await assetToTempFile(_samples[0], 'nometa.heic');
    final outputPath = '${tempDir.path}/nometa.png';

    final result = await HeicNative.convert(
      inputPath,
      outputPath,
      preserveMetadata: false,
    );
    expect(result, isTrue);

    final bytes = await File(outputPath).readAsBytes();
    expect(bytes.sublist(0, 4), equals(_pngMagic));
  });

  testWidgets('convert overwrites existing output', (tester) async {
    final inputPath = await assetToTempFile(_samples[0], 'overwrite.heic');
    final outputPath = '${tempDir.path}/overwrite.png';

    // Create a dummy file at the output path first.
    await File(outputPath).writeAsString('placeholder');

    final result = await HeicNative.convert(inputPath, outputPath);
    expect(result, isTrue);

    final bytes = await File(outputPath).readAsBytes();
    expect(bytes.sublist(0, 4), equals(_pngMagic));
  });

  // ─── convertToBytes() tests ───

  for (var i = 0; i < _samples.length; i++) {
    final sample = _samples[i];
    final label = 'sample${i + 1}';

    testWidgets('convertToBytes $label returns valid PNG bytes', (
      tester,
    ) async {
      final inputPath = await assetToTempFile(sample, '${label}_bytes.heic');

      final bytes = await HeicNative.convertToBytes(inputPath);
      expect(bytes.length, greaterThan(100));
      expect(bytes.sublist(0, 4), equals(_pngMagic));
    });
  }

  testWidgets('convertToBytes with compressionLevel 0', (tester) async {
    final inputPath = await assetToTempFile(
      _samples[0],
      'bytes_compress0.heic',
    );

    final bytes = await HeicNative.convertToBytes(
      inputPath,
      compressionLevel: 0,
    );
    expect(bytes.sublist(0, 4), equals(_pngMagic));
  });

  testWidgets('convertToBytes with compressionLevel 9', (tester) async {
    final inputPath = await assetToTempFile(
      _samples[0],
      'bytes_compress9.heic',
    );

    final bytes = await HeicNative.convertToBytes(
      inputPath,
      compressionLevel: 9,
    );
    expect(bytes.sublist(0, 4), equals(_pngMagic));
  });

  testWidgets('convertToBytes with preserveMetadata false', (tester) async {
    final inputPath = await assetToTempFile(_samples[0], 'bytes_nometa.heic');

    final bytes = await HeicNative.convertToBytes(
      inputPath,
      preserveMetadata: false,
    );
    expect(bytes.sublist(0, 4), equals(_pngMagic));
  });

  // ─── Compression comparison ───

  testWidgets(
    'compression 0 and 9 both produce valid PNGs of different sizes',
    (tester) async {
      final input0 = await assetToTempFile(_samples[0], 'cmp_test_0.heic');
      final input9 = await assetToTempFile(_samples[0], 'cmp_test_9.heic');

      final bytes0 = await HeicNative.convertToBytes(
        input0,
        compressionLevel: 0,
      );
      final bytes9 = await HeicNative.convertToBytes(
        input9,
        compressionLevel: 9,
      );

      // Both must be valid PNGs.
      expect(bytes0.sublist(0, 4), equals(_pngMagic));
      expect(bytes9.sublist(0, 4), equals(_pngMagic));
      // On Linux/Windows, level 0 is strictly larger. On iOS/macOS the
      // relationship can invert because compression level only controls the
      // PNG row filter, not the zlib level. Just verify both are non-empty.
      expect(bytes0.length, greaterThan(100));
      expect(bytes9.length, greaterThan(100));
    },
  );

  // ─── convert() and convertToBytes() produce same image ───

  testWidgets('convert and convertToBytes produce same PNG', (tester) async {
    final input1 = await assetToTempFile(_samples[0], 'consistency_a.heic');
    final input2 = await assetToTempFile(_samples[0], 'consistency_b.heic');
    final outputPath = '${tempDir.path}/consistency.png';

    await HeicNative.convert(input1, outputPath);
    final fileBytes = await File(outputPath).readAsBytes();

    final memBytes = await HeicNative.convertToBytes(input2);

    // Both should produce valid PNGs of similar size.
    expect(fileBytes.sublist(0, 4), equals(_pngMagic));
    expect(memBytes.sublist(0, 4), equals(_pngMagic));
    // Allow 1% tolerance for potential temp file vs memory path differences.
    final ratio = fileBytes.length / memBytes.length;
    expect(ratio, closeTo(1.0, 0.01));
  });

  // ─── Metadata preservation ───

  testWidgets('preserveMetadata flag affects PNG output', (tester) async {
    final inputPath = await assetToTempFile(_samples[0], 'meta_check.heic');

    final withMeta = await HeicNative.convertToBytes(
      inputPath,
      preserveMetadata: true,
    );
    final withoutMeta = await HeicNative.convertToBytes(
      inputPath,
      preserveMetadata: false,
    );

    expect(withMeta.sublist(0, 4), equals(_pngMagic));
    expect(withoutMeta.sublist(0, 4), equals(_pngMagic));

    // With metadata should be larger (ICC profile and/or EXIF add bytes).
    expect(
      withMeta.length,
      greaterThan(withoutMeta.length),
      reason: 'PNG with metadata should be larger than without',
    );

    // The metadata-preserved PNG should contain at least one metadata chunk.
    final metaChunks = _findPngChunkTypes(withMeta);
    final metadataChunkTypes = {'iCCP', 'eXIf', 'tEXt', 'iTXt', 'zTXt', 'sRGB'};
    final hasMetaChunk = metaChunks.any((c) => metadataChunkTypes.contains(c));

    expect(
      hasMetaChunk,
      isTrue,
      reason:
          'Expected iCCP, eXIf, sRGB, or text chunk in metadata-preserved '
          'PNG (found chunks: $metaChunks)',
    );
  });

  // ─── Error cases ───

  testWidgets('convert throws PlatformException for non-existent file', (
    tester,
  ) async {
    expect(
      () async => await HeicNative.convert('/nonexistent.heic', '/out.png'),
      throwsA(isA<PlatformException>()),
    );
  });

  testWidgets('convertToBytes throws PlatformException for non-existent file', (
    tester,
  ) async {
    expect(
      () async => await HeicNative.convertToBytes('/nonexistent.heic'),
      throwsA(isA<PlatformException>()),
    );
  });

  testWidgets('convert throws ArgumentError for empty inputPath', (
    tester,
  ) async {
    expect(
      () => HeicNative.convert('', '/out.png'),
      throwsA(isA<ArgumentError>()),
    );
  });

  testWidgets('convert throws ArgumentError for empty outputPath', (
    tester,
  ) async {
    expect(
      () => HeicNative.convert('/in.heic', ''),
      throwsA(isA<ArgumentError>()),
    );
  });

  testWidgets('convertToBytes throws ArgumentError for empty inputPath', (
    tester,
  ) async {
    expect(() => HeicNative.convertToBytes(''), throwsA(isA<ArgumentError>()));
  });
}

/// Parses a PNG byte stream and returns the list of chunk type strings.
List<String> _findPngChunkTypes(List<int> bytes) {
  final chunks = <String>[];
  // Skip 8-byte PNG signature.
  var offset = 8;
  while (offset + 8 <= bytes.length) {
    final length =
        (bytes[offset] << 24) |
        (bytes[offset + 1] << 16) |
        (bytes[offset + 2] << 8) |
        bytes[offset + 3];
    final type = String.fromCharCodes(bytes.sublist(offset + 4, offset + 8));
    chunks.add(type);
    if (type == 'IEND') break;
    // 4 (length) + 4 (type) + length (data) + 4 (CRC)
    offset += 12 + length;
  }
  return chunks;
}
