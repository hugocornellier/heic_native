// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "heic_native",
    platforms: [
        .macOS("10.15")
    ],
    products: [
        .library(name: "heic_native", targets: ["heic_native"])
    ],
    dependencies: [
        .package(name: "FlutterFramework", path: "../FlutterFramework")
    ],
    targets: [
        .target(
            name: "heic_native",
            dependencies: [
                .product(name: "FlutterFramework", package: "FlutterFramework")
            ],
            resources: [
                .process("PrivacyInfo.xcprivacy"),
            ]
        )
    ]
)
