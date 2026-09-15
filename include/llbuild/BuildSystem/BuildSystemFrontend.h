//===- BuildSystemFrontend.h ------------------------------------*- C++ -*-===//
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

#ifndef LLBUILD_BUILDSYSTEM_BUILDSYSTEMFRONTEND_H
#define LLBUILD_BUILDSYSTEM_BUILDSYSTEMFRONTEND_H

#include "llbuild/Basic/ExecutionQueue.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/BuildSystem/BuildSystem.h"
#include "llbuild/BuildSystem/BuildNode.h"
#include "llbuild/Core/BuildEngine.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Optional.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace llvm {

class SourceMgr;

}

namespace llbuild {
namespace basic {

class FileSystem;

}

namespace buildsystem {

class BuildDescriptionBuilder;
class BuildSystemFrontendDelegate;
class BuildSystemInvocation;
enum class CommandResult;
enum class DiscoveredDependencyKind;

/// This provides a standard "frontend" to the build system features, for use in
/// building bespoke build systems that can still take advantage of desirable
/// shared behavior.
///
/// The frontend glues together various parts of the build system functionality
/// to provide:
///   o Support for common command line options.
///   o Support for parallel, persistent builds.
///   o Support for command line diagnostics and status reporting.
class BuildSystemFrontend {
  void* impl;

public:
  /// Construct a frontend which loads its build description by parsing the
  /// build file named by the `invocation`'s `buildFilePath`.
  BuildSystemFrontend(BuildSystemFrontendDelegate& delegate,
                      const BuildSystemInvocation& invocation,
                      std::unique_ptr<basic::FileSystem> fileSystem);

  /// Construct a frontend which constructs its build description in memory,
  /// rather than parsing one from disk.
  ///
  /// \param populateDescription Invoked once, during initialization, with a
  /// builder wired to the underlying `BuildSystem`; return false from it to fail
  /// initialization. It is retained until the description has been built.
  ///
  /// \param descriptionOriginName A label for the description, used as the
  /// location of any diagnostics it produces. No file is read or written.
  ///
  /// The `invocation`'s `buildFilePath` is unused in this mode. Because a
  /// relative `dbPath` is otherwise resolved against the build file's
  /// directory, it must be absolute here.
  BuildSystemFrontend(BuildSystemFrontendDelegate& delegate,
                      const BuildSystemInvocation& invocation,
                      std::unique_ptr<basic::FileSystem> fileSystem,
                      std::function<bool(BuildDescriptionBuilder&)> populateDescription,
                      StringRef descriptionOriginName);
  ~BuildSystemFrontend();

  /// @name Client API
  /// @{

  /// Initialize the build system.
  ///
  /// This will obtain the build description from the source the frontend was
  /// constructed with, either parsing the build file, or invoking the populate
  /// callback, and apply all of the command line options to construct an
  /// appropriate underlying `BuildSystem` for use by subsequent build calls.
  ///
  /// `build` and `buildNode` call this themselves, so it only needs to be
  /// called directly in order to do the work separately: to measure it, or to
  /// fail before starting a build. It is a no-op once initialized.
  ///
  /// \returns True on success, or false if there were errors. If initialization
  /// fails, the frontend is in an indeterminant state and should not be reused.
  bool initialize();

  /// Build the named target using the specified invocation parameters.
  ///
  /// \returns True on success, or false if there were errors.
  bool build(StringRef targetToBuild);

  /// Build a single node using the specified invocation parameters.
  ///
  /// \returns True on success, or false if there were errors.
  bool buildNode(StringRef nodeToBuild);

  /// @}
};

/// The frontend-specific delegate, which provides some shared behaviors.
///
/// NOTE: This class is thread-safe. Subclasses should be careful to retain that
/// safety in overriden methods.
class BuildSystemFrontendDelegate : public BuildSystemDelegate {
  friend class BuildSystemFrontend;

public:
  /// Handle used to communicate information about a launched process.
  struct ProcessHandle {
    /// Opaque ID.
    uint64_t id;
  };

private:
  void* impl;

  /// Default implementation, cannot be overriden by subclasses.
  virtual void setFileContentsBeingParsed(StringRef buffer) override;

public:
  /// Create a frontend delegate.
  ///
  /// \param sourceMgr The source manager to use for reporting diagnostics.
  /// \param name The name of build system client.
  /// \param version The version of the build system client.
  BuildSystemFrontendDelegate(llvm::SourceMgr& sourceMgr,
                              StringRef name,
                              uint32_t version);
  virtual ~BuildSystemFrontendDelegate();

  /// Called by the build system to get a tool definition, must be provided by
  /// subclasses.
  virtual std::unique_ptr<Tool> lookupTool(StringRef name) override = 0;

  /// Provides an appropriate execution queue based on the invocation options.
  virtual std::unique_ptr<basic::ExecutionQueue> createExecutionQueue() override;

  /// Cancels the current build.
  virtual void cancel();

  /// Provides a default handler.
  ///
  /// Subclass should call this method if overridden.
  virtual void hadCommandFailure() override;

  /// @name Frontend-specific APIs
  /// @{

  /// Report a non-file specific error message.
  void error(const Twine& message);

  /// Provides a default error implementation which will delegate to the
  /// provided source manager.
  virtual void error(StringRef filename, const Token& at,
                     const Twine& message) override;

  /// @}

  /// @name Status Reporting APIs
  ///
  /// The frontend provides default implementations of these methods which
  /// report the status to stdout. Clients should override if they wish to
  /// direct the status elsewhere.
  ///
  /// @{

  /// Called by the build system to report that a declared command's state is
  /// changing.
  virtual void commandStatusChanged(Command*, CommandStatusKind) override;

  /// Called by the build system to report that a declared command is preparing
  /// to run.
  ///
  /// The system guarantees that all such calls will be paired with a
  /// corresponding \see commandFinished() call.
  virtual void commandPreparing(Command*) override;

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
  virtual bool shouldCommandStart(Command*) override;

  /// Called by the build system to report that a declared command has started.
  ///
  /// The system guarantees that all such calls will be paired with a
  /// corresponding \see commandFinished() call.
  virtual void commandStarted(Command*) override;

  /// Called to report an error during the execution of a command.
  ///
  /// \param data - The error message.
  virtual void commandHadError(Command*, StringRef data) override;

  /// Called to report a note during the execution of a command.
  ///
  /// \param data - The note message.
  virtual void commandHadNote(Command*, StringRef data) override;

  /// Called to report a warning during the execution of a command.
  ///
  /// \param data - The warning message.
  virtual void commandHadWarning(Command*, StringRef data) override;

  /// Called by the build system to report a command has completed.
  ///
  /// \param result - The result of command (e.g. success, failure, etc).
  virtual void commandFinished(Command*, basic::ProcessStatus result) override;

  /// Called by the build system to report a discovered dependency.
  ///
  /// \param path - The path of the discovered dependency.
  /// \param kind - The type of the discovered dependency: input, output, missing.
  virtual void commandFoundDiscoveredDependency(Command*, StringRef path, DiscoveredDependencyKind kind) override;

  /// Called by the build system to report a command could not build due to
  /// missing inputs.
  virtual void commandCannotBuildOutputDueToMissingInputs(Command*,
               Node* output, ArrayRef<BuildKey> inputs) override;

  /// Called by the build system when a node has multiple commands that are producing it.
  /// The delegate can return one of the commands for the build system to use or return \p nullptr
  /// for the build system to treat the node as invalid.
  /// If \p nullptr is returned \p cannotBuildNodeDueToMultipleProducers is going to be called next.
  virtual Command* chooseCommandFromMultipleProducers(Node* output,
                   std::vector<Command*>) override;

  /// Called by the build system to report a node could not be built
  /// because multiple commands are producing it.
  virtual void cannotBuildNodeDueToMultipleProducers(Node* output,
               std::vector<Command*>) override;

  /// Called when a command's job has been started.
  ///
  /// The system guarantees that any commandStart() call will be paired with
  /// exactly one \see commandFinished() call.
  //
  // FIXME: We may eventually want to allow the individual job to provide some
  // additional context here, for complex commands.
  //
  // FIXME: Design a way to communicate the "lane" here, for use in "super
  // console" like UIs.
  virtual void commandJobStarted(Command*);

  /// Called when a command's job has been finished.
  virtual void commandJobFinished(Command*);

  /// Called when a command's job has started executing an external process.
  ///
  /// The system guarantees that any commandProcessStarted() call will be paired
  /// with exactly one \see commandProcessFinished() call.
  ///
  /// \param handle - A unique handle used in subsequent delegate calls to
  /// identify the process. This handle should only be used to associate
  /// different status calls relating to the same process. It is only guaranteed
  /// to be unique from when it has been provided here to when it has been
  /// provided to the \see commandProcessFinished() call.
  virtual void commandProcessStarted(Command*, ProcessHandle handle);

  /// Called to report an error in the management of a command process.
  ///
  /// \param handle - The process handle.
  /// \param message - The error message.
  //
  // FIXME: Need to move to more structured error handling.
  virtual void commandProcessHadError(Command*, ProcessHandle handle,
                                       const Twine& message);

  /// Called to report a command processes' (merged) standard output and error.
  ///
  /// \param handle - The process handle.
  /// \param data - The process output.
  virtual void commandProcessHadOutput(Command*, ProcessHandle handle,
                                       StringRef data);

  /// Called when a command's job has finished executing an external process.
  ///
  /// \param handle - The handle used to identify the process. This handle will
  /// become invalid as soon as the client returns from this API call.
  ///
  /// \param result - Whether the process succeeded, failed or was cancelled.
  //
  // FIXME: Need to include additional information on the status here, e.g., the
  // signal status, and the process output (if buffering).
  virtual void commandProcessFinished(Command*, ProcessHandle handle,
                                      const basic::ProcessResult& result);

  /// Called when it's been determined that a rule needs to run.
  ///
  ///  \param ruleNeedingToRun - The rule that needs to run.
  ///
  ///  \param reason - Describes why the rule needs to run. For example, because it has never run or because an input was rebuilt.
  ///
  ///  \param inputRule - If `reason` is `InputRebuilt`, the rule for the rebuilt input, else  `nullptr`.
  virtual void determinedRuleNeedsToRun(core::Rule* ruleNeedingToRun, core::Rule::RunReason reason, core::Rule* inputRule) override;

  /// Called when a cycle is detected by the build engine and it cannot make
  /// forward progress.
  ///
  /// \param items The ordered list of items comprising the cycle, starting from
  /// the node which was requested to build and ending with the first node in
  /// the cycle (i.e., the node participating in the cycle will appear twice).
  virtual void cycleDetected(const std::vector<core::Rule*>& items) = 0;

  /// Called when a cycle is detected by the build engine to check if it should
  /// attempt to resolve the cycle and continue
  ///
  /// \param items The ordered list of items comprising the cycle, starting from
  /// the node which was requested to build and ending with the first node in
  /// the cycle (i.e., the node participating in the cycle will appear twice).
  /// \param candidateRule The rule the engine will use to attempt to break the
  /// cycle.
  /// \param action The action the engine will take on the candidateRule.
  /// \returns True if the engine should attempt to resolve the cycle, false
  /// otherwise. Resolution is attempted by either forcing items to be built, or
  /// supplying a previously built result to a node in the cycle. The latter
  /// action may yield unexpected results and thus this should be opted into
  /// with care.
  virtual bool shouldResolveCycle(const std::vector<core::Rule*>& items,
                                  core::Rule* candidateRule,
                                  core::Rule::CycleAction action);
  /// @}

  /// @name Accessors
  /// @{

  llvm::SourceMgr& getSourceMgr();

  /// Get the number of reported errors.
  unsigned getNumErrors();

  /// Get the number of failed commands.
  unsigned getNumFailedCommands();

  /// @}
};


/// This class wraps the common options which are used by the frontend.
class BuildSystemInvocation {
public:
  /// Whether the command usage should be printed.
  bool showUsage = false;

  /// Whether the command version should be printed.
  bool showVersion = false;

  /// Whether to show verbose output.
  bool showVerboseStatus = false;

  /// Whether to use a serial build.
  bool useSerialBuild = false;

  /// The path of the database file to use, if any.
  std::string dbPath = "build.db";

  /// The path of a directory to change into before anything else, if any.
  std::string chdirPath = "";

  /// The path of the build file to use.
  std::string buildFilePath = "build.llbuild";

  /// The path of the build trace output file to use, if any.
  std::string traceFilePath = "";

  basic::SchedulerAlgorithm schedulerAlgorithm =
      basic::SchedulerAlgorithm::NamePriority;

  uint32_t schedulerLanes = 0;

  /// The Quality of Service class to use. If not set the global default setting will be used.
  Optional<basic::QualityOfService> qos;

  /// The base environment to use when executing subprocesses.
  ///
  /// The format is expected to match that of `::main()`, i.e. a null-terminated
  /// array of pointers to null terminated C strings.
  ///
  /// If empty, the environment of the calling process will be used.
  const char* const* environment = nullptr;

  /// The positional arguments.
  std::vector<std::string> positionalArgs;

  /// Whether there were any parsing errors.
  bool hadErrors = false;

public:
  /// Get the appropriate "usage" text to use for the built in arguments.
  static void getUsage(int optionWidth, raw_ostream& os);

  /// Parse the invocation parameters from the given arguments.
  ///
  /// \param sourceMgr The source manager to use for diagnostics.
  void parse(ArrayRef<std::string> args, llvm::SourceMgr& sourceMgr);

  /// Provides a default string representation for a cycle detected by the build engine.
  static std::string formatDetectedCycle(const std::vector<core::Rule*>& cycle);
};

}
}

#endif
