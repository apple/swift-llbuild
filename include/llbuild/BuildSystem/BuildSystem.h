//===- BuildSystem.h --------------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef LLBUILD_BUILDSYSTEM_BUILDSYSTEM_H
#define LLBUILD_BUILDSYSTEM_BUILDSYSTEM_H

#include "llbuild/Basic/Compiler.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/Basic/Subprocess.h"
#include "llbuild/Core/BuildEngine.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llbuild {
namespace basic {
  class ExecutionQueue;
  class FileSystem;
}

namespace buildsystem {

class BuildDescription;
class BuildDescriptionBuilder;
class BuildKey;
class BuildNode;
class BuildValue;
class Command;
class Node;
class ShellCommand;
class ShellCommandHandler;
class Tool;

enum class DiscoveredDependencyKind {
  Input = 0,
  Missing = 1,
  Output = 2,
};

bool pathIsPrefixedByPath(std::string path, std::string prefixPath);
  
class BuildSystemDelegate {
  // DO NOT COPY
  BuildSystemDelegate(const BuildSystemDelegate&)
    LLBUILD_DELETED_FUNCTION;
  void operator=(const BuildSystemDelegate&)
    LLBUILD_DELETED_FUNCTION;
  BuildSystemDelegate &operator=(BuildSystemDelegate&& rhs)
    LLBUILD_DELETED_FUNCTION;
  
public:
  /// Command status change event kinds.
  ///
  /// This must be kept in sync with core::Rule::StatusKind.
  enum class CommandStatusKind {
    /// Indicates the command is being scanned.
    IsScanning = 0,

    /// Indicates the command is up-to-date, and doesn't need to run.
    IsUpToDate = 1,

    /// Indicates the command was run, and is now complete.
    IsComplete = 2
  };

  /// Minimal token object representing the range where a diagnostic occurred.
  struct Token {
    const char* start;
    unsigned length;
  };

private:
  std::string name;
  uint32_t version;
  
public:
  /// Configure the client properties.
  ///
  /// \param name An identifier for the client system.
  ///
  /// \param version A version number to identify the schema the client is
  /// using, and changes to the schema version number will result in
  /// invalidation of all cached build results. NOTE: Currently, this is limited
  /// to a 16-bit number as an implementation detail.
  BuildSystemDelegate(StringRef name, uint32_t version)
      : name(name), version(version) {}
  virtual ~BuildSystemDelegate();

  /// Called by the build system to get the client name.
  StringRef getName() const { return name; }

  /// Called by the build system to get the current client version.
  uint32_t getVersion() const { return version; }

  /// Called by the build file loader to register the current file contents.
  //
  // FIXME: This is a total hack, and should be cleaned up.
  virtual void setFileContentsBeingParsed(StringRef buffer) = 0;

  /// Called by the build file loader to report an error.
  ///
  /// \param filename The file the error occurred in.
  ///
  /// \param at The token at which the error occurred. The token will be null if
  /// no location is associated.
  ///
  /// \param message The diagnostic message.
  virtual void error(StringRef filename,
                     const Token& at,
                     const Twine& message) = 0;

  /// Called by the build system to get a tool definition.
  ///
  /// This method is called to look for all tools, even ones which are built-in
  /// to the BuildSystem, in order to give the client an opportunity to override
  /// built-in tools.
  ///
  /// \param name The name of the tool to lookup.
  /// \returns The tool to use on success, or otherwise nil.
  virtual std::unique_ptr<Tool> lookupTool(StringRef name) = 0;

  /// Called by the build system to get create the object used to dispatch work.
  virtual std::unique_ptr<basic::ExecutionQueue> createExecutionQueue() = 0;
  
  /// Called by the build system to report a command failure.
  virtual void hadCommandFailure() = 0;

  /// Called by the build system to report that a declared command's state is
  /// changing.
  //
  // FIXME: This API is now gross, there shouldn't be one generic status changed
  // method and three individual other state change methods.
  virtual void commandStatusChanged(Command*, CommandStatusKind) = 0;

  /// Called by the build system to report that a declared command is preparing
  /// to run.
  ///
  /// This method is called before the command starts, when the system has
  /// identified that it will eventually need to run (after all of its inputs
  /// have been satisfied).
  ///
  /// The system guarantees that all such calls will be paired with a
  /// corresponding \see commandFinished() call.
  ///
  /// The system only makes this callback for commands explicitly declared in
  /// the build manifest (i.e., not for any work implicitly spawned by those
  /// commands).
  virtual void commandPreparing(Command*) = 0;

  /// Called by the build system to allow the delegate to skip a command without
  /// implicitly skipping its dependents.
  ///
  /// WARNING: Clients need to take special care when using this. Skipping
  /// commands without considering their dependencies or dependents can easily
  /// produce an inconsistent build.
  ///
  /// This method is called before the command starts, when the system has
  /// identified that it will eventually need to run (after all of its inputs
  /// have been satisfied).
  ///
  /// The system guarantees that all such calls will be paired with a
  /// corresponding \see commandFinished() call.
  virtual bool shouldCommandStart(Command*) = 0;

  /// Called by the build system to report that a declared command has started.
  ///
  /// The system guarantees that all such calls will be paired with a
  /// corresponding \see commandFinished() call.
  ///
  /// The system only makes this callback for commands explicitly declared in
  /// the build manifest (i.e., not for any work implicitly spawned by those
  /// commands).
  virtual void commandStarted(Command*) = 0;

  /// Called to report an error during the execution of a command.
  ///
  /// \param data - The error message.
  virtual void commandHadError(Command*, StringRef data) = 0;

  /// Called to report a note during the execution of a command.
  ///
  /// \param data - The note message.
  virtual void commandHadNote(Command*, StringRef data) = 0;

  /// Called to report a warning during the execution of a command.
  ///
  /// \param data - The warning message.
  virtual void commandHadWarning(Command*, StringRef data) = 0;

  /// Called by the build system to report a command has completed.
  ///
  /// \param status - The status of command (e.g. success, failure, etc).
  virtual void commandFinished(Command*, basic::ProcessStatus status) = 0;

  /// Called by the build system to report a discovered dependency.
  ///
  /// \param path - The path of the discovered dependency.
  /// \param kind - The type of the discovered dependency: input, output, missing.
  virtual void commandFoundDiscoveredDependency(Command*, StringRef path, DiscoveredDependencyKind kind) = 0;

  /// Called by the build system to report a command could not build due to
  /// missing inputs.
  virtual void commandCannotBuildOutputDueToMissingInputs(Command*,
               Node* output, ArrayRef<BuildKey> inputs) = 0;

  /// Called by the build system when a node has multiple commands that are producing it.
  /// The delegate can return one of the commands for the build system to use or return \p nullptr
  /// for the build system to treat the node as invalid.
  /// If \p nullptr is returned \p cannotBuildNodeDueToMultipleProducers is going to be called next.
  virtual Command* chooseCommandFromMultipleProducers(Node* output,
                   std::vector<Command*>) = 0;

  /// Called by the build system to report a node could not be built
  /// because multiple commands are producing it.
  virtual void cannotBuildNodeDueToMultipleProducers(Node* output,
               std::vector<Command*>) = 0;
  
  /// Called when it's been determined that a rule needs to run.
  ///
  ///  \param ruleNeedingToRun - The rule that needs to run.
  ///
  ///  \param reason - Describes why the rule needs to run. For example, because it has never run or because an input was rebuilt.
  ///
  ///  \param inputRule - If `reason` is `InputRebuilt`, the rule for the rebuilt input, else  `nullptr`.
  virtual void determinedRuleNeedsToRun(core::Rule* ruleNeedingToRun, core::Rule::RunReason reason, core::Rule* inputRule) = 0;
};

/// The BuildSystem class is used to perform builds using the native build
/// system.
class BuildSystem {
private:
  void *impl;

  // Copying is disabled.
  BuildSystem(const BuildSystem&) LLBUILD_DELETED_FUNCTION;
  void operator=(const BuildSystem&) LLBUILD_DELETED_FUNCTION;

public:
  /// Create a build system with the given delegate.
  BuildSystem(BuildSystemDelegate& delegate, std::unique_ptr<basic::FileSystem> fileSystem);
  ~BuildSystem();

  /// Return the delegate the engine was configured with.
  BuildSystemDelegate& getDelegate();

  /// Get the file system to use for access.
  basic::FileSystem& getFileSystem();

  /// @name Client API
  /// @{

  /// Load the build description from a file.
  ///
  /// \returns True on success.
  bool loadDescription(StringRef mainFilename);

  /// Load an explicit build description. from a file.
  void loadDescription(std::unique_ptr<BuildDescription> description);

  /// Build a description in memory, without a manifest.
  ///
  /// `populate` is invoked with a builder wired to this system's own build-file
  /// delegate, so tools resolve and diagnostics are reported exactly as they
  /// would when parsing the equivalent manifest.  The description is
  /// assembled by the very same code, so the two paths cannot disagree about a
  /// command's configuration or its signature.
  ///
  /// The builder is only valid for the duration of the callback. Any
  /// file-system mode it records is applied to this system once `populate`
  /// returns.
  ///
  /// \param originName A name to attribute configuration diagnostics to, in
  /// place of a manifest path.
  /// \returns True on success, or false if `populate` returned false or the
  /// description could not be finalized.
  bool loadDescription(
      llvm::function_ref<bool(BuildDescriptionBuilder&)> populate,
      StringRef originName);
  
  /// Attach (or create) the database at the given path.
  ///
  /// \returns True on success.
  bool attachDB(StringRef path, std::string* error_out);

  /// Enable low-level engine tracing into the given output file.
  ///
  /// \returns True on success.
  bool enableTracing(StringRef path, std::string* error_out);

  /// Build the named target.
  ///
  /// A build description *must* have been loaded before calling this method.
  ///
  /// \returns True on success, or false if the build was aborted (for example,
  /// if a cycle was discovered).
  bool build(StringRef target);

  /// Build a specific key directly.
  ///
  /// A build description *must* have been loaded before calling this method.
  ///
  /// \returns The result of computing the value, or nil if the build failed.
  llvm::Optional<BuildValue> build(BuildKey target);

  /// Reset mutable build state before a new build operation.
  void resetForBuild();

  /// Cancel the current build.
  void cancel();

  /// Add cancellation delegate. If the same delegate object was added before
  /// then the call is a noop.
  void addCancellationDelegate(core::CancellationDelegate* del);

  /// Remove cancellation delegate. If the delegate was not added or was
  /// previously removed the call is a noop.
  void removeCancellationDelegate(core::CancellationDelegate* del);

  static uint32_t getSchemaVersion();
  /// @}

  ShellCommandHandler* resolveShellCommandHandler(ShellCommand* command);
  
  BuildNode *lookupNode(StringRef name);
};

}
}

#endif
