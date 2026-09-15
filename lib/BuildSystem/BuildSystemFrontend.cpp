//===-- BuildSystemFrontend.cpp -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildSystemFrontend.h"

#include "llbuild/Basic/Defer.h"
#include "llbuild/Basic/ExecutionQueue.h"
#include "llbuild/Basic/FileSystem.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/Basic/PlatformUtility.h"
#include "llbuild/BuildSystem/BuildDescription.h"
#include "llbuild/BuildSystem/BuildDescriptionBuilder.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/BuildSystem/BuildValue.h"
#include "llbuild/BuildSystem/Command.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::buildsystem;

#pragma mark - BuildSystemInvocation implementation

void BuildSystemInvocation::getUsage(int optionWidth, raw_ostream& os) {
  const struct Options {
    llvm::StringRef option, helpText;
  } options[] = {
    { "--help", "show this help message and exit" },
    { "--version", "show the tool version" },
    { "-C <PATH>, --chdir <PATH>", "change directory to PATH before building" },
    { "--no-db", "disable use of a build database" },
    { "--db <PATH>", "enable building against the database at PATH" },
    { "-f <PATH>", "load the build task file at PATH" },
    { "--serial", "do not build in parallel" },
    { "--scheduler <SCHEDULER>", "set scheduler algorithm" },
    { "-j,--jobs <JOBS>", "set how many concurrent jobs (lanes) to run" },
    { "-v, --verbose", "show verbose status information" },
    { "--trace <PATH>", "trace build engine operation to PATH" },
  };
  
  for (const auto& entry: options) {
    os << "  " << llvm::format("%-*s", optionWidth, entry.option.str().c_str()) << " "
       << entry.helpText << "\n";
  }
}

void BuildSystemInvocation::parse(llvm::ArrayRef<std::string> args,
                                  llvm::SourceMgr& sourceMgr) {
  auto error = [&](const Twine &message) {
    sourceMgr.PrintMessage(llvm::SMLoc{}, llvm::SourceMgr::DK_Error, message);
    hadErrors = true;
  };

  while (!args.empty()) {
    const auto& option = args.front();
    args = args.slice(1);

    if (option == "-") {
      for (const auto& arg: args) {
        positionalArgs.push_back(arg);
      }
      break;
    }

    if (!option.empty() && option[0] != '-') {
      positionalArgs.push_back(option);
      continue;
    }
    
    if (option == "--help") {
      showUsage = true;
      break;
    } else if (option == "--version") {
      showVersion = true;
      break;
    } else if (option == "--no-db") {
      dbPath = "";
    } else if (option == "--db") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      dbPath = args[0];
      args = args.slice(1);
    } else if (option == "-C" || option == "--chdir") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      chdirPath = args[0];
      args = args.slice(1);
    } else if (option == "-f") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      buildFilePath = args[0];
      args = args.slice(1);
    } else if (option == "--serial") {
      useSerialBuild = true;
    } else if (option == "--scheduler") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      auto algorithm = args[0];
      if (algorithm == "commandNamePriority" || algorithm == "default") {
        schedulerAlgorithm = SchedulerAlgorithm::NamePriority;
      } else if (algorithm == "fifo") {
        schedulerAlgorithm = SchedulerAlgorithm::FIFO;
      } else {
        error("unknown scheduler algorithm '" + algorithm + "'");
        break;
      }
      args = args.slice(1);
    } else if (option == "-j" || option == "--jobs") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      char *end;
      schedulerLanes = ::strtol(args[0].c_str(), &end, 10);
      if (*end != '\0') {
        error("invalid argument '" + args[0] + "' to '" + option + "'");
      }
      args = args.slice(1);
    } else if (StringRef(option).startswith("-j")) {
      char *end;
      schedulerLanes = ::strtol(&option[2], &end, 10);
      if (*end != '\0') {
        error("invalid argument to '-j'");
      }
    } else if (option == "-v" || option == "--verbose") {
      showVerboseStatus = true;
    } else if (option == "--trace") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      traceFilePath = args[0];
      args = args.slice(1);
    } else {
      error("invalid option '" + option + "'");
      break;
    }
  }
}

std::string BuildSystemInvocation::formatDetectedCycle(const std::vector<core::Rule*>& cycle) {
  // Compute a description of the cycle path.
  SmallString<256> message;
  llvm::raw_svector_ostream os(message);
  os << "cycle detected while building: ";
  bool first = true;
  for (const auto* rule: cycle) {
    if (!first)
      os << " -> ";

    // Convert to a build key.
    auto key = BuildKey::fromData(rule->key);
    switch (key.getKind()) {
      case BuildKey::Kind::Unknown:
        os << "((unknown))";
        break;
      case BuildKey::Kind::Command:
        os << "command '" << key.getCommandName() << "'";
        break;
      case BuildKey::Kind::CustomTask:
        os << "custom task '" << key.getCustomTaskName() << "'";
        break;
      case BuildKey::Kind::DirectoryContents:
        os << "directory-contents '" << key.getDirectoryPath() << "'";
        break;
      case BuildKey::Kind::FilteredDirectoryContents:
        os << "filtered-directory-contents '"
        << key.getFilteredDirectoryPath() << "'";
        break;
      case BuildKey::Kind::DirectoryTreeSignature:
        os << "directory-tree-signature '"
        << key.getDirectoryTreeSignaturePath() << "'";
        break;
      case BuildKey::Kind::DirectoryTreeStructureSignature:
        os << "directory-tree-structure-signature '"
        << key.getFilteredDirectoryPath() << "'";
        break;
      case BuildKey::Kind::Node:
        os << "node '" << key.getNodeName() << "'";
        break;
      case BuildKey::Kind::Stat:
        os << "stat '" << key.getStatName() << "'";
        break;
      case BuildKey::Kind::Target:
        os << "target '" << key.getTargetName() << "'";
        break;
    }
    first = false;
  }

  return os.str().str();
}

#pragma mark - BuildSystemFrontendDelegate implementation

namespace {

struct BuildSystemFrontendImpl;
struct BuildSystemFrontendDelegateImpl;

class BuildSystemFrontendExecutionQueueDelegate
      : public ExecutionQueueDelegate {
  BuildSystemFrontendDelegateImpl& delegateImpl;
  
  bool showVerboseOutput() const;

  BuildSystem& getSystem() const;
    
public:
  BuildSystemFrontendExecutionQueueDelegate(
      BuildSystemFrontendDelegateImpl& delegateImpl)
      : delegateImpl(delegateImpl) { }
  
  virtual void queueJobStarted(JobDescriptor* command) override {
    static_cast<BuildSystemFrontendDelegate*>(&getSystem().getDelegate())->
      commandJobStarted(reinterpret_cast<Command*>(command));
  }

  virtual void queueJobFinished(JobDescriptor* command) override {
    static_cast<BuildSystemFrontendDelegate*>(&getSystem().getDelegate())->
      commandJobFinished(reinterpret_cast<Command*>(command));
  }

  virtual void processStarted(ProcessContext* command,
                              ProcessHandle handle,
                              llbuild_pid_t pid) override {
    static_cast<BuildSystemFrontendDelegate*>(&getSystem().getDelegate())->
      commandProcessStarted(
          reinterpret_cast<Command*>(command),
          BuildSystemFrontendDelegate::ProcessHandle { handle.id });
  }
  
  virtual void processHadError(ProcessContext* command, ProcessHandle handle,
                               const Twine& message) override {
    static_cast<BuildSystemFrontendDelegate*>(&getSystem().getDelegate())->
      commandProcessHadError(
          reinterpret_cast<Command*>(command),
          BuildSystemFrontendDelegate::ProcessHandle { handle.id },
          message);
  }
  
  virtual void processHadOutput(ProcessContext* command, ProcessHandle handle,
                                StringRef data) override {
    static_cast<BuildSystemFrontendDelegate*>(&getSystem().getDelegate())->
      commandProcessHadOutput(
          reinterpret_cast<Command*>(command),
          BuildSystemFrontendDelegate::ProcessHandle { handle.id },
          data);
  }

  virtual void processFinished(ProcessContext* command, ProcessHandle handle,
                               const ProcessResult& result) override {
    static_cast<BuildSystemFrontendDelegate*>(&getSystem().getDelegate())->
      commandProcessFinished(
          reinterpret_cast<Command*>(command),
          BuildSystemFrontendDelegate::ProcessHandle { handle.id },
          result);
  }
};

struct BuildSystemFrontendDelegateImpl {
  llvm::SourceMgr& sourceMgr;

  StringRef bufferBeingParsed;
  std::atomic<unsigned> numErrors{0};
  std::atomic<unsigned> numFailedCommands{0};

  BuildSystemFrontendExecutionQueueDelegate executionQueueDelegate;

  BuildSystemFrontendImpl* frontend = nullptr;

  /// The set of active command output buffers, by process handle.
  llvm::DenseMap<uintptr_t, std::vector<uint8_t>> processOutputBuffers;

  /// The lock protecting `processOutputBuffers`.
  std::mutex processOutputBuffersMutex;


public:
  BuildSystemFrontendDelegateImpl(llvm::SourceMgr& sourceMgr)
      : sourceMgr(sourceMgr), executionQueueDelegate(*this) {}
};



#pragma mark - BuildSystemFrontendImpl

struct BuildSystemFrontendImpl {
  BuildSystemFrontendDelegate& delegate;
  BuildSystemFrontendDelegateImpl* delegateImpl;
  const BuildSystemInvocation& invocation;

  std::unique_ptr<basic::FileSystem> fileSystem;
  std::unique_ptr<BuildSystem> system;

  /// Populates the description in memory, if this frontend was constructed with
  /// an in-memory source. Null means parse `invocation.buildFilePath` instead.
  ///
  /// Cleared once a description has been loaded, so it cannot outlive its use.
  std::function<bool(BuildDescriptionBuilder&)> populateDescription;

  /// The label to report in-memory description diagnostics against.
  std::string descriptionOriginName;

  /// Whether the description is built in memory rather than parsed.
  ///
  /// Tracked separately from `populateDescription`, which is cleared as soon as
  /// it has been used.
  bool inMemoryDescription = false;


public:
  BuildSystemFrontendImpl(BuildSystemFrontendDelegate& delegate,
                          BuildSystemFrontendDelegateImpl* delegateImpl,
                          const BuildSystemInvocation& invocation,
                          std::unique_ptr<basic::FileSystem> fileSystem,
                          std::function<bool(BuildDescriptionBuilder&)> populateDescription,
                          StringRef descriptionOriginName)
      : delegate(delegate), delegateImpl(delegateImpl), invocation(invocation),
        fileSystem(std::move(fileSystem)),
        populateDescription(std::move(populateDescription)),
        descriptionOriginName(descriptionOriginName) {
        assert(this->fileSystem);
        inMemoryDescription = bool(this->populateDescription);
      }


private:
  /// The status of the delegate.
  std::mutex stateMutex;
  bool cancelled{false};

public:
  void cancel() {
    std::lock_guard<std::mutex> lock(stateMutex);

    // Update the status to cancelled.
    cancelled = true;

    if (system) {
      system->cancel();
    }
  }

  void resetAfterBuild() {
    std::lock_guard<std::mutex> lock(stateMutex);
    cancelled = false;
  }

  bool initialize() {
    std::lock_guard<std::mutex> lock(stateMutex);

    if (auto result = beginInitializeLocked())
      return *result;

    if (populateDescription) {
      // Build the description in place of parsing one. The system runs the
      // callback against its own build-file delegate, so tools resolve exactly
      // as they would for the equivalent manifest.
      //
      // Moved out first: the callback is needed exactly once, and holding it
      // past that would let it outlive whatever it captured.
      auto populate = std::move(populateDescription);
      populateDescription = nullptr;
      if (!system->loadDescription(populate, descriptionOriginName)) {
        system = nullptr;
        return false;
      }
    } else if (!system->loadDescription(invocation.buildFilePath)) {
      system = nullptr;
      return false;
    }

    return finishInitializeLocked();
  }

private:
  /// Requires `stateMutex` held. Resets the per-build counters and prepares the
  /// underlying `BuildSystem`, the work every initialize entry point shares
  /// before it has a description. The result says what the caller should do
  /// next:
  ///   - a value: initialization is already resolved (the frontend was
  ///     cancelled, or the system already existed and was reset); return it.
  ///   - `llvm::None`: a fresh `BuildSystem` was created; load a description
  ///     into it and then call `finishInitializeLocked()`.
  llvm::Optional<bool> beginInitializeLocked() {
    delegateImpl->numFailedCommands = 0;
    delegateImpl->numErrors = 0;

    if (cancelled)
      return false;

    if (system) {
      // Already exists, just reset state
      system->resetForBuild();
      return true;
    }

    if (!createSystemLocked())
      return false;

    return llvm::None;
  }

  /// Requires `stateMutex` held and `system` null. Honors `--chdir` and creates
  /// the underlying `BuildSystem`, without loading a description. On failure,
  /// reports an error and leaves `system` null.
  bool createSystemLocked() {
    if (!invocation.chdirPath.empty()) {
      if (!sys::chdir(invocation.chdirPath.c_str())) {
        delegate.error(Twine("unable to honor --chdir: ") + strerror(errno));
        return false;
      }
    }

    if (!fileSystem) {
      // Catch and report client misuse of a failed build system object
      delegate.error(Twine("invalid reuse after initialization failure"));
      return false;
    }

    // Create the build system.
    system = std::make_unique<BuildSystem>(delegate, std::move(fileSystem));
    return true;
  }

  /// Requires `stateMutex` held, `system` created and a description loaded.
  /// Enables tracing and attaches the database. On failure, reports an error and
  /// clears `system`.
  bool finishInitializeLocked() {
    // Enable tracing, if requested.
    if (!invocation.traceFilePath.empty()) {
      const auto dir = llvm::sys::path::parent_path(invocation.traceFilePath);
      if (!system->getFileSystem().createDirectories(dir) &&
          !system->getFileSystem().getFileInfo(dir).isDirectory()) {
        delegate.error(Twine("unable to create tracing directory: " + dir));
        system = nullptr;
        return false;
      }

      std::string error;
      if (!system->enableTracing(invocation.traceFilePath, &error)) {
        delegate.error(Twine("unable to enable tracing: ") + error);
        system = nullptr;
        return false;
      }
    }

    // Attach the database.
    if (!invocation.dbPath.empty()) {
      // If the database path is relative, always make it relative to the input
      // file.
      SmallString<256> tmp;
      StringRef dbPath = invocation.dbPath;
      if (llvm::sys::path::is_relative(invocation.dbPath) &&
          dbPath.find("://") == StringRef::npos && !dbPath.startswith(":")) {
        // An in-memory description has no input file to be relative to.
        if (inMemoryDescription) {
          delegate.error(Twine("database path must be absolute when the build "
                               "description is built in memory: ") + dbPath);
          system = nullptr;
          return false;
        }
        llvm::sys::path::append(
            tmp, llvm::sys::path::parent_path(invocation.buildFilePath),
            invocation.dbPath);
        dbPath = tmp.str();
      }

      std::string error;
      if (!system->attachDB(dbPath, &error)) {
        delegate.error(Twine("unable to attach DB: ") + error);
        system = nullptr;
        return false;
      }
    }

    return true;
  }

public:

  bool buildNode(StringRef nodeToBuild) {
    llbuild_defer {
      resetAfterBuild();
    };

    if (!initialize()) {
      return false;
    }

    auto buildValue = system->build(BuildKey::makeNode(nodeToBuild));
    if (!buildValue.hasValue()) {
      return false;
    }

    if (!buildValue.getValue().isExistingInput()) {
      if (buildValue.getValue().isMissingInput()) {
        delegate.error((Twine("missing input '") + nodeToBuild + "' and no rule to build it"));
      }
      return false;
    }

    return delegate.getNumFailedCommands() == 0 && delegate.getNumErrors() == 0;
  }

  bool build(StringRef targetToBuild) {
    llbuild_defer {
      resetAfterBuild();
    };

    if (!initialize()) {
      return false;
    }

    // Build the target; if something unspecified failed about the build, return
    // an error.
    if (!system->build(targetToBuild))
      return false;


    // The build was successful if there were no failed commands or unspecified
    // errors, including cancellation. We explicitly include cancellation because
    // it is possible for a build to complete with no failed commands or errors,
    // yet not have done any actual work (i.e. if we are cancelled immediately).
    //
    // It is the job of the client to report a summary, if desired.
    std::lock_guard<std::mutex> lock(stateMutex);
    return !cancelled && delegate.getNumFailedCommands() == 0
        && delegate.getNumErrors() == 0;
  }
};


bool BuildSystemFrontendExecutionQueueDelegate::showVerboseOutput() const {
  return delegateImpl.frontend->invocation.showVerboseStatus;
}

BuildSystem& BuildSystemFrontendExecutionQueueDelegate::getSystem() const {
  assert(delegateImpl.frontend->system);
  return *delegateImpl.frontend->system;
}

}

BuildSystemFrontendDelegate::
BuildSystemFrontendDelegate(llvm::SourceMgr& sourceMgr,
                            StringRef name,
                            uint32_t version)
    : BuildSystemDelegate(name, version),
      impl(new BuildSystemFrontendDelegateImpl(sourceMgr))
{
  
}

BuildSystemFrontendDelegate::~BuildSystemFrontendDelegate() {
  delete static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
}

void
BuildSystemFrontendDelegate::setFileContentsBeingParsed(StringRef buffer) {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);

  impl->bufferBeingParsed = buffer;
}

llvm::SourceMgr& BuildSystemFrontendDelegate::getSourceMgr() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  return impl->sourceMgr;
}

unsigned BuildSystemFrontendDelegate::getNumErrors() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  return impl->numErrors;
}

unsigned BuildSystemFrontendDelegate::getNumFailedCommands() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);

  return impl->numFailedCommands;
}

void
BuildSystemFrontendDelegate::error(const Twine& message) {
  error("", {}, message.str());
}

void
BuildSystemFrontendDelegate::error(StringRef filename,
                                   const Token& at,
                                   const Twine& message) {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  ++impl->numErrors;

  // If we have a file and token, resolve the location and range to one
  // accessible by the source manager.
  //
  // FIXME: We shouldn't need to do this, but should switch llbuild to using
  // SourceMgr natively.
  llvm::SMLoc loc{};
  llvm::SMRange range{};
  if (!filename.empty() && at.start) {
    // FIXME: We ignore errors here, for now, this will be resolved when we move
    // to SourceMgr completely.
    auto buffer = impl->frontend->system->getFileSystem().getFileContents(filename);
    if (buffer) {
      unsigned offset = at.start - impl->bufferBeingParsed.data();
      if (offset + at.length < buffer->getBufferSize()) {
        range.Start = loc = llvm::SMLoc::getFromPointer(
            buffer->getBufferStart() + offset);
        range.End = llvm::SMLoc::getFromPointer(
            buffer->getBufferStart() + (offset + at.length));
        getSourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc{});
      }
    }
  }

  if (range.Start.isValid()) {
    getSourceMgr().PrintMessage(loc, llvm::SourceMgr::DK_Error, message, range);
  } else {
    getSourceMgr().PrintMessage(loc, llvm::SourceMgr::DK_Error, message);
  }
  fflush(stderr);
}

std::unique_ptr<ExecutionQueue>
BuildSystemFrontendDelegate::createExecutionQueue() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  auto invocation = impl->frontend->invocation;
  QualityOfService qos = invocation.qos.hasValue() ?
      invocation.qos.getValue() : getDefaultQualityOfService();
  
  if (invocation.useSerialBuild) {
    return std::unique_ptr<ExecutionQueue>(
        createLaneBasedExecutionQueue(impl->executionQueueDelegate, 1,
                                      invocation.schedulerAlgorithm, qos,
                                      invocation.environment));
  }
    
  // Get the number of CPUs to use.
  unsigned numLanes = invocation.schedulerLanes;
  if (numLanes == 0) {
    unsigned numCPUs = std::thread::hardware_concurrency();
    if (numCPUs == 0) {
      error("<unknown>", {}, "unable to detect number of CPUs");
      numLanes = 1;
    } else {
      numLanes = numCPUs;
    }
  }
    
  return std::unique_ptr<ExecutionQueue>(
      createLaneBasedExecutionQueue(impl->executionQueueDelegate, numLanes,
                                    invocation.schedulerAlgorithm, qos,
                                    invocation.environment));
}

void BuildSystemFrontendDelegate::cancel() {
  auto delegateImpl = static_cast<BuildSystemFrontendDelegateImpl*>(impl);
  delegateImpl->frontend->cancel();
}

void BuildSystemFrontendDelegate::hadCommandFailure() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  // Increment the failed command count.
  ++impl->numFailedCommands;
}

void BuildSystemFrontendDelegate::commandStatusChanged(Command*,
                                                       CommandStatusKind) {
}

void BuildSystemFrontendDelegate::commandPreparing(Command*) {
}

bool BuildSystemFrontendDelegate::shouldCommandStart(Command*) {
  return true;
}

void BuildSystemFrontendDelegate::commandStarted(Command* command) {
  // Don't report status if opted out by the command.
  if (!command->shouldShowStatus()) {
    return;
  }

  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);

  // Log the command.
  //
  // FIXME: Design the logging and status output APIs.
  SmallString<64> description;
  if (impl->frontend->invocation.showVerboseStatus) {
    command->getVerboseDescription(description);
  } else {
    command->getShortDescription(description);

    // If the short description is empty, always show the verbose one.
    if (description.empty()) {
      command->getVerboseDescription(description);
    }
  }
  fprintf(stdout, "%s\n", description.c_str());
  fflush(stdout);
}

void BuildSystemFrontendDelegate::commandHadError(Command* command, StringRef data) {
  fwrite(data.data(), data.size(), 1, stderr);
  fflush(stderr);
}

void BuildSystemFrontendDelegate::commandHadNote(Command* command, StringRef data) {
  fwrite(data.data(), data.size(), 1, stdout);
  fflush(stdout);
}

void BuildSystemFrontendDelegate::commandHadWarning(Command* command, StringRef data) {
  fwrite(data.data(), data.size(), 1, stdout);
  fflush(stdout);
}

void BuildSystemFrontendDelegate::commandFinished(Command*, ProcessStatus) {
}

void BuildSystemFrontendDelegate::commandFoundDiscoveredDependency(Command*, StringRef, DiscoveredDependencyKind) {
}

void BuildSystemFrontendDelegate::commandCannotBuildOutputDueToMissingInputs(
     Command * command, Node *output, ArrayRef<BuildKey> inputs) {
  std::string message;
  llvm::raw_string_ostream messageStream(message);

  messageStream << "cannot build '";
  messageStream << output->getName().str();
  messageStream << "' due to missing inputs: ";

  for (size_t i = 0; i < inputs.size(); ++i) {
    if (i > 0) {
      messageStream << ", ";
    }
    messageStream << "'" << (inputs[i].isNode() ? inputs[i].getNodeName().str() : inputs[i].getKeyData().str()) << "'";
  }

  messageStream.flush();
  fwrite(message.data(), message.size(), 1, stderr);
  fflush(stderr);
}

Command* BuildSystemFrontendDelegate::chooseCommandFromMultipleProducers(
     Node *output, std::vector<Command*> commands) {
  return nullptr;
}

void BuildSystemFrontendDelegate::cannotBuildNodeDueToMultipleProducers(
     Node *output, std::vector<Command*> commands) {
  std::string message;
  llvm::raw_string_ostream messageStream(message);

  messageStream << "unable to build node: '";
  messageStream << output->getName();
  messageStream << "' (node is produced by multiple commands: '";

  for (Command* cmd : commands) {
    if (cmd != *commands.begin()) {
      messageStream << ", ";
    }
    messageStream << "'" << cmd->getName() << "'";
  }

  messageStream.flush();
  fwrite(message.data(), message.size(), 1, stderr);
  fflush(stderr);
}

void BuildSystemFrontendDelegate::commandJobStarted(Command*) {
}

void BuildSystemFrontendDelegate::commandJobFinished(Command*) {
}

void BuildSystemFrontendDelegate::commandProcessStarted(Command*,
                                                        ProcessHandle) {
}

void BuildSystemFrontendDelegate::determinedRuleNeedsToRun(core::Rule* ruleNeedingToRun, core::Rule::RunReason reason, core::Rule* inputRule) {}

bool BuildSystemFrontendDelegate::
shouldResolveCycle(const std::vector<core::Rule*>& items,
                   core::Rule* candidateRule,
                   core::Rule::CycleAction action) {
  return false;
}

void BuildSystemFrontendDelegate::
commandProcessHadError(Command* command, ProcessHandle handle,
                       const Twine& message) {
  SmallString<256> buffer;
  auto str = message.toStringRef(buffer);
  
  // FIXME: Design the logging and status output APIs.
  fwrite(str.data(), str.size(), 1, stderr);
  fputc('\n', stderr);
  fflush(stderr);
}
  
void BuildSystemFrontendDelegate::
commandProcessHadOutput(Command* command, ProcessHandle handle,
                        StringRef data) {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  std::unique_lock<std::mutex> lock(impl->processOutputBuffersMutex);

  // Append to the output buffer.
  auto& buffer = impl->processOutputBuffers[handle.id];
  buffer.insert(buffer.end(), data.begin(), data.end());
}

void BuildSystemFrontendDelegate::
commandProcessFinished(Command*, ProcessHandle handle,
                       const ProcessResult& result) {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  std::unique_lock<std::mutex> lock(impl->processOutputBuffersMutex);

  // If there was an output buffer, flush it.
  auto it = impl->processOutputBuffers.find(handle.id);
  if (it == impl->processOutputBuffers.end())
    return;

  fwrite(it->second.data(), it->second.size(), 1, stdout);
  fflush(stdout);

  impl->processOutputBuffers.erase(it);  
}


#pragma mark - BuildSystemFrontend

BuildSystemFrontend::
BuildSystemFrontend(BuildSystemFrontendDelegate& delegate,
                    const BuildSystemInvocation& invocation,
                    std::unique_ptr<basic::FileSystem> fileSystem)
  : impl(new BuildSystemFrontendImpl(delegate,
                                     static_cast<BuildSystemFrontendDelegateImpl*>(delegate.impl),
                                     invocation,
                                     std::move(fileSystem),
                                     nullptr,
                                     ""))
{
  auto implPtr = static_cast<BuildSystemFrontendImpl*>(impl);
  implPtr->delegateImpl->frontend = implPtr;
}

BuildSystemFrontend::
BuildSystemFrontend(BuildSystemFrontendDelegate& delegate,
                    const BuildSystemInvocation& invocation,
                    std::unique_ptr<basic::FileSystem> fileSystem,
                    std::function<bool(BuildDescriptionBuilder&)> populateDescription,
                    StringRef descriptionOriginName)
  : impl(new BuildSystemFrontendImpl(delegate,
                                     static_cast<BuildSystemFrontendDelegateImpl*>(delegate.impl),
                                     invocation,
                                     std::move(fileSystem),
                                     std::move(populateDescription),
                                     descriptionOriginName))
{
  assert(static_cast<BuildSystemFrontendImpl*>(impl)->populateDescription &&
         "in-memory description requires a populate callback");
  auto implPtr = static_cast<BuildSystemFrontendImpl*>(impl);
  implPtr->delegateImpl->frontend = implPtr;
}

BuildSystemFrontend::~BuildSystemFrontend() {
  delete static_cast<BuildSystemFrontendImpl*>(impl);
  impl = nullptr;
}

bool BuildSystemFrontend::initialize() {
  return static_cast<BuildSystemFrontendImpl*>(impl)->initialize();
}

bool BuildSystemFrontend::build(StringRef targetToBuild) {
  return static_cast<BuildSystemFrontendImpl*>(impl)->build(targetToBuild);
}

bool BuildSystemFrontend::buildNode(StringRef nodeToBuild) {
  return static_cast<BuildSystemFrontendImpl*>(impl)->buildNode(nodeToBuild);
}
