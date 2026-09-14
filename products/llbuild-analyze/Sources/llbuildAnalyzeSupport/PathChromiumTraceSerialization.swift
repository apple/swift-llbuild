// This source file is part of the Swift.org open source project
//
// Copyright 2026 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for Swift project authors

import llbuildAnalysis
import llbuildSwift

import struct Foundation.Data
import class Foundation.JSONEncoder

// MARK: - Chromium Trace Serialization

private let microsecondsPerSecond = 1_000_000.0

struct ChromiumTraceEvent: Encodable, Equatable {
    enum CodingKeys: String, CodingKey {
        case name, cat, ph, ts, dur, pid, tid, args
    }

    struct Arguments: Encodable, Equatable {
        let buildKeyKind: String
        let buildKey: String
        let valueKind: String
        let durationSeconds: Double
        let onCriticalPath: Bool
        let dependencies: [String]
    }

    let name: String
    let cat: String
    let ph: String
    let ts: UInt64
    let dur: UInt64
    let pid: Int
    let tid: Int
    let args: Arguments
}

struct ChromiumTraceFile: Encodable, Equatable {
    let traceEvents: [ChromiumTraceEvent]
}

public func chromiumTrace(_ path: CriticalBuildPath, allKeyResults: BuildDBKeysWithResult) throws
    -> Data
{
    let criticalPathKeys = Set(path.map { $0.key })
    let events = allKeyResults.map { element in
        chromiumTraceEvent(for: element, isOnCriticalPath: criticalPathKeys.contains(element.key))
    }

    let encoder = JSONEncoder()
    if #available(OSX 10.13, *) {
        encoder.outputFormatting = [.sortedKeys]
    }
    return try encoder.encode(ChromiumTraceFile(traceEvents: events))
}

func chromiumTraceEvent(
    for element: BuildDBKeysWithResult.Element,
    isOnCriticalPath: Bool
) -> ChromiumTraceEvent {
    return chromiumTraceEvent(
        name: element.key.description,
        buildKeyKind: element.key.kind.description,
        buildKey: element.key.key,
        valueKind: element.result.value.kind.description,
        start: element.result.start,
        duration: element.result.duration,
        isOnCriticalPath: isOnCriticalPath,
        dependencies: element.result.dependencies.map { $0.description }
    )
}

func chromiumTraceEvent(
    name: String,
    buildKeyKind: String,
    buildKey: String,
    valueKind: String,
    start: Double,
    duration: Double,
    isOnCriticalPath: Bool,
    dependencies: [String]
) -> ChromiumTraceEvent {
    return ChromiumTraceEvent(
        name: name,
        cat: isOnCriticalPath ? "critical-path" : "build",
        ph: "X",
        ts: timestampInMicroseconds(start),
        dur: durationInMicroseconds(duration),
        pid: 0,
        tid: 0,
        args: ChromiumTraceEvent.Arguments(
            buildKeyKind: buildKeyKind,
            buildKey: buildKey,
            valueKind: valueKind,
            durationSeconds: duration,
            onCriticalPath: isOnCriticalPath,
            dependencies: dependencies
        )
    )
}

private func timestampInMicroseconds(_ seconds: Double) -> UInt64 {
    return UInt64(max(0, seconds * microsecondsPerSecond))
}

private func durationInMicroseconds(_ seconds: Double) -> UInt64 {
    return UInt64(max(0, seconds * microsecondsPerSecond))
}
