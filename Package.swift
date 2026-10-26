// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "OpenS4kaMac",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "OpenS4ka", targets: ["OpenS4kaMac"])
    ],
    targets: [
        .target(
            name: "OpenS4kaRuntime",
            path: "Sources/OpenS4kaRuntime",
            publicHeadersPath: "include"
        ),
        .executableTarget(
            name: "OpenS4kaMac",
            dependencies: ["OpenS4kaRuntime"],
            path: "Sources/OpenS4kaMac",
            resources: [
                .process("Resources/mortal-kombat-ico.webp"),
                .process("Resources/digimon-rumble-arena-2.webp"),
                .process("Resources/nfs-most-wanted.webp"),
                .process("Resources/opens4ka-bg.webp"),
                .copy("Resources/opens4ka-logo.png"),
                .copy("Resources/CRT.metallib")
            ],
            swiftSettings: [
                .interoperabilityMode(.Cxx)
            ]
        ),
        .testTarget(
            name: "OpenS4kaMacTests",
            dependencies: ["OpenS4kaMac"],
            path: "Tests/OpenS4kaMacTests",
            swiftSettings: [.interoperabilityMode(.Cxx)]
        )
    ],
    cxxLanguageStandard: .cxx20
)
