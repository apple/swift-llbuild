// swift-tools-version:5.0

// This file defines Swift package manager support for llbuild. See:
//  https://github.com/apple/swift-package-manager/tree/master/Documentation

import PackageDescription

let package = Package(
    name: "llbuild",
    platforms: [
        .macOS(.v10_10), .iOS(.v9),
    ],
    products: [
        .library(
            name: "libllbuild",
            targets: ["libllbuild"]),
        .library(
            name: "llbuildSwift",
            targets: ["llbuildSwift"]),
        .library(
            name: "llbuildSwiftDynamic",
            type: .dynamic,
            targets: ["llbuildSwift"]),
        .executable(
            name: "llbuild-analyze",
            targets: ["llbuildAnalyzeTool"]),
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-tools-support-core.git", .branch("master"))
    ],
    targets: [
        /// The llbuild testing tool.
        .target(
            name: "llbuild",
            dependencies: ["llbuildCommands"],
            path: "products/llbuild"
        ),

        /// The custom build tool used by the Swift package manager.
        .target(
            name: "swift-build-tool",
            dependencies: ["llbuildBuildSystem"],
            path: "products/swift-build-tool"
        ),

        /// The custom build tool used by the Swift package manager.
        .target(
            name: "llbuildSwift",
            dependencies: ["libllbuild"],
            path: "products/llbuildSwift",
            exclude: []
        ),

        /// The public llbuild API.
        .target(
            name: "libllbuild",
            dependencies: ["llbuildCore", "llbuildBuildSystem"],
            path: "products/libllbuild"
        ),
        
        .target(
            name: "llbuildAnalyzeTool",
            dependencies: ["SwiftToolsSupport-auto"],
            path: "products/llbuild-analyze"),
        
        // MARK: Components
        
        .target(
            name: "llbuildBasic",
            dependencies: ["llvmSupport"],
            path: "lib/Basic"
        ),
        .target(
            name: "llbuildCore",
            dependencies: ["llbuildBasic"],
            path: "lib/Core",
            linkerSettings: [.linkedLibrary("sqlite3")]
        ),
        .target(
            name: "llbuildBuildSystem",
            dependencies: ["llbuildCore"],
            path: "lib/BuildSystem"
        ),
        .target(
            name: "llbuildNinja",
            dependencies: ["llbuildBasic"],
            path: "lib/Ninja"
        ),
        .target(
            name: "llbuildCommands",
            dependencies: ["llbuildCore", "llbuildBuildSystem", "llbuildNinja"],
            path: "lib/Commands"
        ),

        // MARK: Test Targets

        .target(
            name: "llbuildBasicTests",
            dependencies: ["llbuildBasic", "gtest"],
            path: "unittests/Basic"),
        .target(
            name: "llbuildCoreTests",
            dependencies: ["llbuildCore", "gtest"],
            path: "unittests/Core"),
        .target(
            name: "llbuildBuildSystemTests",
            dependencies: ["llbuildBuildSystem", "gtest"],
            path: "unittests/BuildSystem"),
        .target(
            name: "llbuildNinjaTests",
            dependencies: ["llbuildNinja", "gtest"],
            path: "unittests/Ninja"),
        .testTarget(
            name: "llbuildSwiftTests",
            dependencies: ["llbuildSwift"],
            path: "unittests/Swift"),
        
        // MARK: GoogleTest

        .target(
            name: "gtest",
            path: "utils/unittest/googletest/src",
            exclude: [
                "gtest-death-test.cc",
                "gtest-filepath.cc",
                "gtest-port.cc",
                "gtest-printers.cc",
                "gtest-test-part.cc",
                "gtest-typed-test.cc",
                "gtest.cc",
            ]),
        
        // MARK: Ingested LLVM code.
        .target(
          name: "llvmDemangle",
          path: "lib/llvm/Demangle"
        ),

        .target(
            name: "llvmSupport",
            dependencies: ["llvmDemangle"],
            path: "lib/llvm/Support",
            linkerSettings: [.linkedLibrary("ncurses", .when(platforms: [.linux, .macOS]))]
        ),
    ],
    cxxLanguageStandard: .cxx14
)
