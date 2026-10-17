// swift-tools-version: 6.0
// SwiftPM package for building the iPhone app with xtool (Linux / Windows WSL / macOS):
//   cd ios && xtool dev
// The Mac/Xcode route (project.yml + XcodeGen) builds the same sources.

import PackageDescription

let package = Package(
    name: "CamStudio",
    platforms: [
        .iOS(.v17),
    ],
    products: [
        // xtool packages the library product named in xtool.yml as the app.
        .library(name: "CamStudio", targets: ["CamStudio"]),
    ],
    targets: [
        .target(
            name: "CamStudio",
            path: "CamStudio",
            swiftSettings: [
                // Written for Swift 5 concurrency rules, like the Xcode target (SWIFT_VERSION 5.9).
                .swiftLanguageMode(.v5),
            ]
        ),
    ]
)
