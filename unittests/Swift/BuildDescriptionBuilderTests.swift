// This source file is part of the Swift.org open source project
//
// Copyright 2026 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for Swift project authors

import Foundation
import XCTest

// The Swift package has llbuildSwift as module
#if SWIFT_PACKAGE
import llbuild
import llbuildSwift
#else
import llbuild
#endif

import llbuildTestSupport

/// A delegate which resolves no tools of its own, so that `shell` and the other
/// builtins fall through to llbuild, and which records enough of the build to
/// tell whether a command actually ran.
final class BuilderTestDelegate: BuildSystemDelegate {
    var fs: FileSystem? { return nil }

    private let lock = NSLock()
    private var startedCommandNames: [String] = []
    private var diagnostics: [String] = []
    private var commandErrors: [String] = []

    /// The names of the commands which executed, in the order they started.
    var startedCommands: [String] {
        lock.lock()
        defer { lock.unlock() }
        return startedCommandNames
    }

    /// Every diagnostic and command error the build produced.
    var errors: [String] {
        lock.lock()
        defer { lock.unlock() }
        return diagnostics + commandErrors
    }

    func lookupTool(_ name: String) -> Tool? { return nil }

    func hadCommandFailure() {}

    func handleDiagnostic(_ diagnostic: Diagnostic) {
        guard diagnostic.kind == .error else { return }
        lock.lock()
        defer { lock.unlock() }
        diagnostics.append(diagnostic.message)
    }

    func commandStatusChanged(_ command: Command, kind: CommandStatusKind) {}

    func commandPreparing(_ command: Command) {}

    func commandStarted(_ command: Command) {
        lock.lock()
        defer { lock.unlock() }
        startedCommandNames.append(command.name)
    }

    func shouldCommandStart(_ command: Command) -> Bool { return true }

    func commandFinished(_ command: Command, result: CommandResult) {}

    func commandFoundDiscoveredDependency(_ command: Command, path: String, kind: DiscoveredDependencyKind) {}

    func commandHadError(_ command: Command, message: String) {
        lock.lock()
        defer { lock.unlock() }
        commandErrors.append("\(command.name): \(message)")
    }

    func commandHadNote(_ command: Command, message: String) {}

    func commandHadWarning(_ command: Command, message: String) {}

    func commandCannotBuildOutputDueToMissingInputs(_ command: Command, output: BuildKey, inputs: [BuildKey]) {}

    func cannotBuildNodeDueToMultipleProducers(output: BuildKey, commands: [Command]) {}

    func commandProcessStarted(_ command: Command, process: ProcessHandle) {}

    func commandProcessHadError(_ command: Command, process: ProcessHandle, message: String) {
        lock.lock()
        defer { lock.unlock() }
        commandErrors.append("\(command.name): \(message)")
    }

    func commandProcessHadOutput(_ command: Command, process: ProcessHandle, data: [UInt8]) {}

    func commandProcessFinished(_ command: Command, process: ProcessHandle, result: CommandExtendedResult) {}

    func cycleDetected(rules: [BuildKey]) {}

    func shouldResolveCycle(rules: [BuildKey], candidate: BuildKey, action: CycleAction) -> Bool {
        return false
    }
}

/// Tests for `BuildSystem.DescriptionSource.inMemory`, which drive a build
/// description through all three layers, the Swift `BuildDescriptionBuilder`,
/// the `llb_buildsystem_description_builder_*` C API and the C++
/// `BuildDescriptionBuilder`, and then run the resulting graph.
///
/// The C++ unit tests check the builder's object graph directly; these check
/// that the bindings on top of it produce a graph that actually builds, and that
/// it is interchangeable with the same graph parsed from a manifest.
@available(macOS 10.15, *)
class BuildDescriptionBuilderTests: XCTestCase {

    /// Create a temporary directory which is removed at teardown.
    private func makeTemporaryDirectory() -> String {
        let path = NSTemporaryDirectory() + "/" + UUID().uuidString
        do {
            try FileManager.default.createDirectory(
                atPath: path, withIntermediateDirectories: true)
        } catch {
            XCTFail("Error while creating temporary directory: \(error)")
        }

        addTeardownBlock {
            try? FileManager.default.removeItem(atPath: path)
        }

        return path
    }

    private func contents(of path: String) -> String? {
        return try? String(contentsOfFile: path, encoding: .utf8)
    }

    /// A shell command runs and its dependent runs after it, with the outputs to
    /// prove it -- the basic end-to-end path, with no manifest written or read.
    func testBuildsAChainOfShellCommands() {
        let directory = makeTemporaryDirectory()
        let first = directory + "/first.txt"
        let second = directory + "/second.txt"

        let delegate = BuilderTestDelegate()
        let system = BuildSystem(
            description: .inMemory(originName: "<test>") { builder in
                builder.addTarget("all", nodes: [second])

                guard let write = builder.beginCommand(name: "1-write", tool: "shell") else {
                    return false
                }
                builder.setCommandOutputs(write, nodes: [first])
                builder.setCommandDescription(write, "write first.txt")
                guard builder.setCommandAttribute(
                    write, name: "args",
                    values: ["/bin/sh", "-c", "echo one > '\(first)'"]) else { return false }
                builder.finishCommand(name: "1-write", write)

                guard let copy = builder.beginCommand(name: "2-copy", tool: "shell") else {
                    return false
                }
                builder.setCommandInputs(copy, nodes: [first])
                builder.setCommandOutputs(copy, nodes: [second])
                builder.setCommandDescription(copy, "write second.txt")
                guard builder.setCommandAttribute(
                    copy, name: "args",
                    values: ["/bin/sh", "-c",
                             "cat '\(first)' > '\(second)'; echo two >> '\(second)'"]) else { return false }
                builder.finishCommand(name: "2-copy", copy)

                return true
            },
            databaseFile: directory + "/build.db",
            delegate: delegate)

        // Nothing about this system is file-based.
        XCTAssertEqual(system.buildFile, "")

        XCTAssertTrue(system.build(target: "all"))
        XCTAssertEqual(delegate.errors, [])

        // The dependency edge is the only thing ordering these, and it held.
        XCTAssertEqual(delegate.startedCommands, ["1-write", "2-copy"])
        XCTAssertEqual(contents(of: first), "one\n")
        XCTAssertEqual(contents(of: second), "one\ntwo\n")
    }

    /// The default target reaches the graph, so a build with no target named
    /// still finds something to do.
    func testBuildsTheDefaultTarget() {
        let directory = makeTemporaryDirectory()
        let output = directory + "/out.txt"

        let delegate = BuilderTestDelegate()
        let system = BuildSystem(
            description: .inMemory(originName: "<test>") { builder in
                builder.addTarget("all", nodes: [output])
                guard builder.setDefaultTarget("all") else { return false }
                // A target which was never added cannot be the default.
                XCTAssertFalse(builder.setDefaultTarget("no-such-target"))

                guard let write = builder.beginCommand(name: "write", tool: "shell") else {
                    return false
                }
                builder.setCommandOutputs(write, nodes: [output])
                guard builder.setCommandAttribute(
                    write, name: "args",
                    values: ["/bin/sh", "-c", "echo hi > '\(output)'"]) else { return false }
                builder.finishCommand(name: "write", write)

                return true
            },
            databaseFile: directory + "/build.db",
            delegate: delegate)

        XCTAssertTrue(system.build())
        XCTAssertEqual(delegate.errors, [])
        XCTAssertEqual(contents(of: output), "hi\n")
    }

    /// An environment base lets an embedder send a shared environment once
    /// rather than once per command. The command's own `env` overrides the base
    /// key by key, and everything else comes from the base -- which is
    /// observable here because `inherit-env` is off, so the base is the entire
    /// environment the command runs with.
    func testSharesAnEnvironmentBase() {
        let directory = makeTemporaryDirectory()
        let output = directory + "/env.txt"

        let delegate = BuilderTestDelegate()
        let system = BuildSystem(
            description: .inMemory(originName: "<test>") { builder in
                builder.addTarget("all", nodes: [output])

                guard builder.addEnvironmentBase("common", bindings: [
                    ("SHARED", "from-base"),
                    ("OVERRIDDEN", "from-base"),
                ]) else { return false }
                // A second base of the same name is refused.
                XCTAssertFalse(builder.addEnvironmentBase("common", bindings: []))

                guard let dump = builder.beginCommand(name: "dump-env", tool: "shell") else {
                    return false
                }
                builder.setCommandOutputs(dump, nodes: [output])
                guard builder.setCommandEnvironmentBase(dump, base: "common") else { return false }
                // A base which was never declared cannot be referenced.
                XCTAssertFalse(builder.setCommandEnvironmentBase(dump, base: "no-such-base"))

                guard builder.setCommandAttribute(dump, name: "inherit-env", value: "false"),
                      builder.setCommandAttribute(
                        dump, name: "env",
                        pairs: [("OVERRIDDEN", "from-command")]),
                      builder.setCommandAttribute(
                        dump, name: "args",
                        values: ["/bin/sh", "-c",
                                 "printf '%s %s\\n' \"$SHARED\" \"$OVERRIDDEN\" > '\(output)'"])
                else { return false }
                builder.finishCommand(name: "dump-env", dump)

                return true
            },
            databaseFile: directory + "/build.db",
            delegate: delegate)

        XCTAssertTrue(system.build(target: "all"))
        XCTAssertEqual(contents(of: output), "from-base from-command\n")
        // The refused reference was diagnosed, not silently dropped. The
        // duplicate base is reported by its return value alone, since only the
        // caller knows whether redeclaring one is an error.
        XCTAssertEqual(delegate.errors, ["unknown environment base 'no-such-base'"])
    }

    /// Node attributes reach the node, and a rejected one is reported back to
    /// the caller rather than silently ignored.
    func testConfiguresNodeAttributes() {
        let directory = makeTemporaryDirectory()
        let output = directory + "/out.txt"

        let delegate = BuilderTestDelegate()
        let system = BuildSystem(
            description: .inMemory(originName: "<test>") { builder in
                builder.addTarget("all", nodes: [output])

                guard builder.setNodeAttribute(node: output, name: "is-mutated", value: "true"),
                      builder.setNodeAttribute(
                        node: output, name: "content-exclusion-patterns",
                        values: ["*.tmp", "*.log"])
                else { return false }

                // Attributes the node does not understand, in each arity.
                XCTAssertFalse(builder.setNodeAttribute(node: output, name: "bogus", value: "x"))
                XCTAssertFalse(builder.setNodeAttribute(node: output, name: "bogus", values: ["x"]))
                XCTAssertFalse(builder.setNodeAttribute(node: output, name: "bogus", pairs: [("x", "y")]))

                guard let write = builder.beginCommand(name: "write", tool: "shell") else {
                    return false
                }
                builder.setCommandOutputs(write, nodes: [output])
                guard builder.setCommandAttribute(
                    write, name: "args",
                    values: ["/bin/sh", "-c", "echo hi > '\(output)'"]) else { return false }
                builder.finishCommand(name: "write", write)

                return true
            },
            databaseFile: directory + "/build.db",
            delegate: delegate)

        XCTAssertTrue(system.build(target: "all"))
        XCTAssertEqual(contents(of: output), "hi\n")
        // Each rejection was diagnosed as well as reported by return value.
        XCTAssertEqual(delegate.errors, Array(repeating: "unexpected attribute: 'bogus'", count: 3))
    }

    /// A tool the delegate does not know and llbuild has no builtin for yields
    /// no command, which the embedder is expected to notice.
    func testUnknownToolYieldsNoCommand() {
        let directory = makeTemporaryDirectory()

        let delegate = BuilderTestDelegate()
        let system = BuildSystem(
            description: .inMemory(originName: "<test>") { builder in
                XCTAssertNil(builder.beginCommand(name: "cmd", tool: "no-such-tool"))
                return false
            },
            databaseFile: directory + "/build.db",
            delegate: delegate)

        // Returning false from `populate` fails initialization, and therefore
        // the build.
        XCTAssertFalse(system.initialize())
        XCTAssertFalse(system.build(target: "all"))
        XCTAssertEqual(delegate.startedCommands, [])
    }

    /// The property the whole feature rests on: a graph built in memory is
    /// interchangeable with the same graph parsed from a manifest. Build it one
    /// way, then the other against the same database, and nothing should re-run
    /// -- which can only be true if the commands' signatures agree across the
    /// two construction paths.
    func testIsInterchangeableWithAnEquivalentBuildFile() {
        let directory = makeTemporaryDirectory()
        let databaseFile = directory + "/build.db"
        let output = directory + "/out.txt"
        let args = ["/bin/sh", "-c", "echo hi > '\(output)'"]
        let environment = [("A", "one"), ("B", "two")]
        let argsYAML = "[" + args.map { "\"\($0)\"" }.joined(separator: ", ") + "]"

        let manifest = """
client:
  name: basic
  version: 0
  file-system: default

targets:
  all: ["\(output)"]

commands:
  write:
    tool: shell
    inputs: []
    outputs: ["\(output)"]
    description: "write out.txt"
    args: \(argsYAML)
    env:
      A: one
      B: two
    inherit-env: false
    allow-missing-inputs: true

"""

        // Build it from a manifest first.
        let fileDelegate = BuilderTestDelegate()
        let fromFile = BuildSystem(
            buildFile: makeTemporaryFile(manifest),
            databaseFile: databaseFile,
            delegate: fileDelegate)
        XCTAssertTrue(fromFile.build(target: "all"))
        XCTAssertEqual(fileDelegate.errors, [])
        XCTAssertEqual(fileDelegate.startedCommands, ["write"])
        XCTAssertEqual(contents(of: output), "hi\n")

        // Now describe exactly the same graph in memory, over the same database.
        func populate(args: [String]) -> (BuildDescriptionBuilder) -> Bool {
            return { builder in
                builder.setFileSystemMode(.full)
                builder.addTarget("all", nodes: [output])

                guard let write = builder.beginCommand(name: "write", tool: "shell") else {
                    return false
                }
                builder.setCommandInputs(write, nodes: [])
                builder.setCommandOutputs(write, nodes: [output])
                builder.setCommandDescription(write, "write out.txt")
                guard builder.setCommandAttribute(write, name: "args", values: args),
                      builder.setCommandAttribute(write, name: "env", pairs: environment),
                      builder.setCommandAttribute(write, name: "inherit-env", value: "false"),
                      builder.setCommandAttribute(write, name: "allow-missing-inputs", value: "true")
                else { return false }
                builder.finishCommand(name: "write", write)

                return true
            }
        }

        let memoryDelegate = BuilderTestDelegate()
        let fromMemory = BuildSystem(
            description: .inMemory(originName: "<test>", populate: populate(args: args)),
            databaseFile: databaseFile,
            delegate: memoryDelegate)
        XCTAssertTrue(fromMemory.build(target: "all"))
        XCTAssertEqual(memoryDelegate.errors, [])
        // The signature matched what the manifest-built command recorded, so
        // there was nothing to do.
        XCTAssertEqual(memoryDelegate.startedCommands, [])

        // And the check above is not vacuous: a graph that really does differ
        // still invalidates the command.
        let changedDelegate = BuilderTestDelegate()
        let changed = BuildSystem(
            description: .inMemory(
                originName: "<test>",
                populate: populate(args: ["/bin/sh", "-c", "echo bye > '\(output)'"])),
            databaseFile: databaseFile,
            delegate: changedDelegate)
        XCTAssertTrue(changed.build(target: "all"))
        XCTAssertEqual(changedDelegate.errors, [])
        XCTAssertEqual(changedDelegate.startedCommands, ["write"])
        XCTAssertEqual(contents(of: output), "bye\n")
    }
}
