import Flutter
import UIKit
import CoreGraphics
import ImageIO

public class HeicNativePlugin: NSObject, FlutterPlugin {
  private let workerQueue = DispatchQueue(label: "com.hugocornellier.heic_native.worker", qos: .userInitiated)

  public static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(name: "heic_native", binaryMessenger: registrar.messenger())
    let instance = HeicNativePlugin()
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "convert":
      guard
        let args = call.arguments as? [String: Any],
        let inputPath = args["inputPath"] as? String,
        let outputPath = args["outputPath"] as? String
      else {
        result(FlutterError(code: "invalid_arguments", message: "inputPath and outputPath are required", details: nil))
        return
      }
      let compressionLevel = min(9, max(0, args["compressionLevel"] as? Int ?? 6))
      let preserveMetadata = args["preserveMetadata"] as? Bool ?? true
      workerQueue.async {
        let error = self.convertHeicToPng(inputPath: inputPath, outputPath: outputPath, compressionLevel: compressionLevel, preserveMetadata: preserveMetadata)
        DispatchQueue.main.async {
          if let error = error {
            result(error)
          } else {
            result(true)
          }
        }
      }

    case "convertToBytes":
      guard
        let args = call.arguments as? [String: Any],
        let inputPath = args["inputPath"] as? String
      else {
        result(FlutterError(code: "invalid_arguments", message: "inputPath is required", details: nil))
        return
      }
      let compressionLevel = min(9, max(0, args["compressionLevel"] as? Int ?? 6))
      let preserveMetadata = args["preserveMetadata"] as? Bool ?? true
      workerQueue.async {
        let (data, error) = self.convertHeicToBytes(inputPath: inputPath, compressionLevel: compressionLevel, preserveMetadata: preserveMetadata)
        DispatchQueue.main.async {
          if let error = error {
            result(error)
          } else {
            result(FlutterStandardTypedData(bytes: data!))
          }
        }
      }

    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func buildProperties(compressionLevel: Int) -> CFDictionary {
    var pngProperties: [CFString: Any] = [:]
    // 0 = PNG_FILTER_NONE, 5 = PNG_FILTER_ADAPTIVE (all filters enabled)
    if compressionLevel == 0 {
      pngProperties[kCGImagePropertyPNGCompressionFilter] = 0
    } else {
      pngProperties[kCGImagePropertyPNGCompressionFilter] = 5
    }
    return [kCGImagePropertyPNGDictionary: pngProperties] as CFDictionary
  }

  /// Returns a CGImage whose pixels are rotated to match EXIF orientation,
  /// decoded from `source` at index `idx`. Returns nil on failure.
  private func orientedCGImage(from source: CGImageSource, at idx: Int) -> CGImage? {
    // kCGImageSourceCreateThumbnailWithTransform applies the EXIF rotation.
    // kCGImageSourceShouldCacheImmediately forces decode now so we don't hold
    // a stale source reference.
    // kCGImageSourceCreateThumbnailFromImageAlways decodes the full image
    // (not a pre-built thumbnail sub-resource).
    let options: [CFString: Any] = [
      kCGImageSourceCreateThumbnailWithTransform: true,
      kCGImageSourceCreateThumbnailFromImageAlways: true,
      kCGImageSourceShouldCacheImmediately: true,
    ]
    return CGImageSourceCreateThumbnailAtIndex(source, idx, options as CFDictionary)
  }

  /// Returns a CFDictionary of image properties from `source` at `idx`,
  /// with the EXIF orientation (kCGImagePropertyOrientation) forced to 1 (Normal).
  private func normalizedProperties(
    from source: CGImageSource,
    at idx: Int,
    baseProperties: CFDictionary,
    rotatedImage: CGImage
  ) -> CFDictionary {
    // Start with the base PNG encoding properties (compression filter etc.)
    var props = (baseProperties as? [CFString: Any]) ?? [:]

    // Copy source image properties into the dictionary.
    if let srcProps = CGImageSourceCopyPropertiesAtIndex(source, idx, nil)
                       as? [CFString: Any] {
      for (k, v) in srcProps {
        // Skip the top-level orientation, PNG does not use it;
        // orientation is expressed inside the EXIF sub-dictionary.
        if k == kCGImagePropertyOrientation { continue }
        if k == kCGImagePropertyPNGDictionary { continue }
        props[k] = v
      }
      // TIFF sub-dictionary also carries orientation in some files.
      if var tiffDict = srcProps[kCGImagePropertyTIFFDictionary] as? [CFString: Any] {
        tiffDict[kCGImagePropertyTIFFOrientation] = 1
        props[kCGImagePropertyTIFFDictionary] = tiffDict
      }
      // EXIF sub-dictionary carries pixel dimensions.
      if var exifDict = srcProps[kCGImagePropertyExifDictionary] as? [CFString: Any] {
        exifDict[kCGImagePropertyExifPixelXDimension] = rotatedImage.width
        exifDict[kCGImagePropertyExifPixelYDimension] = rotatedImage.height
        props[kCGImagePropertyExifDictionary] = exifDict
      }
    }

    // The top-level kCGImagePropertyOrientation for PNG output must be 1.
    props[kCGImagePropertyOrientation] = 1

    // Update top-level pixel dimensions to reflect the rotated image.
    props[kCGImagePropertyPixelWidth] = rotatedImage.width
    props[kCGImagePropertyPixelHeight] = rotatedImage.height

    return props as CFDictionary
  }

  private func convertHeicToPng(inputPath: String, outputPath: String, compressionLevel: Int, preserveMetadata: Bool) -> FlutterError? {
    let compressionLevel = min(9, max(0, compressionLevel))
    let inputURL = URL(fileURLWithPath: inputPath)
    let outputURL = URL(fileURLWithPath: outputPath)
    let tempURL = outputURL.deletingLastPathComponent()
      .appendingPathComponent(UUID().uuidString + ".heic_native.tmp")

    guard let source = CGImageSourceCreateWithURL(inputURL as CFURL, nil) else {
      return FlutterError(code: "decode_failed", message: "Failed to create image source from \(inputPath)", details: nil)
    }

    guard let destination = CGImageDestinationCreateWithURL(
      tempURL as CFURL,
      "public.png" as CFString,
      1,
      nil
    ) else {
      return FlutterError(code: "encode_failed", message: "Failed to create PNG destination at \(outputPath)", details: nil)
    }

    let cfProperties = buildProperties(compressionLevel: compressionLevel)

    if preserveMetadata {
      guard let cgImage = orientedCGImage(from: source, at: 0) else {
        try? FileManager.default.removeItem(at: tempURL)
        return FlutterError(code: "decode_failed", message: "Failed to decode oriented image from source", details: nil)
      }
      let metaProps = normalizedProperties(from: source, at: 0, baseProperties: cfProperties, rotatedImage: cgImage)
      CGImageDestinationAddImage(destination, cgImage, metaProps)
    } else {
      guard let cgImage = orientedCGImage(from: source, at: 0) else {
        try? FileManager.default.removeItem(at: tempURL)
        return FlutterError(code: "decode_failed", message: "Failed to decode image from source", details: nil)
      }
      CGImageDestinationAddImage(destination, cgImage, cfProperties)
    }

    if CGImageDestinationFinalize(destination) {
      do {
        _ = try FileManager.default.replaceItemAt(outputURL, withItemAt: tempURL)
        return nil
      } catch {
        try? FileManager.default.removeItem(at: tempURL)
        return FlutterError(code: "encode_failed", message: "Failed to finalize PNG output", details: nil)
      }
    } else {
      try? FileManager.default.removeItem(at: tempURL)
      return FlutterError(code: "encode_failed", message: "Failed to finalize PNG output", details: nil)
    }
  }

  private func convertHeicToBytes(inputPath: String, compressionLevel: Int, preserveMetadata: Bool) -> (Data?, FlutterError?) {
    let compressionLevel = min(9, max(0, compressionLevel))
    let inputURL = URL(fileURLWithPath: inputPath)

    guard let source = CGImageSourceCreateWithURL(inputURL as CFURL, nil) else {
      return (nil, FlutterError(code: "decode_failed", message: "Failed to create image source from \(inputPath)", details: nil))
    }

    let data = NSMutableData()
    guard let destination = CGImageDestinationCreateWithData(
      data as CFMutableData,
      "public.png" as CFString,
      1,
      nil
    ) else {
      return (nil, FlutterError(code: "encode_failed", message: "Failed to create PNG destination", details: nil))
    }

    let cfProperties = buildProperties(compressionLevel: compressionLevel)

    if preserveMetadata {
      guard let cgImage = orientedCGImage(from: source, at: 0) else {
        return (nil, FlutterError(code: "decode_failed", message: "Failed to decode oriented image from source", details: nil))
      }
      let metaProps = normalizedProperties(from: source, at: 0, baseProperties: cfProperties, rotatedImage: cgImage)
      CGImageDestinationAddImage(destination, cgImage, metaProps)
    } else {
      guard let cgImage = orientedCGImage(from: source, at: 0) else {
        return (nil, FlutterError(code: "decode_failed", message: "Failed to decode image from source", details: nil))
      }
      CGImageDestinationAddImage(destination, cgImage, cfProperties)
    }

    guard CGImageDestinationFinalize(destination) else {
      return (nil, FlutterError(code: "encode_failed", message: "Failed to finalize PNG output", details: nil))
    }

    return (data as Data, nil)
  }
}
