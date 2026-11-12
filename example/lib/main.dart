import 'dart:typed_data';

import 'package:material_ui/material_ui.dart';
import 'package:heic_native/heic_native.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  String _status = 'Idle';
  Uint8List? _pngBytes;

  Future<void> _convertToFile() async {
    setState(() => _status = 'Converting to file...');
    final success = await HeicNative.convert(
      '/path/to/input.heic',
      '/path/to/output.png',
      compressionLevel: 6, // 0 = fastest, 9 = smallest
      preserveMetadata: true, // keep ICC profiles & EXIF
    );
    setState(
      () => _status = success ? 'Saved to output.png' : 'Conversion failed',
    );
  }

  Future<void> _convertToBytes() async {
    setState(() => _status = 'Converting to bytes...');
    final bytes = await HeicNative.convertToBytes(
      '/path/to/input.heic',
      compressionLevel: 6,
      preserveMetadata: true,
    );
    setState(() {
      _pngBytes = bytes;
      _status = 'Got ${bytes.length} bytes';
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('heic_native example')),
        body: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text('Status: $_status'),
              const SizedBox(height: 16),
              ElevatedButton(
                onPressed: _convertToFile,
                child: const Text('Convert to file'),
              ),
              const SizedBox(height: 8),
              ElevatedButton(
                onPressed: _convertToBytes,
                child: const Text('Convert to bytes'),
              ),
              const SizedBox(height: 16),
              if (_pngBytes != null) Expanded(child: Image.memory(_pngBytes!)),
            ],
          ),
        ),
      ),
    );
  }
}
