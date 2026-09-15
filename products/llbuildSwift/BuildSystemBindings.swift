// This source file is part of the Swift.org open source project
//
// Copyright 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for Swift project authors

// This file contains Swift bindings for the llbuild C API.

#if canImport(Darwin)
import Darwin.C
#elseif os(Windows)
import ucrt
import WinSDK
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif canImport(Android)
import Android
#else
#error("Missing libc or equivalent")
#endif

import Foundation

// We don't need this import if we're building
// this file as part of the llbuild framework.
#if !LLBUILD_FRAMEWORK
import llbuild
#endif

private func bytesFromData(_ data: llb_data_t) -> [UInt8] {
    return Array(UnsafeBufferPointer(start: data.data, count: Int(data.length)))
}

/// Delegate for spawning processes through `BuildSystemCommandInterface`.
public protocol ProcessDelegate {
    /// Called when the external process has started executing.
    ///
    /// - pid: The subprocess' identifier, can be -1 for failure reasons.
    func processStarted(pid: llbuild_pid_t?)
    
    /// Called to report an error in the management of a command process.
    ///
    /// - error: The error message.
    func processHadError(error: String)
    
    /// Called to report a command processes' (merged) standard output and error.
    ///
    /// - output: The process output.
    func processHadOutput(output: [UInt8])
    
    /// Called when a command's job has finished executing an external process.
    ///
    /// - result: Whether the process succeeded, failed or was cancelled.
    func processFinished(result: CommandExtendedResult)
}

private class ProcessDelegateWrapper {
    private let delegate: ProcessDelegate
    fileprivate var wrappedDelegate: llb_buildsystem_spawn_delegate_t!
    
    fileprivate init(delegate: ProcessDelegate) {
        self.delegate = delegate
        let wrappedDelegate = llb_buildsystem_spawn_delegate_t(
            context: Unmanaged.passUnretained(self).toOpaque(),
            process_started: { BuildSystem.toProcessDelegateWrapper($0!).delegate.processStarted(pid: $1) },
            process_had_error: { BuildSystem.toProcessDelegateWrapper($0!).delegate.processHadError(error: stringFromData($1!.pointee)) },
            process_had_output: { BuildSystem.toProcessDelegateWrapper($0!).delegate.processHadOutput(output: bytesFromData($1!.pointee)) },
            process_finished: { BuildSystem.toProcessDelegateWrapper($0!).delegate.processFinished(result: CommandExtendedResult($1!)) }
        )
        self.wrappedDelegate = wrappedDelegate
    }
}

/// Interface into the build system to modify the dependencies of a command.
public struct BuildSystemCommandInterface {
    // This struct wraps a bsci object, which should be global, and a task object, which is different for each command.
    // At this time, it is not required to expose the task to the client, so this wrapper abstracts from the task itself.
    // and each command will get a personalized instance of the command interface.
    private let _buildsystemInterface: OpaquePointer
    private let _taskInterface: llb_task_interface_t

    fileprivate init(_ buildsystemInterface: OpaquePointer, _ taskInterface: llb_task_interface_t) {
        _buildsystemInterface = buildsystemInterface
        _taskInterface = taskInterface
    }

    /// Requests an input from the build system which will be provided to the command when it is available
    /// using the `provideValue` method.
    public func commandNeedsInput(key: BuildKey, inputID: UInt) {
        llb_buildsystem_command_interface_task_needs_input(_taskInterface, key.internalBuildKey, inputID)
    }
    
    /// Request an input as a dependency just for the current build iteration.
    /// Once the requesting command finishes, the dependency will be removed so that
    /// incremental builds won't consider it for invalidation.
    ///
    /// NOTE: This method behaves like `request` for the current build.
    public func commandsNeedsSingleUseInput(key: BuildKey, inputID: UInt) {
        llb_buildsystem_command_interface_task_needs_single_use_input(_taskInterface, key.internalBuildKey, inputID)
    }

    /// Marks a build key as a runtime found dependency for the command.
    public func commandDiscoveredDependency(key: BuildKey) {
        llb_buildsystem_command_interface_task_discovered_dependency(_taskInterface, key.internalBuildKey)
    }

    func getFileInfo(_ path: String) throws -> BuildValueFileInfo {
        return llb_buildsystem_command_interface_get_file_info(_buildsystemInterface, path)
    }
    
    /// Spawns a process in the given context
    ///
    /// - commandLine: All command line arguments
    /// - environment: The environment the process will be executed in
    /// - workingDirectory: The path to the directory the process will use
    /// - processDelegate: An instance that handles delegate callbacks about the execution of the process
    public func spawn(_ jobContext: JobContext, commandLine: [String], environment: [String: String], workingDirectory: String, processDelegate: ProcessDelegate) -> Bool {
        let keys = Array(environment.keys)
        let values = Array(environment.values)        
        let wrappedDelegate = ProcessDelegateWrapper(delegate: processDelegate)
        
        var workingDirData = copiedDataFromBytes([UInt8](workingDirectory.utf8))
        
        defer {
            llb_data_destroy(&workingDirData)
        }
        
        return commandLine.withCArrayOfOptionalStrings { commandLinePtr in
            keys.withCArrayOfOptionalStrings { keysPtr in
                values.withCArrayOfOptionalStrings { valuesPtr in
                    llb_buildsystem_command_interface_spawn(_taskInterface, jobContext._context, commandLinePtr, Int32(commandLine.count), keysPtr, valuesPtr, Int32(environment.count), &workingDirData, &wrappedDelegate.wrappedDelegate)
                }
            }
        }
    }
}

/// Opaque object for the context of a job's execution
public struct JobContext {
    fileprivate let _context: OpaquePointer
    
    fileprivate init(_ context: OpaquePointer) {
        _context = context
    }
}

public protocol Tool: AnyObject {
    /// Called to create a specific command instance of this tool.
    func createCommand(_ name: String) -> ExternalCommand?

    /// Called to create a custom command, if the tool accepts the requested CustomTask BuildKey.
    func createCustomCommand(_ buildKey: BuildKey.CustomTask) -> ExternalCommand?
}

public extension Tool {
    // Default implementation to allow clients to avoid declaring this method if not required.
    func createCustomCommand(_ buildKey: BuildKey.CustomTask) -> ExternalCommand? {
        return nil
    }
}

private final class ToolWrapper {
    let tool: Tool

    init(tool: Tool) {
        self.tool = tool
    }

    func createCommand(_ name: UnsafePointer<llb_data_t>) -> OpaquePointer? {
        let command = tool.createCommand(stringFromData(name.pointee)) as ExternalCommand?
        return command.map { command in buildCommand(name, command) } ?? nil
    }

    func createCustomCommand(_ key: OpaquePointer) -> OpaquePointer? {
        // Only process CustomTask keys, as we shouldn't be expecting anything else.
        guard let buildKey = BuildKey.construct(key: key) as? BuildKey.CustomTask,
            let command = tool.createCustomCommand(buildKey) else {
            return nil
        }

        // Since this is a custom task, we can request the name directly instead of doing the roundtrips of
        // data copy when using the BuildKey's name field.
        var name = llb_data_t()
        llb_build_key_get_custom_task_name(key, &name)
        defer {
            llb_data_destroy(&name)
        }

        return buildCommand(&name, command)
    }

    private func buildCommand(_ name: UnsafePointer<llb_data_t>, _ command: ExternalCommand) -> OpaquePointer? {
        let wrapper = CommandWrapper(command: command)
        var _delegate = llb_buildsystem_external_command_delegate_t()
        _delegate.context = Unmanaged.passRetained(wrapper).toOpaque()
        _delegate.destroy_context = { Unmanaged<CommandWrapper>.fromOpaque(UnsafeRawPointer($0!)).release() }
        _delegate.configure = { BuildSystem.toCommandWrapper($0!).configure($1!, $2!, $3!, $4!) }
        _delegate.get_signature = { return BuildSystem.toCommandWrapper($0!).getSignature($1!, $2!) }
        _delegate.start = { return BuildSystem.toCommandWrapper($0!).start($1!, $2!, $3) }
        _delegate.provide_value = { return BuildSystem.toCommandWrapper($0!).provideValue($1!, $2!, $3, $4!, $5) }
        let shouldExecuteDetached = (command as? ExternalDetachedCommand)?.shouldExecuteDetached == true
        if shouldExecuteDetached {
            _delegate.execute_command_detached = {
              return BuildSystem.toCommandWrapper($0!).executeDetachedCommand($1!, $2!, $3, $4!, $5, $6!)
            }
          _delegate.cancel_detached_command = {
            return BuildSystem.toCommandWrapper($0!).cancelDetachedCommand($1!)
          }
        } else {
            _delegate.execute_command = { return BuildSystem.toCommandWrapper($0!).executeCommand($1!, $2!, $3, $4!) }
            if let _ = command as? ProducesCustomBuildValue {
                _delegate.execute_command_ex = {
                    var value: BuildValue = BuildSystem.toCommandWrapper($0!).executeCommand($1!, $2!, $3, $4!)
                    return BuildValue.move(&value)
                }
                _delegate.is_result_valid = {
                    return BuildSystem.toCommandWrapper($0!).isResultValid($1!, $2!)
                }
                _delegate.is_result_valid_with_fallback = {
                    return BuildSystem.toCommandWrapper($0!).isResultValid($1!, $2!, $3!, $4!)
                }
            } else {
                _delegate.execute_command_ex = nil
                _delegate.is_result_valid = nil
                _delegate.is_result_valid_with_fallback = nil
            }
        }

        // Create the low-level command.
        wrapper._command = Command(handle: llb_buildsystem_external_command_create(name, _delegate))

        return wrapper._command.handle
    }
}

public protocol ExternalCommand: AnyObject {
    /// Get a signature used to identify the internal state of the command.
    ///
    /// This is checked to determine if the command needs to rebuild versus the last time it was run.
    func getSignature(_ command: Command) -> [UInt8]
    
    /// Outputs nodes of the command.
    var outputs: [String] { get }
    
    /// Paths to files that contain discovered dependencies after command executed successfully for subsequent builds.
    var dependencyPaths: [String] { get }
    
    /// Format of the dependency files listed in `dependencyPaths`.
    var dependencyDataFormat: DependencyDataFormat { get }

    /// Optional. The command's working directory. Used to resolve relative paths in dependency files.
    var workingDirectory: String? { get }

    /// Called when the command is starting. Commands can request dynamic dependencies using the
    /// commandInterface object.
    ///
    /// - command: A handle to the starting command.
    /// - commandInterface: A handle to the build system's command interface.
    func start(_ command: Command, _ commandInterface: BuildSystemCommandInterface)

    /// Called when the build system obtained a BuildValue from a requested BuildKey. The command may choose
    /// to request additional dependencies through the commandInterface object.
    ///
    /// - command: A handle to the command that is receving the values.
    /// - commandInterface: A handle to the build system's command interface.
    /// - buildValue: The build value requested.
    /// - inputID: Integer for associating requested keys to provided values.
    func provideValue(_ command: Command, _ commandInterface: BuildSystemCommandInterface, _ buildValue: BuildValue, _ inputID: UInt)

    /// Called to execute the given command.
    ///
    /// This method is deprecated in favor of the execute(Command, BuildSystemCommandInterface, JobContext) method.
    ///
    /// - command: A handle to the executing command.
    /// - returns: True on success.
    func execute(_ command: Command) -> Bool

    /// Called to execute the given command.
    ///
    /// This method is deprecated in favor of the execute(Command, BuildSystemCommandInterface, JobContext) method.
    ///
    /// - command: A handle to the executing command.
    /// - commandInterface: A handle to the build system's command interface.
    /// - returns: True on success.
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface) -> Bool
    
    /// Called to execute the given command.
    ///
    /// This method is deprecated in favor of the execute(Command, BuildSystemCommandInterface, JobContext) -> CommandResult method.
    ///
    /// - command: A handle to the executing command.
    /// - commandInterface: A handle to the build system's command interface.
    /// - jobContext: A handle to opaque context of the executing job for spawning external processes.
    /// - returns: True on success.
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface, _ jobContext: JobContext) -> Bool
    
    /// Called to execute the given command.
    ///
    /// - command: A handle to the executing command.
    /// - commandInterface: A handle to the build system's command interface.
    /// - jobContext: A handle to opaque context of the executing job for spawning external processes.
    /// - returns: command execution result.
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface, _ jobContext: JobContext) -> CommandResult
}

public extension ExternalCommand {
    var outputs: [String] {
        []
    }
}

public protocol ExternalDetachedCommand: AnyObject {
    /// Whether the command should run outside the execution lanes.
    /// If true the build system will call `executeDetached` and `cancelDetached`.
    var shouldExecuteDetached: Bool { get }

    /// Called to execute the command, without blocking the execution lanes.
    /// The implementation should do the work asynchronously while returning as
    /// soon as possible.
    ///
    /// - command: A handle to the executing command.
    /// - commandInterface: A handle to the build system's command interface.
    /// - jobContext: A handle to opaque context of the executing job for spawning external processes.
    /// - resultFn: Callback for passing a result and optionally a `BuildValue`.
    func executeDetached(
        _ command: Command,
        _ commandInterface: BuildSystemCommandInterface,
        _ jobContext: JobContext,
        _ resultFn: @escaping (CommandResult, BuildValue?) -> ()
    )

    /// Called to request the command to cancel.
    /// The implementation should aim to return as soon as possible.
    ///
    /// - command: A handle to the executing command.
    func cancelDetached(_ command: Command)
}

public extension ExternalDetachedCommand {
    func cancelDetached(_ command: Command) {}
}

public protocol ProducesCustomBuildValue: AnyObject {
    /// Called to execute the given command that produces a custom build value.
    ///
    /// This method is deprecated in favor of the execute(Command, BuildSystemCommandInterface, JobContext) method.
    ///
    /// - command: A handle to the executing command.
    /// - commandInterface: A handle to the build system's command interface.
    /// - returns: Produced build value.
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface) -> BuildValue
    
    /// Called to execute the given command that produces a custom build value.
    ///
    /// - command: A handle to the executing command.
    /// - commandInterface: A handle to the build system's command interface.
    /// - returns: Produced build value.
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface, _ jobContext: JobContext) -> BuildValue

    /// Called to check if the current result for this command remains valid.
    ///
    /// - command: A handle to the executing command.
    /// - buildValue: The most recently computed build value.
    func isResultValid(_ command: Command, _ buildValue: BuildValue) -> Bool
    
    /// Called to check if the current result for this command remains valid.
    ///
    /// - command: A handle to the executing command.
    /// - buildValue: The most recently computed build value.
    /// - fallback: The default implementation: llbuild::buildsystem::ExternalCommand::isResultValid().
    func isResultValid(_ command: Command, _ buildValue: BuildValue, _ fallback: @escaping (Command, BuildValue) -> Bool) -> Bool
}

public extension ProducesCustomBuildValue {
    func isResultValid(_ command: Command, _ buildValue: BuildValue, _ fallback: @escaping (Command, BuildValue) -> Bool) -> Bool {
        // This should default to the fallback, but instead we defer to ProducesCustomBuildValue.isResultValid(_:_:) for backward compatibility.
        return isResultValid(command, buildValue)
    }
}

// Extension to provide a default implementation of execute(_ Command, _ commandInterface) and
// execute(_ Command, _ commandInterface, _ jobContext) to allow clients to migrate in a staggered
// manner.
public extension ExternalCommand {
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface) -> Bool {
        return execute(command)
    }
    
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface, _ jobContext: JobContext) -> Bool {
        return execute(command, commandInterface)
    }
    
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface, _ jobContext: JobContext) -> CommandResult {
        return execute(command, commandInterface, jobContext) ? .succeeded : .failed
    }

    // If this implementation is invoked, it means that the client implementing ExternalCommand did not
    // implement either of the execute methods, which is a programmer error.
    func execute(_ command: Command) -> Bool {
        fatalError("This should never be called.")
    }
}

// Default implementations for these hooks since they're optional to the client.
public extension ExternalCommand {
    var dependencyDataFormat: DependencyDataFormat { .unused }
    var dependencyPaths: [String] { [] }
    var workingDirectory: String? { nil }
    func start(_ command: Command, _ commandInterface: BuildSystemCommandInterface) {}
    func provideValue(_ command: Command, _ commandInterface: BuildSystemCommandInterface, _ buildValue: BuildValue, _ inputID: UInt) {}
}

// Extension to provide a default implementation of execute(_ Command, _ commandInterface, _ jobContext)
// to allow clients to migrate in a staggered manner.
public extension ProducesCustomBuildValue {
    // If this implementation is invoked, it means that the client implementing ProducesCustomBuildValue
    // did not implement either of the execute methods, which is a programmer error.
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface) -> BuildValue {
        fatalError("This should never be called.")
    }
    
    func execute(_ command: Command, _ commandInterface: BuildSystemCommandInterface, _ jobContext: JobContext) -> BuildValue {
        execute(command, commandInterface)
    }
}

// FIXME: The terminology is really confusing here, we have ExternalCommand which is divorced from the actual internal command implementation of the same name.
private final class CommandWrapper {
    let command: ExternalCommand
    var _command: Command

    init(command: ExternalCommand) {
        self.command = command
        self._command = Command(handle: nil)
    }
    
    func configure(_ context: OpaquePointer, _ single: @convention(c) (OpaquePointer?, llb_data_t, llb_data_t) -> Void, _ collection: @convention(c) (OpaquePointer?, llb_data_t, UnsafeMutablePointer<llb_data_t>?, size_t) -> Void, _ map: @convention(c) (OpaquePointer?, llb_data_t, UnsafeMutablePointer<llb_data_t>?, UnsafeMutablePointer<llb_data_t>?, size_t) -> Void) {
        if !command.dependencyPaths.isEmpty {
            var formatKey = copiedDataFromBytes(Array("deps-style".utf8))
            var value: llb_data_t
            switch command.dependencyDataFormat {
            case .makefile:
                value = copiedDataFromBytes(Array("makefile".utf8))
            case .dependencyinfo:
                value = copiedDataFromBytes(Array("dependency-info".utf8))
            case .makefileIgnoringSubsequentOutputs:
                value = copiedDataFromBytes(Array("makefile-ignoring-subsequent-outputs".utf8))
            default:
                value = copiedDataFromBytes(Array("unusued".utf8))
            }
            defer {
                llb_data_destroy(&formatKey)
                llb_data_destroy(&value)
            }
            single(context, formatKey, value)
            
            var paths: [llb_data_t] = []
            for path in command.dependencyPaths {
                paths.append(copiedDataFromBytes(Array(path.utf8)))
            }
            var depsKey = copiedDataFromBytes(Array("deps".utf8))
            defer {
                llb_data_destroy(&depsKey)
                for index in paths.indices {
                    llb_data_destroy(&paths[index])
                }
            }
            if paths.count == 1 {
                single(context, depsKey, paths[0])
            } else {            
                collection(context, depsKey, &paths, paths.count)
            }
        }
        if let workingDirectory = command.workingDirectory {
            var workingDirectoryKey = copiedDataFromBytes(Array("working-directory".utf8))
            var workingDirectoryValue = copiedDataFromBytes(Array(workingDirectory.utf8))
            defer {
                llb_data_destroy(&workingDirectoryKey)
                llb_data_destroy(&workingDirectoryValue)
            }
            single(context, workingDirectoryKey, workingDirectoryValue)
        }
        if !command.outputs.isEmpty {
            var outputs: [llb_data_t] = []
            for output in command.outputs {
                outputs.append(copiedDataFromBytes(Array(output.utf8)))
            }
            var key = copiedDataFromBytes(Array("outputs".utf8))
            defer {
                llb_data_destroy(&key)
                for index in outputs.indices {
                    llb_data_destroy(&outputs[index])
                }
            }
            if outputs.count == 1 {
                single(context, key, outputs[0])
            } else {
                collection(context, key, &outputs, outputs.count)
            }
        }
    }

    func getSignature(_: OpaquePointer, _ data: UnsafeMutablePointer<llb_data_t>) {
        data.pointee = copiedDataFromBytes(command.getSignature(_command))
    }

    func start(_: OpaquePointer, _ buildsystemInterface: OpaquePointer, _ taskInterface: llb_task_interface_t) {
        let commandInterface = BuildSystemCommandInterface(buildsystemInterface, taskInterface)
        return command.start(_command, commandInterface)
    }

    func provideValue(_: OpaquePointer, _ buildsystemInterface: OpaquePointer, _ taskInterface: llb_task_interface_t, _ value: OpaquePointer, _ inputID: UInt) {

        guard let buildValue = BuildValue.construct(from: value) else {
            fatalError("Could not decode incoming build value.")
        }

        let commandInterface = BuildSystemCommandInterface(buildsystemInterface, taskInterface)
        return command.provideValue(_command, commandInterface, buildValue, inputID)
    }

    func executeCommand(_: OpaquePointer, _ buildsystemInterface: OpaquePointer, _ taskInterface: llb_task_interface_t, _ jobContext: OpaquePointer) -> CommandResult {
        let commandInterface = BuildSystemCommandInterface(buildsystemInterface, taskInterface)
        return command.execute(_command, commandInterface, JobContext(jobContext))
    }

    func executeDetachedCommand(
        _: OpaquePointer,
        _ buildsystemInterface: OpaquePointer,
        _ taskInterface: llb_task_interface_t,
        _ jobContext: OpaquePointer,
        _ resultContext: UnsafeMutableRawPointer?,
        _ resultFn: @escaping (_ resultContext: UnsafeMutableRawPointer?, CommandResult, OpaquePointer?) -> ()
    ) {
        let commandInterface = BuildSystemCommandInterface(buildsystemInterface, taskInterface)
        func resultReceiver(_ result: CommandResult, value: BuildValue?) {
            if var value {
                resultFn(resultContext, result, BuildValue.move(&value))
            } else {
                resultFn(resultContext, result, nil)
            }
        }
        return (command as! ExternalDetachedCommand).executeDetached(_command, commandInterface, JobContext(jobContext), resultReceiver)
    }

    func cancelDetachedCommand(_: OpaquePointer) {
        return (command as! ExternalDetachedCommand).cancelDetached(_command)
    }

    func executeCommand(_: OpaquePointer, _ buildsystemInterface: OpaquePointer, _ taskInterface: llb_task_interface_t, _ jobContext: OpaquePointer) -> BuildValue {
        let commandInterface = BuildSystemCommandInterface(buildsystemInterface, taskInterface)
        return (command as! ProducesCustomBuildValue).execute(_command, commandInterface, JobContext(jobContext))
    }

    func isResultValid(_: OpaquePointer, _ value: OpaquePointer) -> Bool {
        guard let buildValue = BuildValue.construct(from: value) else {
            fatalError("Could not decode incoming build value.")
        }

        return (command as! ProducesCustomBuildValue).isResultValid(_command, buildValue)
    }
    
    func isResultValid(
        _: OpaquePointer,
        _ value: OpaquePointer,
        _ fallbackCtx: UnsafeMutableRawPointer?,
        _ fallback: @escaping (UnsafeMutableRawPointer?, OpaquePointer?, OpaquePointer?) -> Bool
    ) -> Bool {
        guard let buildValue = BuildValue.construct(from: value) else {
            fatalError("Could not decode incoming build value.")
        }
        
        func fallbackWrapper(_ command: Command, value: BuildValue) -> Bool {
            return fallback(fallbackCtx, command.handle, BuildValue.clone(value))
        }
        
        return (command as! ProducesCustomBuildValue).isResultValid(_command, buildValue, fallbackWrapper)
    }
}

/// Encapsulates a diagnostic as reported by the build system.
public struct Diagnostic: Sendable {
    public typealias Kind = DiagnosticKind

    /// The kind of diagnostic.
    public let kind: Kind

    /// The diagnostic location, if provided.
    public let location: (filename: String, line: Int, column: Int)?

    /// The diagnostic text.
    public let message: String
}

extension CommandStatusKind: CustomStringConvertible {
    public var description: String {
        switch self {
        case .isScanning:
            return "isScanning"
        case .isUpToDate:
            return "isUpToDate"
        case .isComplete:
            return "isComplete"
        @unknown default:
            return "unknown"
        }
    }
}

extension BuildKeyKind: CustomStringConvertible {
    public var description: String {
        switch self {
        case .command:
            return "command"
        case .customTask:
            return "customTask"
        case .directoryContents:
            return "directoryContents"
        case .directoryTreeSignature:
            return "directoryTreeSignature"
        case .node:
            return "node"
        case .target:
            return "target"
        case .unknown:
            return "unknown"
        case .directoryTreeStructureSignature:
            return "directoryTreeStructureSignature"
        case .filteredDirectoryContents:
            return "filteredDirectoryContents"
        case .stat:
            return "stat"
        @unknown default:
            return "unknown-\(rawValue)"
        }
    }
}

extension DiagnosticKind: CustomStringConvertible {
    public var description: String {
        switch self {
        case .note:
            return "note"
        case .warning:
            return "warning"
        case .error:
            return "error"
        @unknown default:
            return "unknown"
        }
    }
}

extension SchedulerAlgorithm {
    public init?(rawValue: String) {
        switch rawValue {
        case "commandNamePriority":
            self = .commandNamePriority
        case "fifo":
            self = .fifo
        default:
            return nil
        }
    }
}

/// Handle for a command as invoked by the low-level BuildSystem.
public struct Command: Hashable, CustomStringConvertible, CustomDebugStringConvertible {
    private struct Values: Hashable {
        let name: String
        let shouldShowStatus: Bool
        let description: String
        let verboseDescription: String
    }

    private enum Representation: Hashable {
        case handle(OpaquePointer?)
        case values(Values)
    }

    private let representation: Representation

    fileprivate init(handle: OpaquePointer?) {
        self.representation = .handle(handle)
    }

    /// Creates a dummy `Command` with specific values for testing purposes.
    public init(name: String, shouldShowStatus: Bool, description: String, verboseDescription: String) {
        self.representation = .values(Values(
            name: name,
            shouldShowStatus: shouldShowStatus,
            description: description,
            verboseDescription: verboseDescription
        ))
    }

    fileprivate var handle: OpaquePointer? {
        switch representation {
        case .handle(let handle):
            return handle
        case .values:
            return nil
        }
    }

    /// The command name.
    //
    // FIXME: We shouldn't need to expose this to use for mapping purposes, we should be able to use something more efficient.
    public var name: String {
        switch representation {
        case .handle(let handle):
            var data = llb_data_t()
            withUnsafeMutablePointer(to: &data) { (ptr: UnsafeMutablePointer<llb_data_t>) in
                llb_buildsystem_command_get_name(handle, ptr)
            }
            return stringFromData(data)
        case .values(let values):
            return values.name
        }
    }

    /// Whether the default status reporting shows status for the command.
    public var shouldShowStatus: Bool {
        switch representation {
        case .handle(let handle):
            return llb_buildsystem_command_should_show_status(handle)
        case .values(let values):
            return values.shouldShowStatus
        }
    }

    /// The description provided by the command.
    public var description: String {
        switch representation {
        case .handle(let handle):
            let name = llb_buildsystem_command_get_description(handle)!
            defer { free(name) }
            return String(cString: name)
        case .values(let values):
            return values.description
        }
    }

    /// The verbose description provided by the command.
    public var verboseDescription: String {
        switch representation {
        case .handle(let handle):
            let name = llb_buildsystem_command_get_verbose_description(handle)!
            defer { free(name) }
            return String(cString: name)
        case .values(let values):
            return values.verboseDescription
        } 
    }

    /// The debug description provides the verbose description.
    public var debugDescription: String {
        return verboseDescription
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(representation)
    }

    public static func ==(lhs: Command, rhs: Command) -> Bool {
        return lhs.representation == rhs.representation
    }
}

/// Handle for a process which has been launched by a command.
//
// FIXME: We would like to call this Process, but then it conflicts with Swift's builtin Process. Maybe there is another name?
public struct ProcessHandle: Hashable {
    fileprivate let handle: OpaquePointer

    fileprivate init(_ handle: OpaquePointer) {
        self.handle = handle
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(handle.hashValue)
    }

    public static func ==(lhs: ProcessHandle, rhs: ProcessHandle) -> Bool {
        return lhs.handle == rhs.handle
    }
}

public struct CommandMetrics: Hashable, Sendable {
    public let utime: UInt64         /// User time (in us)
    public let stime: UInt64         /// Sys time (in us)
    public let maxRSS: UInt64        /// Max RSS (in bytes)

    public init(utime: UInt64, stime: UInt64, maxRSS: UInt64) {
        self.utime = utime
        self.stime = stime
        self.maxRSS = maxRSS
    }
}

/// Result of a command execution.
public struct CommandExtendedResult: Sendable {
    public let result: CommandResult    /// The result of a command execution
    public let exitStatus: Int32        /// The exit code
    public let pid: llbuild_pid_t?      /// The process identifier (nil if failed to create a process)
    public let metrics: CommandMetrics? /// Metrics about the executed command

    init(_ result: UnsafePointer<llb_buildsystem_command_extended_result_t>) {
        self.result = result.pointee.result
        self.exitStatus = result.pointee.exit_status
        #if os(Windows)
        let invalidFile = INVALID_HANDLE_VALUE
        #else
        let invalidFile = 0
        #endif
        if result.pointee.pid != invalidFile {
            self.pid = result.pointee.pid
        } else {
            self.pid = nil
        }
        switch self.result {
        case .succeeded, .failed:
            self.metrics = CommandMetrics(utime: result.pointee.utime, stime: result.pointee.stime, maxRSS: result.pointee.maxrss)
        default:
            self.metrics = nil
        }
    }

    public init(result: CommandResult, exitStatus: Int32, pid: llbuild_pid_t?, metrics: CommandMetrics? = nil) {
        self.result = result
        self.exitStatus = exitStatus
        self.pid = pid
        self.metrics = metrics
    }

}

/// File system information for a particular file.
///
/// This is a simple wrapper for stat() information.
public protocol FileInfo {
    /// Creates a new `FileInfo` object.
    init(_ statBuf: stat)

    var statBuf: stat { get }
}

/// Abstracted access to file system operations.
// FIXME: We want to remove this protocol eventually and use the FileSystem
// protocol from SwiftPM's Basic target.
public protocol FileSystem {
    /// Get the contents of a file.
    func read(_ path: String) throws -> [UInt8]

    /// Returns the stat of a file at `path`.
    func getFileInfo(_ path: String) throws -> FileInfo
}

/// Delegate interface for use with the build system.
public protocol BuildSystemDelegate {

    /// The FileSystem to use, if any.
    ///
    /// This is currently very limited.
    var fs: FileSystem? { get }

    /// Called in response to requests for new tools.
    ///
    /// The client should return an appropriate tool implementation if recognized.
    func lookupTool(_ name: String) -> Tool?

    /// Called to report any form of command failure.
    ///
    /// This can may be called to report the failure of a command which has
    /// executed, but may also be used to report the inability of a command to
    /// run. It is expected to be used by the client in making decisions with
    /// regard to cancelling the build.
    func hadCommandFailure()

    /// Called to report an unassociated diagnostic from the build system.
    func handleDiagnostic(_ diagnostic: Diagnostic)

    /// Called when a command has changed state.
    ///
    /// The system guarantees that any commandStart() call will be paired with
    /// exactly one \see commandFinished() call.
    func commandStatusChanged(_ command: Command, kind: CommandStatusKind)

    /// Called when a command is preparing to start.
    ///
    /// The system guarantees that any commandStart() call will be paired with
    /// exactly one \see commandFinished() call.
    func commandPreparing(_ command: Command)

    /// Called when a command has been started.
    ///
    /// The system guarantees that any commandStart() call will be paired with
    /// exactly one \see commandFinished() call.
    func commandStarted(_ command: Command)

    /// Called to allow the delegate to skip commands without cancelling their
    /// dependents. See llbuild's should_command_start.
    func shouldCommandStart(_ command: Command) -> Bool

    /// Called when a command has been finished.
    func commandFinished(_ command: Command, result: CommandResult)

    /// Called to report a discovered dependency.
    func commandFoundDiscoveredDependency(_ command: Command, path: String, kind: DiscoveredDependencyKind)

    /// Called to report an error during the execution of a command.
    func commandHadError(_ command: Command, message: String)

    /// Called to report a note during the execution of a command.
    func commandHadNote(_ command: Command, message: String)

    /// Called to report a warning during the execution of a command.
    func commandHadWarning(_ command: Command, message: String)

    /// Called by the build system to report a command could not build due to
    /// missing inputs.
    func commandCannotBuildOutputDueToMissingInputs(_ command: Command, output: BuildKey, inputs: [BuildKey])

    /// Called by the build system to report a command could not build due to
    /// missing inputs.
    func commandCannotBuildOutputDueToMissingInputs(_ command: Command, output: BuildKey?, inputs: [BuildKey])

    /// Called by the build system when a node has multiple commands that are producing it.
    /// The delegate can return one of the commands for the build system to use or return `nil`
    /// for the build system to treat the node as invalid.
    /// If `nil` is returned `cannotBuildNodeDueToMultipleProducers` is going to be called next.
    func chooseCommandFromMultipleProducers(output: BuildKey, commands: [Command]) -> Command?

    /// Called by the build system to report a node could not be built
    /// because multiple commands are producing it.
    func cannotBuildNodeDueToMultipleProducers(output: BuildKey, commands: [Command])

    /// Called when a command's job has started executing an external process.
    ///
    /// The system guarantees that any commandProcessStarted() call will be paired
    /// with exactly one \see commandProcessFinished() call.
    ///
    /// - parameter process: A unique handle used in subsequent delegate calls
    /// to identify the process. This handle should only be used to associate
    /// different status calls relating to the same process. It is only
    /// guaranteed to be unique from when it has been provided here to when it
    /// has been provided to the \see commandProcessFinished() call.
    func commandProcessStarted(_ command: Command, process: ProcessHandle)

    /// Called to report an error in the management of a command process.
    ///
    /// - parameter process: The process handle.
    /// - parameter message: The error message.
    func commandProcessHadError(_ command: Command, process: ProcessHandle, message: String)

    /// Called to report a command processes' (merged) standard output and error.
    ///
    /// - parameter process: The process handle.
    /// - parameter data: The process output.
    func commandProcessHadOutput(_ command: Command, process: ProcessHandle, data: [UInt8])

    /// Called when a command's job has finished executing an external process.
    ///
    /// - parameter process: The handle used to identify the process. This
    /// handle will become invalid as soon as the client returns from this API
    /// call.
    ///
    /// - parameter result: Whether the process succeeded, failed or was cancelled.
    /// - parameter exitStatus: The raw exit status of the process.
    func commandProcessFinished(_ command: Command, process: ProcessHandle, result: CommandExtendedResult)

    /// Called when it's been determined that a rule needs to run.
    ///
    ///  - parameter ruleNeedingToRun: The rule that needs to run.
    ///
    ///  - parameter reason: Describes why the rule needs to run. For example, because it has never run or because an input was rebuilt.
    ///
    ///  - parameter inputRule: If `reason` is `InputRebuilt`, the rule for the rebuilt input, else  `nil`.
    func determinedRuleNeedsToRun(_ rule: BuildKey, reason: RuleRunReason, inputRule: BuildKey?)

    /// Called when a cycle is detected by the build engine and it cannot make
    /// forward progress.
    func cycleDetected(rules: [BuildKey])

    /// Called when a cycle is detected by the build engine to check if it should
    /// attempt to resolve the cycle and continue
    ///
    /// - parameter rules: The ordered list of items comprising the cycle,
    /// starting from the node which was requested to build and ending with the
    /// first node in the cycle (i.e., the node participating in the cycle will
    /// appear twice).
    /// - parameter candidate: The rule the engine will use to attempt to break the
    /// cycle.
    /// - parameter action: The action the engine will take on the candidateRule.
    ///
    /// Returns true if the engine should attempt to resolve the cycle, false
    /// otherwise. Resolution is attempted by either forcing items to be built, or
    /// supplying a previously built result to a node in the cycle. The latter
    /// action may yield unexpected results and thus this should be opted into
    /// with care.
    func shouldResolveCycle(rules: [BuildKey], candidate: BuildKey, action: CycleAction) -> Bool
}

extension BuildSystemDelegate {
    public func commandFoundDiscoveredDependency(_ command: Command, path: String, kind: DiscoveredDependencyKind) {
        // default implementation for ABI compatibility with older clients
    }

    public func chooseCommandFromMultipleProducers(output: BuildKey, commands: [Command]) -> Command? {
        // default implementation for ABI compatibility with older clients
        return nil
    }

    public func determinedRuleNeedsToRun(_ rule: BuildKey, reason: RuleRunReason, inputRule: BuildKey?) {
        // default implementation for compatibility with older clients
    }

    public func commandCannotBuildOutputDueToMissingInputs(_ command: Command, output: BuildKey?, inputs: [BuildKey]) {
        if let output = output {
            commandCannotBuildOutputDueToMissingInputs(command, output: output, inputs: inputs)
        }
    }
}

/// Utility class for constructing a C-style environment.
private final class CStyleEnvironment {
    /// The list of individual bindings, which must be deallocated.
    private let bindings: [UnsafeMutablePointer<CChar>]?

    /// The environment array, which will be a valid C-style environment pointer
    /// for the lifetime of the instance.
    private let envp: [UnsafePointer<CChar>?]?

    init(_ environment: [String: String]?) {
        if let environment = environment {
            // Allocate the individual binding strings.
            let bindings = environment.map{ "\($0.0)=\($0.1)".withCString(strdup)! }
            self.bindings = bindings

            // Allocate the envp array.
            self.envp = bindings.map{ UnsafePointer($0) } + [nil]
        } else {
            self.bindings = nil
            self.envp = nil
        }
    }

    func withUnsafeEnvPointer(_ body: (UnsafePointer<UnsafePointer<CChar>?>?) -> Void) -> Void {
        if let env = envp {
            env.withUnsafeBufferPointer { ptr in
                body(ptr.baseAddress)
            }
        } else {
            body(nil)
        }
    }

    deinit {
        bindings?.forEach{ free($0) }
    }
}

fileprivate func withData<R>(_ string: String, _ body: (UnsafePointer<llb_data_t>) -> R) -> R {
    var string = string
    return string.withUTF8 { buf in
        var data = llb_data_t(length: UInt64(buf.count), data: buf.baseAddress)
        return withUnsafePointer(to: &data) { body($0) }
    }
}

fileprivate func withDataArray<Strings: Collection, R>(_ strings: Strings, _ body: (UnsafePointer<llb_data_t>?, UInt64) -> R) -> R where Strings.Element == String {
    if strings.isEmpty {
        return body(nil, 0)
    }
    // Pack every string into one buffer, then -- once it has stopped growing, so
    // its address is stable -- point an `llb_data_t` at each one's slice.
    var bytes: [UInt8] = []
    var bounds: [Range<Int>] = []
    bounds.reserveCapacity(strings.count)
    for string in strings {
        let start = bytes.count
        bytes.append(contentsOf: string.utf8)
        bounds.append(start ..< bytes.count)
    }
    return bytes.withUnsafeBufferPointer { buf in
        let datas = bounds.map { llb_data_t(length: UInt64($0.count), data: buf.baseAddress! + $0.lowerBound) }
        return datas.withUnsafeBufferPointer { body($0.baseAddress, UInt64(datas.count)) }
    }
}

fileprivate func withDataArrayPairs<R>(_ pairs: [(String, String)], _ body: (UnsafePointer<llb_data_t>?, UnsafePointer<llb_data_t>?, UInt64) -> R) -> R {
    // `lazy` so neither side is materialized into an array of its own.
    withDataArray(pairs.lazy.map(\.0)) { keys, count in
        withDataArray(pairs.lazy.map(\.1)) { values, _ in
            body(keys, values, count)
        }
    }
}

/// Constructs an llbuild build description in memory, driving the same object
/// primitives the manifest parser uses, so the resulting description, including
/// every command's signature, is indistinguishable from one parsed from the
/// equivalent manifest.
///
/// Vended transiently to the `populate` closure of
/// `BuildSystem.DescriptionSource.inMemory`; do not retain it beyond that
/// closure.
public final class BuildDescriptionBuilder {
    /// The underlying `llb_buildsystem_description_builder_t*`.
    private let handle: OpaquePointer

    fileprivate init(_ handle: OpaquePointer) {
        self.handle = handle
    }

    /// Set the file-system mode the build system runs with (equivalent to the
    /// manifest's `client.file-system` property).
    ///
    /// This configures the build system rather than the graph. Clients whose
    /// manifests declare a non-default mode must set it here too, or the two
    /// paths will disagree about which outputs are up to date.
    public func setFileSystemMode(_ mode: BuildSystemFileSystemMode) {
        llb_buildsystem_description_builder_set_file_system_mode(handle, mode)
    }

    /// Declare a target with the given member node names.
    public func addTarget(_ name: String, nodes: [String]) {
        withData(name) { namePtr in
            withDataArray(nodes) { nodesPtr, count in
                llb_buildsystem_description_builder_add_target(handle, namePtr, nodesPtr, count)
            }
        }
    }

    /// Set the default target, which must already have been added.
    @discardableResult
    public func setDefaultTarget(_ name: String) -> Bool {
        withData(name) { llb_buildsystem_description_builder_set_default_target(handle, $0) }
    }

    /// Configure a scalar attribute on the named node, declaring it if this is
    /// its first mention.
    @discardableResult
    public func setNodeAttribute(node: String, name: String, value: String) -> Bool {
        withData(node) { nodePtr in
            withData(name) { namePtr in
                withData(value) { valuePtr in
                    llb_buildsystem_description_builder_set_node_attribute(handle, nodePtr, namePtr, valuePtr)
                }
            }
        }
    }

    /// Configure a sequence attribute on the named node.
    @discardableResult
    public func setNodeAttribute(node: String, name: String, values: [String]) -> Bool {
        withData(node) { nodePtr in
            withData(name) { namePtr in
                withDataArray(values) { valuesPtr, count in
                    llb_buildsystem_description_builder_set_node_attribute_list(handle, nodePtr, namePtr, valuesPtr, count)
                }
            }
        }
    }

    /// Configure a map attribute on the named node.
    @discardableResult
    public func setNodeAttribute(node: String, name: String, pairs: [(String, String)]) -> Bool {
        withData(node) { nodePtr in
            withData(name) { namePtr in
                withDataArrayPairs(pairs) { keysPtr, valuesPtr, count in
                    llb_buildsystem_description_builder_set_node_attribute_map(handle, nodePtr, namePtr, keysPtr, valuesPtr, count)
                }
            }
        }
    }

    /// Declare a shared environment table that commands can inherit from.
    ///
    /// A command pointed at a base by `setCommandEnvironmentBase` takes the
    /// base's bindings as its environment, with its own `env` attribute
    /// overriding individual keys *in place*, so the effective environment
    /// keeps the base's ordering, and factoring a command's environment into a
    /// base leaves its signature unchanged.
    ///
    /// This is how a client with many commands whose environments differ in only
    /// a few keys avoids transferring, storing, and hashing the shared bulk once
    /// per command.
    ///
    /// - returns: False if a base of this name was already declared.
    @discardableResult
    public func addEnvironmentBase(_ name: String, bindings: [(String, String)]) -> Bool {
        withData(name) { namePtr in
            withDataArrayPairs(bindings) { keysPtr, valuesPtr, count in
                llb_buildsystem_description_builder_add_environment_base(handle, namePtr, keysPtr, valuesPtr, count)
            }
        }
    }

    /// Create a command for the named tool. Returns nil on failure (unknown
    /// tool, or the tool declined). Configure it with the `setCommand*` methods,
    /// then call `finishCommand`.
    public func beginCommand(name: String, tool: String) -> Command? {
        let commandHandle = withData(name) { namePtr in
            withData(tool) { toolPtr in
                llb_buildsystem_description_builder_begin_command(handle, namePtr, toolPtr)
            }
        }
        guard let commandHandle else { return nil }
        return Command(handle: commandHandle)
    }

    public func setCommandInputs(_ command: Command, nodes: [String]) {
        withDataArray(nodes) { nodesPtr, count in
            llb_buildsystem_description_builder_set_command_inputs(handle, command.handle, nodesPtr, count)
        }
    }

    public func setCommandOutputs(_ command: Command, nodes: [String]) {
        withDataArray(nodes) { nodesPtr, count in
            llb_buildsystem_description_builder_set_command_outputs(handle, command.handle, nodesPtr, count)
        }
    }

    public func setCommandDescription(_ command: Command, _ description: String) {
        withData(description) {
            llb_buildsystem_description_builder_set_command_description(handle, command.handle, $0)
        }
    }

    @discardableResult
    public func setCommandAttribute(_ command: Command, name: String, value: String) -> Bool {
        withData(name) { namePtr in
            withData(value) { valuePtr in
                llb_buildsystem_description_builder_set_command_attribute(handle, command.handle, namePtr, valuePtr)
            }
        }
    }

    @discardableResult
    public func setCommandAttribute(_ command: Command, name: String, values: [String]) -> Bool {
        withData(name) { namePtr in
            withDataArray(values) { valuesPtr, count in
                llb_buildsystem_description_builder_set_command_attribute_list(handle, command.handle, namePtr, valuesPtr, count)
            }
        }
    }

    @discardableResult
    public func setCommandAttribute(_ command: Command, name: String, pairs: [(String, String)]) -> Bool {
        withData(name) { namePtr in
            withDataArrayPairs(pairs) { keysPtr, valuesPtr, count in
                llb_buildsystem_description_builder_set_command_attribute_map(handle, command.handle, namePtr, keysPtr, valuesPtr, count)
            }
        }
    }

    /// Point `command` at an environment base declared with
    /// `addEnvironmentBase`. Its own `env` attribute, if any, overrides
    /// individual keys.
    ///
    /// - returns: False if no such base was declared, or if the command does not
    ///   support environments.
    @discardableResult
    public func setCommandEnvironmentBase(_ command: Command, base: String) -> Bool {
        withData(base) {
            llb_buildsystem_description_builder_set_command_environment_base(handle, command.handle, $0)
        }
    }

    /// Register the fully-configured command. Call once, after all `setCommand*`
    /// calls for it; the `Command` must not be used afterwards.
    public func finishCommand(name: String, _ command: Command) {
        withData(name) {
            llb_buildsystem_description_builder_finish_command(handle, $0, command.handle)
        }
    }

    /// Request that ownership analysis run when the description is finalized
    /// (equivalent to the manifest's `perform-ownership-analysis: yes`).
    public func setPerformOwnershipAnalysis(_ value: Bool) {
        llb_buildsystem_description_builder_set_perform_ownership_analysis(handle, value)
    }
}

/// This class allows building using llbuild's native BuildSystem component.
public final class BuildSystem {
    public typealias SchedulerAlgorithm = llbuild.SchedulerAlgorithm

    public typealias QualityOfService = llbuild.QualityOfService

    /// Where the system gets its build description, decided once at construction.
    public enum DescriptionSource {
        /// Parse an llbuild build file from disk.
        case buildFile(String)

        /// Construct the description in memory, driving the same object
        /// primitives the parser uses, so no file is read or written.
        ///
        /// `populate` is invoked once, during initialization; return false from
        /// it to fail initialization. `originName` labels the description in any
        /// diagnostics it produces.
        ///
        /// Because a relative `databaseFile` is otherwise resolved against the
        /// build file's directory, it must be absolute with this source.
        case inMemory(originName: String, populate: (BuildDescriptionBuilder) -> Bool)
    }

    /// Where the system's build description comes from.
    public let descriptionSource: DescriptionSource

    /// The build file that the system is configured with, empty if the
    /// description is built in memory.
    public var buildFile: String {
        switch descriptionSource {
        case .buildFile(let path): return path
        case .inMemory: return ""
        }
    }

    /// The delegate used by the system.
    public let delegate: BuildSystemDelegate

    /// The internal llbuild build system.
    private var _system: OpaquePointer? = nil

    /// The C environment, if used.
    private let _cEnvironment: CStyleEnvironment

    /// Parse the build description from a build file on disk.
    public convenience init(buildFile: String, databaseFile: String, delegate: BuildSystemDelegate, environment: [String: String]? = nil, serial: Bool = false, traceFile: String? = nil, schedulerAlgorithm: SchedulerAlgorithm = .commandNamePriority, schedulerLanes: UInt32 = 0, qos: QualityOfService? = nil) {
        self.init(description: .buildFile(buildFile), databaseFile: databaseFile, delegate: delegate, environment: environment, serial: serial, traceFile: traceFile, schedulerAlgorithm: schedulerAlgorithm, schedulerLanes: schedulerLanes, qos: qos)
    }

    public init(description: DescriptionSource, databaseFile: String, delegate: BuildSystemDelegate, environment: [String: String]? = nil, serial: Bool = false, traceFile: String? = nil, schedulerAlgorithm: SchedulerAlgorithm = .commandNamePriority, schedulerLanes: UInt32 = 0, qos: QualityOfService? = nil) {

        // Safety check that we have linked against a compatibile llbuild framework version
        if llb_get_api_version() != LLBUILD_C_API_VERSION {
            fatalError("llbuild C API version mismatch, found \(llb_get_api_version()), expect \(LLBUILD_C_API_VERSION)")
        }

        self.descriptionSource = description
        self.delegate = delegate

        // llbuild uses the build file path only to parse the description and to
        // resolve a relative database path against; an in-memory description has
        // neither need, and passes its origin name separately.
        let buildFilePath: String
        switch description {
        case .buildFile(let path):
            buildFilePath = path
        case .inMemory:
            buildFilePath = ""
        }

        // Create a stable C string path.
        let pathPtr = strdup(buildFilePath)
        defer {
            if let pathPtr = pathPtr {
                free(pathPtr)
            }
        }

        let dbPathPtr = strdup(databaseFile)
        defer {
            if let dbPathPtr = dbPathPtr {
                free(dbPathPtr)
            }
        }

        let tracePathPtr = strdup(traceFile ?? "")
        defer {
            if let tracePathPtr = tracePathPtr {
                free(tracePathPtr)
            }
        }

        // Allocate a C style environment, if necessary.
        _cEnvironment = CStyleEnvironment(environment)

        _cEnvironment.withUnsafeEnvPointer { envp in
            var _invocation = llb_buildsystem_invocation_t()
            _invocation.buildFilePath = UnsafePointer(pathPtr)
            _invocation.dbPath = UnsafePointer(dbPathPtr)
            _invocation.traceFilePath = UnsafePointer(tracePathPtr)
            _invocation.environment = envp
            _invocation.showVerboseStatus = true
            _invocation.useSerialBuild = serial
            _invocation.schedulerAlgorithm = schedulerAlgorithm
            _invocation.schedulerLanes = schedulerLanes
            _invocation.qos = qos ?? .unspecified

            // Construct the system delegate.
            var _delegate = llb_buildsystem_delegate_t()
            _delegate.context = Unmanaged.passUnretained(self).toOpaque()
            if delegate.fs != nil {
                _delegate.fs_get_file_contents = { BuildSystem.toSystem($0!).fsGetFileContents(String(cString: $1!), $2!) }
                _delegate.fs_get_file_info = { BuildSystem.toSystem($0!).fsGetFileInfo(String(cString: $1!), $2!) }
                // FIXME: This should be a separate callback, not shared with getFileInfo (or get FileInfo should take a parameter).
                _delegate.fs_get_link_info = { BuildSystem.toSystem($0!).fsGetFileInfo(String(cString: $1!), $2!) }

                // FIXME: should support fs_create_symlink, but for now explicitly defers to built-in symlink
                _delegate.fs_create_symlink = nil
            }
            _delegate.lookup_tool = { return BuildSystem.toSystem($0!).lookupTool($1!) }
            _delegate.had_command_failure = { BuildSystem.toSystem($0!).hadCommandFailure() }
            _delegate.handle_diagnostic = { BuildSystem.toSystem($0!).handleDiagnostic($1, String(cString: $2!), Int($3), Int($4), String(cString: $5!)) }
            _delegate.command_status_changed = { BuildSystem.toSystem($0!).commandStatusChanged(Command(handle: $1), $2) }
            _delegate.command_preparing = { BuildSystem.toSystem($0!).commandPreparing(Command(handle: $1)) }
            _delegate.command_started = { BuildSystem.toSystem($0!).commandStarted(Command(handle: $1)) }
            _delegate.should_command_start = { BuildSystem.toSystem($0!).shouldCommandStart(Command(handle: $1)) }
            _delegate.command_finished = { BuildSystem.toSystem($0!).commandFinished(Command(handle: $1), $2) }
            _delegate.command_found_discovered_dependency = { BuildSystem.toSystem($0!).commandFoundDiscoveredDependency(Command(handle: $1), String(cString: $2!), $3) }
            _delegate.command_had_error = { BuildSystem.toSystem($0!).commandHadError(Command(handle: $1), $2!) }
            _delegate.command_had_note = { BuildSystem.toSystem($0!).commandHadNote(Command(handle: $1), $2!) }
            _delegate.command_had_warning = { BuildSystem.toSystem($0!).commandHadWarning(Command(handle: $1), $2!) }
            _delegate.command_cannot_build_output_due_to_missing_inputs = {
                let inputsPtr = $3
                let inputs = (0..<Int($4)).map { BuildKey.construct(key: inputsPtr![$0]!) }
                let output = $2?.pointee.map { BuildKey.construct(key: $0) }
                BuildSystem.toSystem($0!).commandCannotBuildOutputDueToMissingInputs(Command(handle: $1), output, inputs)
            }
            _delegate.choose_command_from_multiple_producers = {
                let commandsPtr = $2!
                let commands = (0..<Int($3)).map { Command(handle: commandsPtr[$0]) }
                let chosenCommand = BuildSystem.toSystem($0!).chooseCommandFromMultipleProducers(BuildKey.construct(key: $1!.pointee!), commands)
                return chosenCommand?.handle
            }
            _delegate.cannot_build_node_due_to_multiple_producers = {
                let commandsPtr = $2!
                let commands = (0..<Int($3)).map { Command(handle: commandsPtr[$0]) }
                BuildSystem.toSystem($0!).cannotBuildNodeDueToMultipleProducers(BuildKey.construct(key: $1!.pointee!), commands)
            }
            _delegate.command_process_started = { BuildSystem.toSystem($0!).commandProcessStarted(Command(handle: $1), ProcessHandle($2!)) }
            _delegate.command_process_had_error = { BuildSystem.toSystem($0!).commandProcessHadError(Command(handle: $1), ProcessHandle($2!), $3!) }
            _delegate.command_process_had_output = { BuildSystem.toSystem($0!).commandProcessHadOutput(Command(handle: $1), ProcessHandle($2!), $3!) }
            _delegate.command_process_finished = { BuildSystem.toSystem($0!).commandProcessFinished(Command(handle: $1), ProcessHandle($2!), CommandExtendedResult($3!)) }
            _delegate.determined_rule_needs_to_run = {
                BuildSystem.toSystem($0!).determinedRuleNeedsToRun(BuildKey.construct(key: $1!), reason: $2, inputRule: $3.map { BuildKey.construct(key: $0) })
            }
            _delegate.cycle_detected = {
                var rules = [BuildKey]()
                UnsafeBufferPointer(start: $1, count: Int($2)).forEach {
                    rules.append(BuildKey.construct(key: $0!))
                }
                BuildSystem.toSystem($0!).cycleDetected(rules)
            }
            _delegate.should_resolve_cycle = {
                var rules = [BuildKey]()
                UnsafeBufferPointer(start: $1, count: Int($2)).forEach {
                    rules.append(BuildKey.construct(key: $0!))
                }
                let candidate = BuildKey.construct(key: $3!)

                let result = BuildSystem.toSystem($0!).shouldResolveCycle(rules, candidate, $4)

                return (result) ? 1 : 0;
            }

            // Create the system, over whichever description source was asked for.
            switch description {
            case .buildFile:
                _system = llb_buildsystem_create(_delegate, _invocation)
            case .inMemory(let originName, _):
                var originData = copiedDataFromBytes([UInt8](originName.utf8))
                defer { llb_data_destroy(&originData) }
                _system = llb_buildsystem_create_with_description(
                    _delegate, _invocation, &originData,
                    Unmanaged.passUnretained(self).toOpaque()
                ) { context, builderHandle in
                    guard case .inMemory(_, let populate) = BuildSystem.toSystem(context!).descriptionSource else {
                        preconditionFailure("in-memory description callback on a file-based build system")
                    }
                    return populate(BuildDescriptionBuilder(builderHandle!))
                }
            }
        }
    }

    deinit {
        assert(_system != nil)
        llb_buildsystem_destroy(_system)
    }

    /// Build a single node.
    ///
    /// The client is responsible for ensuring only one build is ever executing concurrently.
    ///
    /// - parameter node: Path to a single node to build.
    /// - returns: True if the build was successful, false otherwise.
    public func build(node: String) -> Bool {
        var data = copiedDataFromBytes([UInt8](node.utf8))
        defer {
            llb_data_destroy(&data)
        }
        return llb_buildsystem_build_node(_system, &data)
    }

    /// Build the default target, or optionally a specific target.
    ///
    /// The client is responsible for ensuring only one build is ever executing concurrently.
    ///
    /// - parameter target: Optional name of the target to build.
    /// - returns: True if the build was successful, false otherwise.
    public func build(target: String? = nil) -> Bool {
        var data = target.map({ copiedDataFromBytes([UInt8]($0.utf8)) }) ?? llb_data_t(length: 0, data: nil)
        defer {
            llb_data_destroy(&data)
        }
        return llb_buildsystem_build(_system, &data)
    }

    /// Cancel any running build.
    public func cancel() {
        llb_buildsystem_cancel(_system)
    }

    /// Initialize the build system: obtain the build description from the source
    /// the system was constructed with, and apply options (for example,
    /// attaching the database), without building anything. `build`
    /// auto-initializes, so calling this is only needed to do the work
    /// separately, to measure it, or to fail early. It is a no-op once
    /// initialized.
    ///
    /// - returns: True on success, false if initialization failed.
    @discardableResult
    public func initialize() -> Bool {
        return llb_buildsystem_initialize(_system)
    }

    /// MARK: Internal Delegate Implementation

    /// Helper function for getting the system from the delegate context.
    static private func toSystem(_ context: UnsafeMutableRawPointer) -> BuildSystem {
        return Unmanaged<BuildSystem>.fromOpaque(UnsafeRawPointer(context)).takeUnretainedValue()
    }

    /// Helper function for getting the tool wrapper from the delegate context.
    static private func toToolWrapper(_ context: UnsafeMutableRawPointer) -> ToolWrapper {
        return Unmanaged<ToolWrapper>.fromOpaque(UnsafeRawPointer(context)).takeUnretainedValue()
    }

    /// Helper function for getting the command wrapper from the delegate context.
    static fileprivate func toCommandWrapper(_ context: UnsafeMutableRawPointer) -> CommandWrapper {
        return Unmanaged<CommandWrapper>.fromOpaque(UnsafeRawPointer(context)).takeUnretainedValue()
    }
    
    /// Helper function for getting the process delegate wrapper from the delegate context.
    static fileprivate func toProcessDelegateWrapper(_ context: UnsafeMutableRawPointer) -> ProcessDelegateWrapper {
        return Unmanaged<ProcessDelegateWrapper>.fromOpaque(UnsafeRawPointer(context)).takeUnretainedValue()
    }

    private func fsGetFileContents(_ path: String, _ data: UnsafeMutablePointer<llb_data_t>) -> Bool {
        let fs = delegate.fs!

        // Get the contents for the file.
        guard let contents = try? fs.read(path) else {
            return false
        }

        data.pointee = copiedDataFromBytes(contents)

        return true
    }

    private func fsGetFileInfo(_ path: String, _ info: UnsafeMutablePointer<llb_fs_file_info_t>) {
        // If the path doesn't exist, it is missing.
        let fs = delegate.fs!
        guard let s = try? fs.getFileInfo(path).statBuf else {
            info.pointee = llb_fs_file_info_t()
            return
        }

        // Otherwise, we have some kind of file.
        info.pointee.device = UInt64(s.st_dev)
        info.pointee.inode = UInt64(s.st_ino)
        info.pointee.mode = UInt64(s.st_mode)
        info.pointee.size = UInt64(s.st_size)
        #if canImport(Darwin)
        info.pointee.mod_time.seconds = UInt64(s.st_mtimespec.tv_sec)
        info.pointee.mod_time.nanoseconds = UInt64(s.st_mtimespec.tv_nsec)
        #elseif os(Windows)
        info.pointee.mod_time.seconds = UInt64(s.st_mtime)
        info.pointee.mod_time.nanoseconds = 0
        #elseif canImport(Glibc) || canImport(Musl) || canImport(Android)
        info.pointee.mod_time.seconds = UInt64(s.st_mtim.tv_sec)
        info.pointee.mod_time.nanoseconds = UInt64(s.st_mtim.tv_nsec)
        #else
        #error("Missing libc or equivalent")
        #endif
    }

    private func fsGetLinkInfo(_ path: String, _ info: UnsafeMutablePointer<llb_fs_file_info_t>) {
        // FIXME: We do not support this natively, yet.
        return fsGetFileInfo(path, info)
    }

    private func lookupTool(_ name: UnsafePointer<llb_data_t>) -> OpaquePointer? {
        // Look up the named tool.
        guard let tool = delegate.lookupTool(stringFromData(name.pointee)) else {
            return nil
        }

        // If we got a tool, save it and create an appropriate low-level instance.
        let wrapper = ToolWrapper(tool: tool)
        var _delegate = llb_buildsystem_tool_delegate_t()
        _delegate.context = Unmanaged.passRetained(wrapper).toOpaque()
        _delegate.destroy_context = { Unmanaged<CommandWrapper>.fromOpaque(UnsafeRawPointer($0!)).release() }
        _delegate.create_command = { return BuildSystem.toToolWrapper($0!).createCommand($1!) }
        _delegate.create_custom_command = { return BuildSystem.toToolWrapper($0!).createCustomCommand($1!) }

        // Create the tool.
        return llb_buildsystem_tool_create(name, _delegate)
    }

    private func hadCommandFailure() {
        delegate.hadCommandFailure()
    }

    private func handleDiagnostic(_ kind: DiagnosticKind, _ filename: String, _ line: Int, _ column: Int, _ message: String) {
        // Clean up the location.
        let location: (filename: String, line: Int, column: Int)?
        if filename == "<unknown>" || (line == -1 && column == -1) {
            location = nil
        } else {
            location = (filename: filename, line: line, column: column)
        }

        delegate.handleDiagnostic(Diagnostic(kind: kind, location: location, message: message))
    }

    private func commandStatusChanged(_ command: Command, _ kind: CommandStatusKind) {
        delegate.commandStatusChanged(command, kind: kind)
    }

    private func commandPreparing(_ command: Command) {
        delegate.commandPreparing(command)
    }

    private func commandStarted(_ command: Command) {
        delegate.commandStarted(command)
    }

    private func shouldCommandStart(_ command: Command) -> Bool {
        return delegate.shouldCommandStart(command)
    }

    private func commandFinished(_ command: Command, _ result: CommandResult) {
        delegate.commandFinished(command, result: result)
    }

    private func commandFoundDiscoveredDependency(_ command: Command, _ path: String, _ kind: DiscoveredDependencyKind) {
        delegate.commandFoundDiscoveredDependency(command, path: path, kind: kind)
    }

    private func commandHadError(_ command: Command, _ data: UnsafePointer<llb_data_t>) {
        delegate.commandHadError(command, message: stringFromData(data.pointee))
    }

    private func commandHadNote(_ command: Command, _ data: UnsafePointer<llb_data_t>) {
        delegate.commandHadNote(command, message: stringFromData(data.pointee))
    }

    private func commandHadWarning(_ command: Command, _ data: UnsafePointer<llb_data_t>) {
        delegate.commandHadWarning(command, message: stringFromData(data.pointee))
    }

    private func commandCannotBuildOutputDueToMissingInputs(_ command: Command, _ output: BuildKey?, _ inputs: [BuildKey]) {
        delegate.commandCannotBuildOutputDueToMissingInputs(command, output: output, inputs: inputs)
    }

    private func chooseCommandFromMultipleProducers(_ output: BuildKey, _ commands: [Command]) -> Command? {
        return delegate.chooseCommandFromMultipleProducers(output: output, commands: commands)
    }

    private func cannotBuildNodeDueToMultipleProducers(_ output: BuildKey, _ commands: [Command]) {
        delegate.cannotBuildNodeDueToMultipleProducers(output: output, commands: commands)
    }

    private func commandProcessStarted(_ command: Command, _ process: ProcessHandle) {
        delegate.commandProcessStarted(command, process: process)
    }

    private func commandProcessHadError(_ command: Command, _ process: ProcessHandle, _ data: UnsafePointer<llb_data_t>) {
        delegate.commandProcessHadError(command, process: process, message: stringFromData(data.pointee))
    }

    private func commandProcessHadOutput(_ command: Command, _ process: ProcessHandle, _ data: UnsafePointer<llb_data_t>) {
        delegate.commandProcessHadOutput(command, process: process, data: bytesFromData(data.pointee))
    }

    private func commandProcessFinished(_ command: Command, _ process: ProcessHandle, _ result: CommandExtendedResult) {
        delegate.commandProcessFinished(command, process: process, result: result)
    }

    private func determinedRuleNeedsToRun(_ rule: BuildKey, reason: RuleRunReason, inputRule: BuildKey?) {
        delegate.determinedRuleNeedsToRun(rule, reason: reason, inputRule: inputRule)
    }

    private func cycleDetected(_ rules: [BuildKey]) {
        delegate.cycleDetected(rules: rules)
    }

    private func shouldResolveCycle(_ rules: [BuildKey], _ candidate: BuildKey, _ action: CycleAction) -> Bool {
        return delegate.shouldResolveCycle(rules: rules, candidate: candidate, action: action)
    }


    /// Toggle tracing
    public static func setTracing(enabled: Bool) {
        if (enabled) {
            llb_enable_tracing()
        } else {
            llb_disable_tracing()
        }
    }

    /// Get the scheduler algorithm
    public static func getSchedulerAlgorithm() -> SchedulerAlgorithm {
        // OBSOLETE: remove once downstream client(s) have adopted initializer
        return .commandNamePriority
    }

    /// Set scheduler algorithm
    public static func setSchedulerAlgorithm(_ algorithm: SchedulerAlgorithm) {
        // OBSOLETE: remove once downstream clients have adopted initializer
    }

    /// Get scheduler lane width
    public static func getSchedulerLaneWidth() -> UInt32 {
        // OBSOLETE: remove once downstream clients have adopted initializer
        return 0
    }

    /// Set scheduler lane width
    public static func setSchedulerLaneWidth(width: UInt32) {
        // OBSOLETE: remove once downstream clients have adopted initializer
    }
}
