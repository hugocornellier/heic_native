#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint heic_native.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'heic_native'
  s.version          = '0.1.1'
  s.summary          = 'Convert HEIC/HEIF images to PNG using native iOS APIs.'
  s.description      = <<-DESC
A Flutter plugin that converts HEIC/HEIF images to PNG format using CoreGraphics and ImageIO on iOS.
                       DESC
  s.homepage         = 'https://github.com/hugocornellier/heic_native'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Hugo Cornellier' => 'hugocornellier@gmail.com' }

  s.source           = { :path => '.' }
  s.source_files     = 'heic_native/Sources/heic_native/**/*.{swift,h,m}'
  s.resource_bundles = { 'heic_native_privacy' => ['heic_native/Sources/heic_native/PrivacyInfo.xcprivacy'] }

  s.dependency 'Flutter'

  s.platform = :ios, '12.0'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
