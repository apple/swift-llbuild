// This source file is part of the Swift.org open source project
//
// Copyright 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for Swift project authors

import TSCBasic
import llbuildAnalysis
import llbuildAnalyzeSupport
import llbuildSwift
import ArgumentParser

import struct Foundation.Data
import class Foundation.FileManager
import class Foundation.JSONEncoder

struct CriticalPathTool: ParsableCommand {
    static var configuration = CommandConfiguration(commandName: "critical-path", shouldDisplay: true)
    
    enum OutputFormat: String, ExpressibleByArgument {
        case json, graphviz, chromiumTrace = "chromium-trace"

        init?(argument: String) {
            switch argument {
            case "json":
                self = .json
            case "graphviz":
                self = .graphviz
            case "chromium-trace", "chromiumTrace":
                self = .chromiumTrace
            default:
                return nil
            }
        }
    }

    enum GraphvizDisplay: String, ExpressibleByArgument {
        case criticalPath
        case all
    }
    
    @Argument(help: "Path to the build database.", transform: { AbsolutePath($0) })
    var database: AbsolutePath
    
    @Option(name: .shortAndLong, help: "Path to generate exported output to.", transform: { AbsolutePath($0) })
    var output: AbsolutePath?
    
    @Option
    var clientSchemaVersion: Int = 9
    
    @Option(name: [.customShort("f"), .customLong("outputFormat")], help: "The format of the output file.")
    var outputFormat: OutputFormat = .json
    
    @Option(name: .customLong("graphvizOutput"))
    var graphvizDisplay: GraphvizDisplay = .criticalPath
    
    @Flag(help: "If outputFormat is set to json, it will be pretty formatted.")
    var pretty: Bool = false
    
    @Flag(name: .shortAndLong, help: "Set to hide output to stdout and export only.")
    var quiet: Bool = false
    
    func run() throws {
        let db = try BuildDB(path: database.pathString, clientSchemaVersion: UInt32(clientSchemaVersion))
        let allKeysWithResult = try db.getKeysWithResult()
        let solver = CriticalBuildPath.Solver(keys: allKeysWithResult)
        let path = solver.run()
        
        // Output
        if let outputPath = output {
            let data: Data
            switch outputFormat {
            case .json:
                data = try json(path, allKeyResults: allKeysWithResult, buildKeyLookup: solver.keyLookup)
            case .graphviz:
                data = graphViz(path, buildKeyLookup: solver.keyLookup)
            case .chromiumTrace:
                data = try chromiumTrace(path, allKeyResults: allKeysWithResult)
            }
            try verifyOutputPath()
            FileManager.default.createFile(atPath: outputPath.pathString, contents: data)
        }
        
        if quiet { return }
        print(path.elements.isEmpty ? "Couldn't critical path from database at \(database.pathString) because no builds were build." : "Critical Path:\n\(path)")
    }
    
    public func json(_ path: CriticalBuildPath, allKeyResults: BuildDBKeysWithResult, buildKeyLookup: IdentifierFactory<BuildKey>) throws -> Data {
        let encoder = JSONEncoder()
        if pretty {
            encoder.outputFormatting = [.prettyPrinted]
            if #available(OSX 10.13, *) {
                encoder.outputFormatting.insert(.sortedKeys)
            }
        }
        return try encoder.encode(PathOutput(path, allKeyResults: allKeyResults, buildKeyLookup: buildKeyLookup))
    }
    
    private func verifyOutputPath() throws {
        guard let outputPath = output else { return }
        if FileManager.default.fileExists(atPath: outputPath.pathString) {
            throw StringError("Can't output critical path to \(outputPath) - file exists.")
        }
    }
    
    private func graphViz(_ path: CriticalBuildPath, buildKeyLookup: IdentifierFactory<BuildKey>) -> Data {
        var result = "digraph G {\n\tedge [style=dotted]\n"

        var edges = Set<DirectedEdge>()

        if var last = path.first {
            for item in path[1..<path.endIndex] {
                edges.insert(DirectedEdge(a: last.key, b: item.key, isCritical: true))
                last = item
            }
        }

        if graphvizDisplay == .all {
            for element in path {
                for dep in element.result.dependencies {
                    edges.insert(DirectedEdge(a: dep, b: element.key, isCritical: false))
                }
            }
        }

        result += edges.map{ $0.graphVizString }.joined()
        result += "}"
        return result.data(using: .utf8) ?? Data()
    }
}
