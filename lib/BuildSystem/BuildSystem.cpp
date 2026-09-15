//===-- BuildSystem.cpp ---------------------------------------------------===//
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

#include "llbuild/BuildSystem/BuildSystem.h"
#include "llbuild/BuildSystem/BuildSystemExtensions.h"
#include "llbuild/BuildSystem/BuildSystemFrontend.h"
#include "llbuild/BuildSystem/BuildSystemHandlers.h"

#include "llbuild/Basic/CrossPlatformCompatibility.h"
#include "llbuild/Basic/ExecutionQueue.h"
#include "llbuild/Basic/FileInfo.h"
#include "llbuild/Basic/FileSystem.h"
#include "llbuild/Basic/Hashing.h"
#include "llbuild/Basic/JSON.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/Basic/PlatformUtility.h"
#include "llbuild/Basic/ShellUtility.h"
#include "llbuild/BuildSystem/BuildDescriptionBuilder.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/BuildSystem/BuildNode.h"
#include "llbuild/BuildSystem/BuildValue.h"
#include "llbuild/BuildSystem/ExternalCommand.h"
#include "llbuild/BuildSystem/ShellCommand.h"
#include "llbuild/BuildSystem/Tool.h"
#include "llbuild/Core/BuildDB.h"
#include "llbuild/Core/BuildEngine.h"
#include "llbuild/Core/DependencyInfoParser.h"
#include "llbuild/Core/MakefileDepsParser.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <mutex>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <Shlwapi.h>
#else
#include <limits.h>
#include <fnmatch.h>
#include <unistd.h>
#endif

using namespace llvm;
using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::core;
using namespace llbuild::buildsystem;

/// The extension manager singleton.
static BuildSystemExtensionManager extensionManager{};

BuildSystemDelegate::~BuildSystemDelegate() {}

#pragma mark - BuildSystem implementation

namespace {

class BuildSystemImpl;

/// The delegate used to load the build file for use by a build system.
class BuildSystemFileDelegate : public BuildFileDelegate {
  BuildSystemImpl& system;

  /// FIXME: It would be nice to have a StringSet.
  llvm::StringMap<bool> internedStrings;

public:
  BuildSystemFileDelegate(BuildSystemImpl& system)
      : BuildFileDelegate(), system(system) {}

  BuildSystemDelegate& getSystemDelegate();

  /// @name Delegate Implementation
  /// @{

  virtual StringRef getInternedString(StringRef value) override {
    auto entry = internedStrings.insert(std::make_pair(value, true));
    return entry.first->getKey();
  }

  virtual FileSystem& getFileSystem() override;
  
  virtual void setFileContentsBeingParsed(StringRef buffer) override;
  
  virtual void error(StringRef filename,
                     const BuildFileToken& at,
                     const Twine& message) override;

  virtual void cannotLoadDueToMultipleProducers(Node *output,
                                                std::vector<Command*> commands) override;

  virtual bool configureClient(const ConfigureContext&, StringRef name,
                               uint32_t version,
                               const property_list_type& properties) override;

  virtual std::unique_ptr<Tool> lookupTool(StringRef name) override;

  virtual void loadedTarget(StringRef name,
                            const Target& target) override;

  virtual void loadedDefaultTarget(StringRef target) override;

  virtual void loadedCommand(StringRef name,
                             const Command& target) override;

  virtual std::unique_ptr<Node> createNode(StringRef name,
                                           bool isImplicit=false) override;

  /// @}
};

/// The delegate used to build a loaded build file.
class BuildSystemEngineDelegate : public BuildEngineDelegate {
  BuildSystemImpl& system;
  
  // FIXME: This is an inefficent map, the string is duplicated.
  std::unordered_map<std::string, std::unique_ptr<BuildNode>> dynamicNodes;

  // FIXME: This is an inefficent map, the string is duplicated.
  std::unordered_map<std::string, std::unique_ptr<StatNode>> dynamicStatNodes;

  /// The custom tasks which are owned by the build system.
  std::vector<std::unique_ptr<Command>> customTasks;

  const BuildDescription& getBuildDescription() const;

  virtual std::unique_ptr<Rule> lookupRule(const KeyType& keyData) override;
  virtual void determinedRuleNeedsToRun(Rule* ruleNeedingToRun, Rule::RunReason reason, Rule* inputRule) override;
  virtual bool shouldResolveCycle(const std::vector<Rule*>& items,
                                  Rule* candidateRule,
                                  Rule::CycleAction action) override;
  virtual void cycleDetected(const std::vector<Rule*>& items) override;
  virtual void error(const Twine& message) override;

  std::unique_ptr<basic::ExecutionQueue> createExecutionQueue() override;

public:
  BuildSystemEngineDelegate(BuildSystemImpl& system) : system(system) {}

  BuildSystemImpl& getBuildSystem() {
    return system;
  }
  
  BuildNode *lookupNode(StringRef name);
};

class BuildSystemImpl {
public:
  /// The internal schema version.
  ///
  /// Version History:
  /// * 9: Added filters to Directory* BuildKeys
  /// * 8: Added DirectoryTreeStructureSignature to BuildValue
  /// * 7: Added StaleFileRemoval to BuildValue
  /// * 6: Added DirectoryContents to BuildKey
  /// * 5: Switch BuildValue to be BinaryCoding based
  /// * 4: Pre-history
  static const uint32_t internalSchemaVersion = 9;

private:
  BuildSystem& buildSystem;

  /// The delegate the BuildSystem was configured with.
  BuildSystemDelegate& delegate;

  /// The file system used by the build system
  std::unique_ptr<basic::FileSystem> fileSystem;

  /// The name of the main input file.
  std::string mainFilename;

  /// The delegate used for the loading the build file.
  BuildSystemFileDelegate fileDelegate;

  /// The build description, once loaded.
  std::unique_ptr<BuildDescription> buildDescription;

  /// The delegate used for building the file contents.
  BuildSystemEngineDelegate engineDelegate;

  /// The build engine.
  BuildEngine buildEngine;

  /// Flag indicating if the build has been aborted.
  bool buildWasAborted = false;

  /// Cache of instantiated shell command handlers.
  llvm::StringMap<std::unique_ptr<ShellCommandHandler>> shellHandlers;

public:
  ShellCommandHandler*
  resolveShellCommandHandler(ShellCommand* command) {
    // Ignore empty commands.
    if (command->getArgs().empty()) { return nullptr; }

    // Check the cache.
    auto toolPath = command->getArgs()[0];
    auto it = shellHandlers.find(toolPath);
    if (it != shellHandlers.end()) return it->second.get();

    // If missing, check for an extension which can provide it.
    auto* extension = extensionManager.lookupByCommandPath(toolPath);
    if (!extension) {
      shellHandlers[toolPath] = nullptr; // Negative caching
      return nullptr;
    }

    auto handler = extension->createShellCommandHandler(toolPath);
    auto *result = handler.get();
    shellHandlers[toolPath] = std::move(handler);

    return result;
  }
  
  /// @}

public:
  BuildSystemImpl(class BuildSystem& buildSystem,
                  BuildSystemDelegate& delegate,
                  std::unique_ptr<basic::FileSystem> fileSystem)
      : buildSystem(buildSystem), delegate(delegate),
        fileSystem(std::move(fileSystem)),
        fileDelegate(*this), engineDelegate(*this), buildEngine(engineDelegate) {}

  BuildSystem& getBuildSystem() {
    return buildSystem;
  }

  BuildSystemDelegate& getDelegate() {
    return delegate;
  }

  basic::FileSystem& getFileSystem() {
    return *fileSystem;
  }

  // FIXME: We should eliminate this, it isn't well formed when loading
  // descriptions not from a file. We currently only use that for unit testing,
  // though.
  StringRef getMainFilename() {
    return mainFilename;
  }

  const BuildDescription& getBuildDescription() const {
    assert(buildDescription);
    return *buildDescription;
  }

  void error(StringRef filename, const Twine& message) {
    getDelegate().error(filename, {}, message);
  }

  void error(StringRef filename, const BuildSystemDelegate::Token& at,
             const Twine& message) {
    getDelegate().error(filename, at, message);
  }
  
  BuildNode* lookupNode(StringRef name);

  std::unique_ptr<BuildNode> createNode(StringRef name,
                                        bool isImplicit);

  uint32_t getMergedSchemaVersion() {
    // FIXME: Find a cleaner strategy for merging the internal schema version
    // with that from the client.
    auto clientVersion = delegate.getVersion();
    assert(clientVersion <= (1 << 16) && "unsupported client version");
    return internalSchemaVersion + (clientVersion << 16);
  }

  void configureFileSystem(FileSystemMode mode) {
    if (mode == FileSystemMode::DeviceAgnostic) {
      std::unique_ptr<basic::FileSystem> newFS(
          new DeviceAgnosticFileSystem(std::move(fileSystem)));
      fileSystem.swap(newFS);
    } else if (mode == FileSystemMode::ChecksumOnly) {
      std::unique_ptr<basic::FileSystem> newFS(
          new ChecksumOnlyFileSystem(std::move(fileSystem)));
      fileSystem.swap(newFS);
    }
  }
  
  /// @name Client API
  /// @{

  bool loadDescription(StringRef filename) {
    this->mainFilename = filename;

    auto description = BuildFile(filename, fileDelegate).load();
    if (!description) {
      error(getMainFilename(), "unable to load build file");
      return false;
    }

    buildDescription = std::move(description);
    return true;
  }

  void loadDescription(std::unique_ptr<BuildDescription> description) {
    buildDescription = std::move(description);
  }

  bool loadDescription(
      llvm::function_ref<bool(BuildDescriptionBuilder&)> populate,
      StringRef originName) {
    this->mainFilename = originName;

    // The builder is wired to the same delegate the manifest loader uses, so
    // tools resolve and diagnostics are reported identically.
    BuildDescriptionBuilder builder(fileDelegate, originName);
    if (!populate(builder)) {
      error(getMainFilename(), "unable to build in-memory build description");
      return false;
    }

    auto description = builder.finalize();
    if (!description) {
      error(getMainFilename(), "unable to build in-memory build description");
      return false;
    }

    // The client section has no in-memory analogue, so the one part of it that
    // configures the system rather than the graph has to be applied by hand.
    configureFileSystem(builder.getFileSystemMode());

    buildDescription = std::move(description);
    return true;
  }

  bool attachDB(StringRef filename, std::string* error_out) {
    // FIXME: How do we pass the client schema version here, if we haven't
    // loaded the file yet.
    std::unique_ptr<core::BuildDB> db(
                                      core::createSQLiteBuildDB(filename, getMergedSchemaVersion(), /* recreateUnmatchedVersion = */ true, error_out));
    if (!db)
      return false;

    return buildEngine.attachDB(std::move(db), error_out);
  }

  bool enableTracing(StringRef filename, std::string* error_out) {
    return buildEngine.enableTracing(filename, error_out);
  }

  /// Build the given key, and return the result and an indication of success.
  llvm::Optional<BuildValue> build(BuildKey key);
  
  bool build(StringRef target);

  void setBuildWasAborted(bool value) {
    buildWasAborted = value;
  }

  void resetForBuild() {
    buildEngine.resetForBuild();
  }

  /// Cancel the running build.
  void cancel() {
    buildEngine.cancelBuild();
  }

  /// Check if the build has been cancelled.
  bool isCancelled() {
    return buildEngine.isCancelled();
  }

  void addCancellationDelegate(CancellationDelegate* del) {
    buildEngine.addCancellationDelegate(del);
  }

  void removeCancellationDelegate(CancellationDelegate* del) {
    buildEngine.removeCancellationDelegate(del);
  }

  /// @}
};

#pragma mark - BuildSystem engine integration

std::unique_ptr<basic::ExecutionQueue> BuildSystemEngineDelegate::createExecutionQueue() {
  return system.getDelegate().createExecutionQueue();
}


#pragma mark - Task implementations

static BuildSystemImpl& getBuildSystem(TaskInterface ti) {
  return static_cast<BuildSystemEngineDelegate*>(ti.delegate())->getBuildSystem();
}

static BuildSystemImpl& getBuildSystem(BuildEngine& engine) {
  return static_cast<BuildSystemEngineDelegate*>(engine.getDelegate())->getBuildSystem();
}


FileSystem& BuildSystemFileDelegate::getFileSystem() {
  return system.getFileSystem();
}

  
/// This is the task used to "build" a target, it translates between the request
/// for building a target key and the requests for all of its nodes.
class TargetTask : public Task {
  Target& target;
  
  // Build specific data.
  //
  // FIXME: We should probably factor this out somewhere else, so we can enforce
  // it is never used when initialized incorrectly.

  /// If there are any elements, the command had missing input nodes (this implies
  /// ShouldSkip is true).
  SmallPtrSet<Node*, 1> missingInputNodes;

  virtual void start(TaskInterface ti) override {
    // Request all of the necessary system tasks.
    unsigned id = 0;
    for (auto it = target.getNodes().begin(),
           ie = target.getNodes().end(); it != ie; ++it, ++id) {
      ti.request(BuildKey::makeNode(*it).toData(), id);
    }
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
    // Do nothing.
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& valueData) override {
    // Do nothing.
    auto value = BuildValue::fromData(valueData);

    if (value.isMissingInput()) {
      missingInputNodes.insert(target.getNodes()[inputID]);
    }
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    // If the build should cancel, do nothing.
    if (ti.isCancelled()) {
      ti.complete(BuildValue::makeSkippedCommand().toData());
      return;
    }

    if (!missingInputNodes.empty()) {
      std::string inputs;
      raw_string_ostream inputsStream(inputs);
      for (Node* missingInputNode : missingInputNodes) {
        if (missingInputNode != *missingInputNodes.begin()) {
          inputsStream << ", ";
        }
        inputsStream << "'" << missingInputNode->getName() << "'";
      }
      inputsStream.flush();

      // FIXME: Design the logging and status output APIs.
      auto& system = getBuildSystem(ti);
      system.error(system.getMainFilename(),
                   (Twine("cannot build target '") + target.getName() +
                    "' due to missing inputs: " + inputs));
      
      // Report the command failure.
      system.getDelegate().hadCommandFailure();
    }
    
    // Complete the task immediately.
    ti.complete(BuildValue::makeTarget().toData());
  }

public:
  TargetTask(Target& target) : target(target) {}

  static bool isResultValid(BuildEngine&, Target&, const BuildValue&) {
    // Always treat target tasks as invalid.
    return false;
  }
};


/// This is the task to "build" a file node which represents pure raw input to
/// the system.
class FileInputNodeTask : public Task {
  BuildNode& node;

  virtual void start(TaskInterface) override {
    assert(node.getProducers().empty());
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& value) override {
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    // FIXME: We should do this work in the background.

    // Get the information on the file.
    //
    // FIXME: This needs to delegate, since we want to have a notion of
    // different node types.
    assert(!node.isVirtual());
    auto info = node.getFileInfo(
        getBuildSystem(ti).getFileSystem());
    if (info.isMissing()) {
      ti.complete(BuildValue::makeMissingInput().toData());
      return;
    }

    ti.complete(BuildValue::makeExistingInput(info).toData());
  }

public:
  FileInputNodeTask(BuildNode& node) : node(node) {
    assert(!node.isVirtual());
  }

  static bool isResultValid(BuildEngine& engine, const BuildNode& node,
                            const BuildValue& value) {
    // The result is valid if the existence matches the value type and the file
    // information remains the same.
    //
    // FIXME: This is inefficient, we will end up doing the stat twice, once
    // when we check the value for up to dateness, and once when we "build" the
    // output.
    //
    // We can solve this by caching ourselves but I wonder if it is something
    // the engine should support more naturally. In practice, this is unlikely
    // to be very performance critical in practice because this is only
    // redundant in the case where we have never built the node before (or need
    // to rebuild it), and thus the additional stat is only one small part of
    // the work we need to perform.
    auto info = node.getFileInfo(
        getBuildSystem(engine).getFileSystem());
    if (info.isMissing()) {
      return value.isMissingInput();
    } else {
      return value.isExistingInput() && value.getOutputInfo() == info;
    }
  }
};

/// This is the task to "build" a file info node which represents raw stat info
/// of a file system object.
class StatTask : public Task {
  StatNode& statnode;

  virtual void start(TaskInterface ti) override {
    // Create a weak link on any potential producer nodes so that we get up to
    // date stat information. We always run (see isResultValid) so this should
    // be safe (unlike directory contents where it may not run).
    ti.mustFollow(BuildKey::makeNode(statnode.getName()).toData());
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& value) override {
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    // FIXME: We should do this work in the background.

    // Get the information on the file.
    auto info = statnode.getFileInfo(getBuildSystem(ti).getFileSystem());
    if (info.isMissing()) {
      ti.complete(BuildValue::makeMissingInput().toData());
      return;
    }

    ti.complete(BuildValue::makeExistingInput(info).toData());
  }

public:
  StatTask(StatNode& statnode) : statnode(statnode) {}

  static bool isResultValid(BuildEngine&, const StatNode&, const BuildValue&) {
    // Always read the stat information
    return false;
  }
};

/// This is the task to "build" a directory node.
///
/// This node effectively just adapts a directory tree signature to a node. The
/// reason why we need it (versus simply making the directory tree signature
/// *be* this, is that we want the directory signature to be able to interface
/// with other build nodes produced by commands).
class DirectoryInputNodeTask : public Task {
  BuildNode& node;

  core::ValueType directorySignature;

  int totalBlockingDeps = 0;
  int finishedBlockingDeps = 0;

  void performUnblockedRequest(TaskInterface ti) {
    // Remove any trailing slash from the node name.
    StringRef path =  node.getName();
    if (path.endswith("/") && path != "/") {
      path = path.substr(0, path.size() - 1);
    }

    ti.request(BuildKey::makeDirectoryTreeSignature(path,
                 node.contentExclusionPatterns()).toData(),
               /*inputID=*/0);
  }

  virtual void start(TaskInterface ti) override {
    // We reserve inputID=0 for the blocked DirectoryTreeSignature request.
    // inputID=1, 2, ... are used for mustScanAfterPaths
    for (auto mustScanAfterPath: node.getMustScanAfterPaths()) {
      ++totalBlockingDeps;
      ti.request(BuildKey::makeNode(mustScanAfterPath).toData(), totalBlockingDeps);
    }

    // If mustScanAfterPaths is empty, simply request DirectoryTreeSignature in start
    if (totalBlockingDeps == 0) {
      performUnblockedRequest(ti);
    }
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface ti, uintptr_t inputID,
                            const KeyType& key, const ValueType& valueData) override {
    if (inputID == 0) {
      directorySignature = valueData;
    } else {
      auto value = BuildValue::fromData(valueData);

      ++finishedBlockingDeps;
      if (finishedBlockingDeps == totalBlockingDeps) {
        // All paths within mustScanAfterPaths are scanned..
        // DirectoryTreeSignature is unblocked
        performUnblockedRequest(ti);
      }
    }
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    // Simply propagate the value.
    ti.complete(ValueType(directorySignature));
  }

public:
  DirectoryInputNodeTask(BuildNode& node) : node(node) {
    assert(!node.isVirtual());
  }
};


/// This is the task to "build" a directory structure node.
///
/// This node effectively just adapts a directory tree structure signature to a
/// node. The reason why we need it (versus simply making the directory tree
/// signature *be* this, is that we want the directory signature to be able to
/// interface with other build nodes produced by commands).
class DirectoryStructureInputNodeTask : public Task {
  BuildNode& node;

  core::ValueType directorySignature;

  virtual void start(TaskInterface ti) override {
    // Remove any trailing slash from the node name.
    StringRef path =  node.getName();
    if (path.endswith("/") && path != "/") {
      path = path.substr(0, path.size() - 1);
    }
    ti.request(BuildKey::makeDirectoryTreeStructureSignature(path,
                 node.contentExclusionPatterns()).toData(),
               /*inputID=*/0);
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& value) override {
    directorySignature = value;
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    // Simply propagate the value.
    ti.complete(ValueType(directorySignature));
  }

public:
  DirectoryStructureInputNodeTask(BuildNode& node) : node(node) {
    assert(!node.isVirtual());
  }
};


/// This is the task to build a virtual node which isn't connected to any
/// output.
class VirtualInputNodeTask : public Task {
  virtual void start(TaskInterface) override {
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& value) override {
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    ti.complete(BuildValue::makeVirtualInput().toData());
  }

public:
  VirtualInputNodeTask() {}

  static bool isResultValid(BuildEngine& engine, const BuildNode& node,
                            const BuildValue& value) {
    // Virtual input nodes are always valid unless the value type is wrong.
    return value.isVirtualInput();
  }
};


/// This is the task to "build" a node which is the product of some command.
///
/// It is responsible for selecting the appropriate producer command to run to
/// produce the node, and for synchronizing any external state the node depends
/// on.
class ProducedNodeTask : public Task {
  Node& node;
  BuildValue nodeResult;
  Command* producingCommand = nullptr;

  // Build specific data.
  //
  // FIXME: We should probably factor this out somewhere else, so we can enforce
  // it is never used when initialized incorrectly.

  // Whether this is a node we are unable to produce.
  bool isInvalid = false;
  
  virtual void start(TaskInterface ti) override {
    // Request the producer command.
    auto getCommand = [&]()->Command* {
      if (node.getProducers().size() == 1) {
        return node.getProducers()[0];
      }
      // Give the delegate a chance to resolve to a single command.
      return getBuildSystem(ti).getDelegate().
          chooseCommandFromMultipleProducers(&node, node.getProducers());
    };

    if (Command* foundCommand = getCommand()) {
      producingCommand = foundCommand;
      ti.request(BuildKey::makeCommand(producingCommand->getName()).toData(),
                 /*InputID=*/0);
      return;
    }

    // Notify that we could not resolve to a single producer.
    getBuildSystem(ti).getDelegate().
        cannotBuildNodeDueToMultipleProducers(&node, node.getProducers());
    isInvalid = true;
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& valueData) override {
    auto value = BuildValue::fromData(valueData);

    // Extract the node result from the command.
    assert(producingCommand);
    nodeResult = producingCommand->getResultForOutput(&node, value);
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    if (isInvalid) {
      getBuildSystem(ti).getDelegate().hadCommandFailure();
      ti.complete(BuildValue::makeFailedInput().toData());
      return;
    }
    
    assert(!nodeResult.isInvalid());
    
    // Complete the task immediately.
    ti.complete(nodeResult.toData());
  }

public:
  ProducedNodeTask(Node& node)
      : node(node), nodeResult(BuildValue::makeInvalid()) {}
  
  static bool isResultValid(BuildEngine& engine, Node& node,
                            const BuildValue& value) {
    // If the result was failure, we always need to rebuild (it may produce an
    // error).
    if (value.isFailedInput())
      return false;

    // If the result was previously a missing input, it may have been because
    // we did not previously know how to produce this node. We do now, so
    // attempt to build it now.
    if (value.isMissingInput())
      return false;

    // The produced node result itself doesn't need any synchronization.
    return true;
  }
};

class ProducedDirectoryNodeTask : public Task {
  Node& node;

  BuildValue nodeResult;
  core::ValueType directorySignature;

  Command* producingCommand = nullptr;

  // Whether this is a node we are unable to produce.
  bool isInvalid = false;
  bool returnDirectorySignature = false;

  virtual void start(TaskInterface ti) override {
    // Request the producer command.
    auto getCommand = [&]()->Command* {
      if (node.getProducers().size() == 1) {
        return node.getProducers()[0];
      }
      // Give the delegate a chance to resolve to a single command.
      return getBuildSystem(ti).getDelegate().
          chooseCommandFromMultipleProducers(&node, node.getProducers());
    };

    if (Command* foundCommand = getCommand()) {
      producingCommand = foundCommand;
      ti.request(BuildKey::makeCommand(producingCommand->getName()).toData(),
                 /*InputID=*/0);
      return;
    }

    // Notify that we could not resolve to a single producer.
    getBuildSystem(ti).getDelegate().
        cannotBuildNodeDueToMultipleProducers(&node, node.getProducers());
    isInvalid = true;
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface ti, uintptr_t inputID,
                            const KeyType& key, const ValueType& valueData) override {
    if (inputID == 0) {
      auto value = BuildValue::fromData(valueData);

      // Extract the node result from the command.
      assert(producingCommand);

      // NOTE: nodeResult only contains stat info of the directory, not its signature.
      nodeResult = producingCommand->getResultForOutput(&node, value);

      if (nodeResult.isExistingInput()) {
        // The external command must have produced the directory node.
        // Request for its signature and store it

        StringRef path =  node.getName();
        if (path.endswith("/") && path != "/") {
          path = path.substr(0, path.size() - 1);
        }
        ti.request(BuildKey::makeDirectoryTreeSignature(path,basic::StringList()).toData(), /*inputID=*/1);
        returnDirectorySignature = true;
      }
    } else if (inputID == 1) {
      directorySignature = valueData;
    }
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    if (returnDirectorySignature) {
      ti.complete(ValueType(directorySignature));
      return;
    } else {
      if (isInvalid) {
        getBuildSystem(ti).getDelegate().hadCommandFailure();
        ti.complete(BuildValue::makeFailedInput().toData());
        return;
      }
      assert(!nodeResult.isInvalid());
      
      // Complete the task immediately.
      ti.complete(nodeResult.toData());
    }
  }

public:
  ProducedDirectoryNodeTask(Node& node)
      : node(node), nodeResult(BuildValue::makeInvalid()) {}

  static bool isResultValid(BuildEngine& engine, Node& node,
                            const BuildValue& value) {
    // If the result was failure, we always need to rebuild (it may produce an
    // error).
    if (value.isFailedInput())
      return false;

    // If the result was previously a missing input, it may have been because
    // we did not previously know how to produce this node. We do now, so
    // attempt to build it now.
    if (value.isMissingInput())
      return false;

    // The produced node result itself doesn't need any synchronization.
    // If the directory signature is changed, it will be reflected in value of this node.
    return true;
  }
};


/// This task is responsible for computing the lists of files in directories.
class DirectoryContentsTask : public Task {
  std::string path;

  /// The value for the input directory.
  BuildValue directoryValue;
  
  virtual void start(TaskInterface ti) override {
    // Request the base directory node -- this task doesn't actually use the
    // value, but this connects the task to its producer if present.

    // FIXME:
    //
    //engine.taskMustFollow(this, BuildKey::makeNode(path).toData());
    //
    // The taskMustFollow method expresses the weak dependency we have on
    // 'path', but only at the task level. What we really want is to say at the
    // 'isResultValid'/scanning level is 'must scan after'. That way we hold up
    // this and downstream rules until the 'path' node has been set into its
    // final state*.
    //
    // With the explicit dependency we are establishing with request(), we
    // will unfortunately mark directory contents as 'needs to be built' under
    // situations where non-releveant stat info has changed. This causes
    // unnecessary rebuilds. See rdar://problem/30640904
    //
    // * The 'final state' of a directory is also a thorny patch of toxic land
    // mines. We really want directory contents to weakly depend upon anything
    // that is currently and/or may be altered within it. i.e. if one rule
    // creates the directory and another rule writes a file into it, we want to
    // defer scanning until both of them have been scanned and possibly run.
    // Having a 'must scan after' would help with the first rule (mkdir), but
    // not the second, in particular if rules are added in subsequent builds.
    // Related rdar://problem/30638921
    //
    ti.request(BuildKey::makeNode(path).toData(), /*inputID=*/0);
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& value) override {
    if (inputID == 0) {
      directoryValue = BuildValue::fromData(value);
      return;
    }
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    // FIXME: We should do this work in the background.
    
    if (directoryValue.isMissingInput()) {
      ti.complete(BuildValue::makeMissingInput().toData());
      return;
    }

    if (directoryValue.isMissingOutput()) {
      // Rewrite a missing output as a missing input build value for this node, both conceptually and as a hedge against violating the downstream expectations of other rules.
      ti.complete(BuildValue::makeMissingInput().toData());
      return;
    }

    if (directoryValue.isFailedInput()) {
      ti.complete(BuildValue::makeFailedInput().toData());
      return;
    }

    // The input directory may be a 'mkdir' command, which can be cancelled or
    // skipped by the engine or the delegate. rdar://problem/50380532
    if (directoryValue.isSkippedCommand()) {
      ti.complete(BuildValue::makeSkippedCommand().toData());
      return;
    }

    std::vector<std::string> filenames;
    std::error_code ec = getContents(path, filenames);

    // Currently, tests and presumably clients expect that errors are silently
    // ignored when reading directory contents listings.
    (void)ec;

    // Create the result.
    ti.complete(BuildValue::makeDirectoryContents(directoryValue.getOutputInfo(),
                                                  filenames).toData());
  }


  static std::error_code getContents(StringRef path, std::vector<std::string>& filenames) {
    // Get the list of files in the directory.
    // FIXME: This is not going through the filesystem object. Indeed the fs
    // object does not currently support directory listing/iteration, but
    // probably should so that clients may override it.
    //
    // Exit the loop if we encounter any errors, to prevent infinitely looping
    // over an invalid directory in some circumstances. rdar://101717159
    std::error_code ec;
    for (auto it = llvm::sys::fs::directory_iterator(path, ec, /*follow_symlinks */false),
         end = llvm::sys::fs::directory_iterator(); it != end && !ec;
         it = it.increment(ec)) {
      // If this is a symlink to a parent directory, exclude it from results so we don't get stuck in a loop.
      if (llvm::sys::fs::is_symlink_file(*it->status())) {
        SmallString<256> resolvedPath;
        if (!llvm::sys::fs::real_path(it->path(), resolvedPath)) {
          if (path.startswith(resolvedPath)) {
            continue;
          }
        }
      }
      filenames.push_back(llvm::sys::path::filename(it->path()));
    }

    // Order the filenames.
    std::sort(filenames.begin(), filenames.end(),
              [](const std::string& a, const std::string& b) {
                return a < b;
              });

    return ec;
  }


public:
  DirectoryContentsTask(StringRef path)
      : path(path), directoryValue(BuildValue::makeInvalid()) {}

  static bool isResultValid(BuildEngine& engine, StringRef path,
                            const BuildValue& value) {
    // The result is valid if the existence matches the existing value type, and
    // the file information remains the same.
    auto info = getBuildSystem(engine).getFileSystem().getFileInfo(
        path);
    if (info.isMissing()) {
      return value.isMissingInput();
    } else {
      if (!value.isDirectoryContents())
        return false;

      // If the type changes rebuild
      if (info.isDirectory() != value.getOutputInfo().isDirectory())
        return false;

      // For files, it is direct stat info that matters
      if (!info.isDirectory())
        return value.getOutputInfo() == info;

      // With filters, we list the current filtered contents and then compare
      // the lists.
      std::vector<std::string> cur;
      std::error_code ec = getContents(path, cur);

      // Currently, tests and presumably clients expect that errors are silently
      // ignored when reading directory contents listings.
      (void)ec;

      auto prev = value.getDirectoryContents();

      if (cur.size() != prev.size())
        return false;

      auto cur_it = cur.begin();
      auto prev_it = prev.begin();
      for (; cur_it != cur.end() && prev_it != prev.end(); cur_it++, prev_it++) {
        if (*cur_it != *prev_it) {
          return false;
        }
      }

      return true;
    }
  }
};


/// This task is responsible for computing the filtered lists of files in
/// directories.
class FilteredDirectoryContentsTask : public Task {
  std::string path;

  /// The exclusion filters used while computing the signature
  StringList filters;

  /// The value for the input directory.
  BuildValue directoryValue;

  virtual void start(TaskInterface ti) override {
    // FIXME:
    //
    //engine.taskMustFollow(this, BuildKey::makeNode(path).toData());
    //
    // The taskMustFollow method expresses the weak dependency we have on
    // 'path', but only at the task level. What we really want is to say at the
    // 'isResultValid'/scanning level is 'must scan after'. That way we hold up
    // this and downstream rules until the 'path' node has been set into its
    // final state*.
    //
    // Here we depend on the file node so that it can be connected up to
    // potential producers and the raw stat information, in case something else
    // has changed the contents of the directory. The value does not encode the
    // raw stat information, thus will produce the same result if the filtered
    // contents is otherwise the same. This reduces unnecessary rebuilds. That
    // said, we are still subject to the 'final state' problem:
    //
    // * The 'final state' of a directory is also a thorny patch of toxic land
    // mines. We really want directory contents to weakly depend upon anything
    // that is currently and/or may be altered within it. i.e. if one rule
    // creates the directory and another rule writes a file into it, we want to
    // defer scanning until both of them have been scanned and possibly run.
    // Having a 'must scan after' would help with the first rule (mkdir), but
    // not the second, in particular if rules are added in subsequent builds.
    // Related rdar://problem/30638921
    ti.request(BuildKey::makeNode(path).toData(), /*inputID=*/0);
    ti.request(BuildKey::makeStat(path).toData(), /*inputID=*/1);
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& value) override {
    if (inputID == 1) {
      directoryValue = BuildValue::fromData(value);
      return;
    }
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    if (directoryValue.isMissingInput()) {
      ti.complete(BuildValue::makeMissingInput().toData());
      return;
    }

    if (!directoryValue.isExistingInput()) {
      ti.complete(BuildValue::makeFailedInput().toData());
      return;
    }

    auto& info = directoryValue.getOutputInfo();

    // Non-directory things are just plain-ol' inputs
    if (!info.isDirectory()) {
      ti.complete(BuildValue::makeExistingInput(info).toData());
      return;
    }

    // Collect the filtered contents
    std::vector<std::string> filenames;
    getFilteredContents(path, filters, filenames);

    // Create the result.
    ti.complete(BuildValue::makeFilteredDirectoryContents(filenames).toData());
  }


  static std::error_code getFilteredContents(StringRef path, const StringList& filters,
                                  std::vector<std::string>& filenames) {
    auto filterStrings = filters.getValues();

    // Get the list of files in the directory.
    // FIXME: This is not going through the filesystem object. Indeed the fs
    // object does not currently support directory listing/iteration, but
    // probably should so that clients may override it.
    //
    // Exit the loop if we encounter any errors, to prevent infinitely looping
    // over an invalid directory in some circumstances. rdar://101717159
    std::error_code ec;
    for (auto it = llvm::sys::fs::directory_iterator(path, ec),
         end = llvm::sys::fs::directory_iterator(); it != end && !ec;
         it = it.increment(ec)) {
      std::string filename = llvm::sys::path::filename(it->path());
      bool excluded = false;
      for (auto pattern : filterStrings) {
        if (llbuild::basic::sys::filenameMatch(pattern.data(),
                                               filename.c_str()) ==
            llbuild::basic::sys::MATCH) {
          excluded = true;
          break;
        }
      }
      if (!excluded)
        filenames.push_back(filename);
    }

    // Order the filenames.
    std::sort(filenames.begin(), filenames.end(),
              [](const std::string& a, const std::string& b) {
                return a < b;
              });

    return ec;
  }


public:
  FilteredDirectoryContentsTask(StringRef path, StringList&& filters)
      : path(path), filters(std::move(filters))
      , directoryValue(BuildValue::makeInvalid()) {}
};



/// This is the task to "build" a directory node which will encapsulate (via a
/// signature) a (optionally) filtered view of the contents of the directory,
/// recursively.
class DirectoryTreeSignatureTask : public Task {
  // The basic algorithm we need to follow:
  //
  // 1. Get the directory contents.
  // 2. Get the subpath directory info.
  // 3. For each node input, if it is a directory, get the input node for it.
  //
  // FIXME: This algorithm currently does a redundant stat for each directory,
  // because we stat it once to find out it is a directory, then again when we
  // gather its contents (to use for validating the directory contents).
  //
  // FIXME: We need to fix the directory list to not get contents for symbolic
  // links.

  /// This structure encapsulates the information we need on each child.
  struct SubpathInfo {
    /// The filename;
    std::string filename;

    /// The result of requesting the node at this subpath, once available.
    ValueType value;

    /// The directory signature, if needed.
    llvm::Optional<ValueType> directorySignatureValue;
  };

  /// The path we are taking the signature of.
  std::string path;

  /// The exclusion filters used while computing the signature
  StringList filters;

  /// The value for the directory itself.
  ValueType directoryValue;

  /// The accumulated list of child input info.
  ///
  /// Once we have the input directory information, we resize this to match the
  /// number of children to avoid dynamically resizing it.
  std::vector<SubpathInfo> childResults;

  virtual void start(TaskInterface ti) override {
    // Ask for the base directory directory contents.
    if (filters.isEmpty()) {
      ti.request(BuildKey::makeDirectoryContents(path).toData(), /*inputID=*/0);
    } else {
      ti.request(BuildKey::makeFilteredDirectoryContents(path, filters).toData(),
                 /*inputID=*/0);
    }
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface ti, uintptr_t inputID,
                            const KeyType& key, const ValueType& valueData) override {
    // The first input is the directory contents.
    if (inputID == 0) {
      // Record the value for the directory.
      directoryValue = valueData;

      // Request the inputs for each subpath.
      auto value = BuildValue::fromData(valueData);
      if ((filters.isEmpty() && !value.isDirectoryContents()) ||
          (!filters.isEmpty() && !value.isFilteredDirectoryContents())) {
        return;
      }

      assert(value.isFilteredDirectoryContents() || value.isDirectoryContents());
      auto filenames = value.getDirectoryContents();
      for (size_t i = 0; i != filenames.size(); ++i) {
        SmallString<256> childPath{ path };
        llvm::sys::path::append(childPath, filenames[i]);
        childResults.emplace_back(SubpathInfo{ filenames[i], {}, None });
        ti.request(BuildKey::makeNode(childPath).toData(), /*inputID=*/1 + i);
      }
      return;
    }

    // If the input is a child, add it to the collection and dispatch a
    // directory request if needed.
    if (inputID >= 1 && inputID < 1 + childResults.size()) {
      auto index = inputID - 1;
      auto& childResult = childResults[index];
      childResult.value = valueData;

      // If this node is a directory, request its signature recursively.
      auto value = BuildValue::fromData(valueData);
      if (value.isExistingInput()) {
        if (value.getOutputInfo().isDirectory()) {
          SmallString<256> childPath{ path };
          llvm::sys::path::append(childPath, childResult.filename);

          ti.request(BuildKey::makeDirectoryTreeSignature(childPath,
                                                          filters).toData(),
                     /*inputID=*/1 + childResults.size() + index);
        }
      }
      return;
    }

    // Otherwise, the input should be a directory signature.
    auto index = inputID - 1 - childResults.size();
    assert(index < childResults.size());
    childResults[index].directorySignatureValue = valueData;
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    // Compute the signature across all of the inputs.
    using llvm::hash_combine;
    llvm::hash_code code = hash_value(path);

    // Add the signature for the actual input path.
    code = hash_combine(
        code, hash_combine_range(directoryValue.begin(), directoryValue.end()));

    // For now, we represent this task as the aggregation of all the inputs.
    for (const auto& info: childResults) {
      // We merge the children by simply combining their encoded representation.
      code = hash_combine(
          code, hash_combine_range(info.value.begin(), info.value.end()));
      if (info.directorySignatureValue.hasValue()) {
        auto& data = info.directorySignatureValue.getValue();
        code = hash_combine(
            code, hash_combine_range(data.begin(), data.end()));
      } else {
        // Combine a random number to represent nil.
        code = hash_combine(code, 0XC183979C3E98722E);
      }
    }

    // Compute the signature.
    ti.complete(BuildValue::makeDirectoryTreeSignature(
                  CommandSignature(uint64_t(code))).toData());
  }

public:
  DirectoryTreeSignatureTask(StringRef path, StringList&& filters)
      : path(path), filters(std::move(filters)) {}
};


/// This is the task to "build" a directory structure node which will
/// encapsulate (via a signature) the structure of the directory, recursively.
class DirectoryTreeStructureSignatureTask : public Task {
  // The basic algorithm we need to follow:
  //
  // 1. Get the directory contents.
  // 2. Get the subpath directory info.
  // 3. For each node input, if it is a directory, get the input node for it.
  //
  // FIXME: This algorithm currently does a redundant stat for each directory,
  // because we stat it once to find out it is a directory, then again when we
  // gather its contents (to use for validating the directory contents).
  //
  // FIXME: We need to fix the directory list to not get contents for symbolic
  // links.

  /// This structure encapsulates the information we need on each child.
  struct SubpathInfo {
    /// The filename;
    std::string filename;
    
    /// The result of requesting the node at this subpath, once available.
    ValueType value;

    /// The directory structure signature, if needed.
    llvm::Optional<ValueType> directoryStructureSignatureValue;
  };
  
  /// The path we are taking the signature of.
  std::string path;

  /// The exclusion filters used while computing the signature
  StringList filters;

  /// The value for the directory itself.
  ValueType directoryValue;

  /// The accumulated list of child input info.
  ///
  /// Once we have the input directory information, we resize this to match the
  /// number of children to avoid dynamically resizing it.
  std::vector<SubpathInfo> childResults;
  
  virtual void start(TaskInterface ti) override {
    // Ask for the base directory directory contents.
    if (filters.isEmpty()) {
      ti.request(BuildKey::makeDirectoryContents(path).toData(), /*inputID=*/0);
    } else {
      ti.request(BuildKey::makeFilteredDirectoryContents(path, filters).toData(),
                 /*inputID=*/0);
    }
  }

  virtual void providePriorValue(TaskInterface,
                                 const ValueType& value) override {
  }

  virtual void provideValue(TaskInterface ti, uintptr_t inputID,
                            const KeyType& key, const ValueType& valueData) override {
    // The first input is the directory contents.
    if (inputID == 0) {
      // Record the value for the directory.
      directoryValue = valueData;

      // Request the inputs for each subpath.
      auto value = BuildValue::fromData(valueData);
      if (value.isMissingInput() || value.isSkippedCommand())
        return;

      assert(value.isFilteredDirectoryContents() || value.isDirectoryContents());
      auto filenames = value.getDirectoryContents();
      for (size_t i = 0; i != filenames.size(); ++i) {
        SmallString<256> childPath{ path };
        llvm::sys::path::append(childPath, filenames[i]);
        childResults.emplace_back(SubpathInfo{ filenames[i], {}, None });
        ti.request(BuildKey::makeNode(childPath).toData(), /*inputID=*/1 + i);
      }
      return;
    }

    // If the input is a child, add it to the collection and dispatch a
    // directory structure request if needed.
    if (inputID >= 1 && inputID < 1 + childResults.size()) {
      auto index = inputID - 1;
      auto& childResult = childResults[index];
      childResult.value = valueData;

      // If this node is a directory, request its signature recursively.
      auto value = BuildValue::fromData(valueData);
      if (value.isExistingInput()) {
        if (value.getOutputInfo().isDirectory()) {
          SmallString<256> childPath{ path };
          llvm::sys::path::append(childPath, childResult.filename);
        
          ti.request(
            BuildKey::makeDirectoryTreeStructureSignature(childPath, filters).toData(),
            /*inputID=*/1 + childResults.size() + index);
        }
      }
      return;
    }

    // Otherwise, the input should be a directory signature.
    auto index = inputID - 1 - childResults.size();
    assert(index < childResults.size());
    childResults[index].directoryStructureSignatureValue = valueData;
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    // Compute the signature across all of the inputs.
    using llvm::hash_combine;
    llvm::hash_code code = hash_value(path);

    // Only merge the structure information on the directory itself.
    {
      // We need to merge mode information about the directory itself, in case
      // it changes type.
      auto value = BuildValue::fromData(directoryValue);
      if (value.isDirectoryContents()) {
        code = hash_combine(code, value.getOutputInfo().mode);
      } else {
        code = hash_combine(
            code, hash_combine_range(directoryValue.begin(),
                                     directoryValue.end()));
      }
    }
    
    // For now, we represent this task as the aggregation of all the inputs.
    for (const auto& info: childResults) {
      // We only merge the "structural" information on a child; i.e. its
      // filename and type.
      code = hash_combine(code, info.filename);
      auto value = BuildValue::fromData(info.value);
      if (value.isExistingInput()) {
        code = hash_combine(code, value.getOutputInfo().mode);
      } else {
        // If this node has been modified to report a non-file value, just merge
        // the encoded representation.
        code = hash_combine(
            code, hash_combine_range(info.value.begin(), info.value.end()));
      }
      
      if (info.directoryStructureSignatureValue.hasValue()) {
        auto& data = info.directoryStructureSignatureValue.getValue();
        code = hash_combine(
            code, hash_combine_range(data.begin(), data.end()));
      } else {
        // Combine a random number to represent nil.
        code = hash_combine(code, 0XC183979C3E98722E);
      }
    }
    
    // Compute the signature.
    ti.complete(BuildValue::makeDirectoryTreeStructureSignature(
                  CommandSignature(uint64_t(code))).toData());
  }

public:
  DirectoryTreeStructureSignatureTask(StringRef path, StringList&& filters) : path(path), filters(std::move(filters)) {}
};


/// This is the task to actually execute a command.
class CommandTask : public Task {
  Command& command;

  virtual void start(TaskInterface ti) override {
    // Notify the client the command is preparing to run.
    getBuildSystem(ti).getDelegate().commandPreparing(&command);

    command.start(getBuildSystem(ti).getBuildSystem(), ti);
  }

  virtual void providePriorValue(TaskInterface ti,
                                 const ValueType& valueData) override {
    BuildValue value = BuildValue::fromData(valueData);
    command.providePriorValue(getBuildSystem(ti).getBuildSystem(), ti, value);
  }

  virtual void provideValue(TaskInterface ti, uintptr_t inputID,
                            const KeyType& key, const ValueType& valueData) override {
    command.provideValue(getBuildSystem(ti).getBuildSystem(), ti, inputID,
                         key, BuildValue::fromData(valueData));
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    auto fn = [this, ti](QueueJobContext* context) mutable {
      // If the build should cancel, do nothing.
      if (ti.isCancelled()) {
        ti.complete(BuildValue::makeCancelledCommand().toData());
        return;
      }

      // Check if the command should be skipped.
      if (!getBuildSystem(ti).getDelegate().shouldCommandStart(&command)) {
        // We need to call commandFinished here because commandPreparing and
        // shouldCommandStart guarantee that they're followed by
        // commandFinished.
        getBuildSystem(ti).getDelegate().commandFinished(&command, ProcessStatus::Skipped);
        ti.complete(BuildValue::makeSkippedCommand().toData());
        return;
      }
    
      // Execute the command, with notifications to the delegate.
      command.execute(getBuildSystem(ti).getBuildSystem(), ti, context, [ti](BuildValue&& result) mutable {
        // Inform the engine of the result.
        if (result.isFailedCommand()) {
          getBuildSystem(ti).getDelegate().hadCommandFailure();
        }
        ti.complete(result.toData());
      });
    };
    if (command.isDetached()) {
      struct DetachedContext: public QueueJobContext {
        unsigned laneID() const override { return -1; }
      };
      DetachedContext ctx;
      fn(&ctx);
    } else {
      ti.spawn({ &command, std::move(fn) });
    }
  }

public:
  CommandTask(Command& command) : command(command) {}

  static bool isResultValid(BuildEngine& engine, Command& command,
                            const BuildValue& value) {
    // Delegate to the command for further checking.
    auto& buildSystem =
      static_cast<BuildSystemEngineDelegate*>(engine.getDelegate())->getBuildSystem();
    return command.isResultValid(buildSystem.getBuildSystem(), value);
  }
};

#pragma mark - BuildSystemEngineDelegate implementation

/// This is a synthesized task used to represent a missing command.
///
/// This command is used in cases where a command has been removed from the
/// manifest, but can still be found during an incremental rebuild. This command
/// is used to inject an invalid value thus forcing downstream clients to
/// rebuild.
class MissingCommandTask : public Task {
private:
  virtual void start(TaskInterface) override { }
  virtual void providePriorValue(TaskInterface,
                                 const ValueType& valueData) override { }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& valueData) override { }

  virtual void inputsAvailable(TaskInterface ti) override {
    // A missing command always builds to an invalid value, and forces
    // downstream clients to be rebuilt (at which point they will presumably see
    // the command is no longer used).
    return ti.complete(BuildValue::makeInvalid().toData(),
                       /*forceChange=*/true);
  }

public:
  using Task::Task;
};

const BuildDescription& BuildSystemEngineDelegate::getBuildDescription() const {
  return system.getBuildDescription();
}

static BuildSystemDelegate::CommandStatusKind
convertStatusKind(core::Rule::StatusKind kind) {
  switch (kind) {
  case core::Rule::StatusKind::IsScanning:
    return BuildSystemDelegate::CommandStatusKind::IsScanning;
  case core::Rule::StatusKind::IsUpToDate:
    return BuildSystemDelegate::CommandStatusKind::IsUpToDate;
  case core::Rule::StatusKind::IsComplete:
    return BuildSystemDelegate::CommandStatusKind::IsComplete;
  }
  assert(0 && "unknown status kind");
  return BuildSystemDelegate::CommandStatusKind::IsScanning;
}

class BuildSystemRule : public Rule {
private:
  /// Called to create the task to build the rule, when necessary.
  std::function<Task*(BuildEngine&)> action;

  /// Called to check whether the previously computed value for this rule is
  /// still valid.
  ///
  /// This callback is designed for use in synchronizing values which represent
  /// state managed externally to the build engine. For example, a rule which
  /// computes something on the file system may use this to verify that the
  /// computed output has not changed since it was built.
  std::function<bool(BuildEngine&, const Rule&,
                     const ValueType&)> resultValid;

  /// Called to indicate a change in the rule status.
  std::function<void(BuildEngine&, StatusKind)> update;

public:
  BuildSystemRule(
    const KeyType& key,
    const basic::CommandSignature& signature,
    std::function<Task*(BuildEngine&)> action,
    std::function<bool(BuildEngine&, const Rule&, const ValueType&)> valid = nullptr,
    std::function<void(BuildEngine&, StatusKind)> update = nullptr)
  : Rule(key, signature), action(action), resultValid(valid), update(update)
  { }

public:
  Task* createTask(BuildEngine& engine) override {
    return action(engine);
  }

  bool isResultValid(BuildEngine& engine, const ValueType& value) override {
    if (!resultValid) return true;
    return resultValid(engine, *this, value);
  }

  void updateStatus(BuildEngine& engine, Rule::StatusKind status) override {
    if (update) update(engine, status);
  }
};



BuildNode *BuildSystemEngineDelegate::lookupNode(StringRef name) {
  // Find the node.
  auto it = getBuildDescription().getNodes().find(name);
  BuildNode* node;
  if (it != getBuildDescription().getNodes().end()) {
    node = static_cast<BuildNode*>(it->second.get());
  } else {
    auto it = dynamicNodes.find(name);
    if (it != dynamicNodes.end()) {
      node = it->second.get();
    } else {
      // Create nodes on the fly for any unknown ones.
      auto nodeOwner = system.createNode(
                                         name, /*isImplicit=*/true);
      node = nodeOwner.get();
      dynamicNodes[name] = std::move(nodeOwner);
    }
  }
  return node;
}

std::unique_ptr<Rule> BuildSystemEngineDelegate::lookupRule(const KeyType& keyData) {
  // Decode the key.
  auto key = BuildKey::fromData(keyData);

  switch (key.getKind()) {
  case BuildKey::Kind::Unknown:
    break;
    
  case BuildKey::Kind::Command: {
    // Find the comand.
    auto it = getBuildDescription().getCommands().find(key.getCommandName());
    if (it == getBuildDescription().getCommands().end()) {
      // If there is no such command, produce an error task.
      return std::unique_ptr<Rule>(new BuildSystemRule(
        keyData,
        /*signature=*/{},
        /*Action=*/ [](BuildEngine& engine) -> Task* {
          return new MissingCommandTask();
        },
        /*IsValid=*/ [](BuildEngine&, const Rule&, const ValueType&) -> bool {
          // The cached result for a missing command is never valid.
          return false;
        }
      ));
    }

    // Create the rule for the command.
    Command* command = it->second.get();
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      command->getSignature(),
      /*Action=*/ [command](BuildEngine& engine) -> Task* {
        return new CommandTask(*command);
      },
      /*IsValid=*/ [command](BuildEngine& engine, const Rule& rule,
                             const ValueType& value) -> bool {
        return CommandTask::isResultValid(
            engine, *command, BuildValue::fromData(value));
      },
      /*UpdateStatus=*/ [command](BuildEngine& engine,
                                  core::Rule::StatusKind status) {
        return ::getBuildSystem(engine).getDelegate().commandStatusChanged(
            command, convertStatusKind(status));
      }
    ));
  }

  case BuildKey::Kind::CustomTask: {
    // Search for a tool which knows how to create the given custom task.
    //
    // FIXME: We should most likely have some kind of registration process so we
    // can do an efficient query here, but exactly how this should look isn't
    // clear yet.
    for (const auto& it: getBuildDescription().getTools()) {
      auto result = it.second->createCustomCommand(key);
      if (!result) continue;

      // Save the custom command.
      customTasks.emplace_back(std::move(result));
      Command *command = customTasks.back().get();
      
      return std::unique_ptr<Rule>(new BuildSystemRule(
        keyData,
        command->getSignature(),
        /*Action=*/ [command](BuildEngine& engine) -> Task* {
          return new CommandTask(*command);
        },
        /*IsValid=*/ [command](BuildEngine& engine, const Rule& rule,
                               const ValueType& value) -> bool {
          return CommandTask::isResultValid(
              engine, *command, BuildValue::fromData(value));
        },
        /*UpdateStatus=*/ [command](BuildEngine& engine,
                                    core::Rule::StatusKind status) {
          return ::getBuildSystem(engine).getDelegate().commandStatusChanged(
              command, convertStatusKind(status));
        }
      ));
    }
    
    // We were unable to create an appropriate custom command, produce an error
    // task.
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      /*signature=*/{},
      /*Action=*/ [](BuildEngine& engine) -> Task* {
        return new MissingCommandTask();
      },
      /*IsValid=*/ [](BuildEngine&, const Rule&, const ValueType&) -> bool {
        // The cached result for a missing command is never valid.
        return false;
      }
    ));
  }

  case BuildKey::Kind::DirectoryContents: {
    std::string path = key.getDirectoryPath();
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      /*signature=*/{},
      /*Action=*/ [path](BuildEngine& engine) -> Task* {
        return new DirectoryContentsTask(path);
      },
      /*IsValid=*/ [path](BuildEngine& engine, const Rule& rule,
          const ValueType& value) mutable -> bool {
        return DirectoryContentsTask::isResultValid(
            engine, path, BuildValue::fromData(value));
      }
    ));
  }

  case BuildKey::Kind::FilteredDirectoryContents: {
    std::string path = key.getFilteredDirectoryPath();
    std::string patterns = key.getContentExclusionPatterns();
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      /*signature=*/{},
      /*Action=*/ [path, patterns](BuildEngine& engine) -> Task* {
        BinaryDecoder decoder(patterns);
        return new FilteredDirectoryContentsTask(path, StringList(decoder));
      },
      /*IsValid=*/ nullptr
    ));
  }

  case BuildKey::Kind::DirectoryTreeSignature: {
    std::string path = key.getDirectoryTreeSignaturePath();
    std::string filters = key.getContentExclusionPatterns();
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      /*signature=*/{},
      /*Action=*/ [path, filters](
          BuildEngine& engine) mutable -> Task* {
        BinaryDecoder decoder(filters);
        return new DirectoryTreeSignatureTask(path, StringList(decoder));
      },
        // Directory signatures don't require any validation outside of their
        // concrete dependencies.
      /*IsValid=*/ nullptr
    ));
  }

  case BuildKey::Kind::DirectoryTreeStructureSignature: {
    std::string path = key.getFilteredDirectoryPath();
    std::string filters = key.getContentExclusionPatterns();
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      /*signature=*/{},
      /*Action=*/ [path, filters](
          BuildEngine& engine) mutable -> Task* {
        BinaryDecoder decoder(filters);
        return new DirectoryTreeStructureSignatureTask(path, StringList(decoder));
      },
        // Directory signatures don't require any validation outside of their
        // concrete dependencies.
      /*IsValid=*/ nullptr
    ));
  }
    
  case BuildKey::Kind::Node: {
    // Find the node.
    BuildNode* node = lookupNode(key.getNodeName());

    // Create the rule used to construct this node.
    //
    // We could bypass this level and directly return the rule to run the
    // command, which would reduce the number of tasks in the system. For now we
    // do the uniform thing, but do differentiate between input and command
    // nodes.

    // Create an input node if there are no producers.
    if (node->getProducers().empty()) {
      if (node->isVirtual()) {
        return std::unique_ptr<Rule>(new BuildSystemRule(
          keyData,
          node->getSignature(),
          /*Action=*/ [](BuildEngine& engine) -> Task* {
            return new VirtualInputNodeTask();
          },
          /*IsValid=*/ [node](BuildEngine& engine, const Rule& rule,
                                const ValueType& value) -> bool {
            return VirtualInputNodeTask::isResultValid(
                engine, *node, BuildValue::fromData(value));
          }
        ));
      }

      // DirectoryInputNodeTask
      if (node->isDirectory()) {
        return std::unique_ptr<Rule>(new BuildSystemRule(
          keyData,
          node->getSignature(),
          /*Action=*/ [node](BuildEngine& engine) -> Task* {
            return new DirectoryInputNodeTask(*node);
          },
            // Directory nodes don't require any validation outside of their
            // concrete dependencies.
          /*IsValid=*/ nullptr
        ));
      }

      if (node->isDirectoryStructure()) {
        return std::unique_ptr<Rule>(new BuildSystemRule(
          keyData,
          node->getSignature(),
          /*Action=*/ [node](BuildEngine& engine) -> Task* {
            return new DirectoryStructureInputNodeTask(*node);
          },
            // Directory nodes don't require any validation outside of their
            // concrete dependencies.
          /*IsValid=*/ nullptr
        ));
      }

      // FileInputNodeTask
      return std::unique_ptr<Rule>(new BuildSystemRule(
        keyData,
        node->getSignature(),
        /*Action=*/ [node](BuildEngine& engine) -> Task* {
          return new FileInputNodeTask(*node);
        },
        /*IsValid=*/ [node](BuildEngine& engine, const Rule& rule,
                            const ValueType& value) -> bool {
          return FileInputNodeTask::isResultValid(
              engine, *node, BuildValue::fromData(value));
        }
      ));
    }

    // Otherwise, create a task for a produced node.
    // ProducedDirectoryNodeTask
    if (node->isDirectory()) {
      return std::unique_ptr<Rule>(new BuildSystemRule(
        keyData,
        node->getSignature(),
        /*Action=*/ [node](BuildEngine& engine) -> Task* {
          return new ProducedDirectoryNodeTask(*node);
        },
        /*IsValid=*/ [node](BuildEngine& engine, const Rule& rule,
                            const ValueType& value) -> bool {
          return ProducedDirectoryNodeTask::isResultValid(
              engine, *node, BuildValue::fromData(value));
        }
      ));
    }

    // ProducedNodeTask
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      node->getSignature(),
      /*Action=*/ [node](BuildEngine& engine) -> Task* {
        return new ProducedNodeTask(*node);
      },
      /*IsValid=*/ [node](BuildEngine& engine, const Rule& rule,
                          const ValueType& value) -> bool {
        return ProducedNodeTask::isResultValid(
            engine, *node, BuildValue::fromData(value));
      }
    ));
  }

  case BuildKey::Kind::Stat: {
    StatNode* statnode;
    auto it = dynamicStatNodes.find(key.getStatName());
    if (it != dynamicStatNodes.end()) {
      statnode = it->second.get();
    } else {
      // Create nodes on the fly for any unknown ones.
      auto statOwner = llvm::make_unique<StatNode>(key.getStatName());
      statnode = statOwner.get();
      dynamicStatNodes[key.getStatName()] = std::move(statOwner);
    }

    // Create the rule to construct this target.
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      /*signature=*/{},
      /*Action=*/ [statnode](BuildEngine& engine) -> Task* {
        return new StatTask(*statnode);
      },
      /*IsValid=*/ [statnode](BuildEngine& engine, const Rule& rule,
                            const ValueType& value) -> bool {
        return StatTask::isResultValid(
            engine, *statnode, BuildValue::fromData(value));
      }
    ));
  }
  case BuildKey::Kind::Target: {
    // Find the target.
    auto it = getBuildDescription().getTargets().find(key.getTargetName());
    if (it == getBuildDescription().getTargets().end()) {
      // FIXME: Invalid target name, produce an error.
      assert(0 && "FIXME: invalid target");
      abort();
    }

    // Create the rule to construct this target.
    Target* target = it->second.get();
    return std::unique_ptr<Rule>(new BuildSystemRule(
      keyData,
      /*signature=*/{},
      /*Action=*/ [target](BuildEngine& engine) -> Task* {
        return new TargetTask(*target);
      },
      /*IsValid=*/ [target](BuildEngine& engine, const Rule& rule,
                            const ValueType& value) -> bool {
        return TargetTask::isResultValid(
            engine, *target, BuildValue::fromData(value));
      }
    ));
  }
  }

  assert(0 && "invalid key type");
  abort();
}

void BuildSystemEngineDelegate::determinedRuleNeedsToRun(Rule* ruleNeedingToRun, Rule::RunReason reason, Rule* inputRule) {
  return getBuildSystem().getDelegate().determinedRuleNeedsToRun(ruleNeedingToRun, reason, inputRule);
}

bool BuildSystemEngineDelegate::shouldResolveCycle(const std::vector<Rule*>& cycle,
                                                     Rule* candidateRule,
                                                     Rule::CycleAction action) {
  return static_cast<BuildSystemFrontendDelegate*>(&getBuildSystem().getDelegate())->shouldResolveCycle(cycle, candidateRule, action);
}

void BuildSystemEngineDelegate::cycleDetected(const std::vector<Rule*>& cycle) {
  // Track that the build has been aborted.
  getBuildSystem().setBuildWasAborted(true);
  static_cast<BuildSystemFrontendDelegate*>(&getBuildSystem().getDelegate())->cycleDetected(cycle);
}

void BuildSystemEngineDelegate::error(const Twine& message) {
  system.error(system.getMainFilename(), message);
}

#pragma mark - BuildSystemImpl implementation

BuildNode *BuildSystemImpl::lookupNode(StringRef name) {
  return engineDelegate.lookupNode(name);
}

std::unique_ptr<BuildNode>
BuildSystemImpl::createNode(StringRef name, bool isImplicit) {
  if (name.endswith("/")) {
    return BuildNode::makeDirectory(name);
  }

  if (!name.empty() && name[0] == '<' && name.back() == '>') {
    return BuildNode::makeVirtual(name);
  }

  return BuildNode::makePlain(name);
}

llvm::Optional<BuildValue> BuildSystemImpl::build(BuildKey key) {

  if (basic::sys::raiseOpenFileLimit() != 0) {
    error(getMainFilename(), "failed to raise open file limit");
    return None;
  }

  // Build the target.
  buildWasAborted = false;
  auto result = buildEngine.build(key.toData());
    
  // Clear out the shell handlers, as we do not want to hold on to them across
  // multiple builds.
  shellHandlers.clear();

  if (buildWasAborted)
    return None;
  return BuildValue::fromData(result);
}

bool BuildSystemImpl::build(StringRef target) {
  // The build description must have been loaded.
  if (!buildDescription) {
    error(getMainFilename(), "no build description loaded");
    return false;
  }

  // If target name is not passed then we try to load the default target name
  // from manifest file
  if (target.empty()) {
    target = getBuildDescription().getDefaultTarget();
  }

  // Validate the target name.
  auto& targets = getBuildDescription().getTargets();
  if (targets.find(target) == targets.end()) {
    error(getMainFilename(), "No target named '" + target + "' in build description");
    return false;
  }

  return build(BuildKey::makeTarget(target)).hasValue();
}

#pragma mark - PhonyTool implementation

class PhonyCommand : public ExternalCommand {
public:
  using ExternalCommand::ExternalCommand;

  virtual bool shouldShowStatus() override { return false; }

  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream(result) << getName();
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream(result) << getName();
  }

  virtual void startExternalCommand(BuildSystem&, TaskInterface) override {
    return;
  }

  virtual void provideValueExternalCommand(
      BuildSystem&,
      TaskInterface,
      uintptr_t inputID,
      const BuildValue& value) override { }

  virtual void executeExternalCommand(
      BuildSystem&,
      TaskInterface,
      QueueJobContext* context,
      llvm::Optional<ProcessCompletionFn> completionFn) override {
    // Nothing needs to be done for phony commands.
    if (completionFn.hasValue())
      completionFn.getValue()(ProcessStatus::Succeeded);
  }

  virtual BuildValue getResultForOutput(Node* node, const BuildValue& value) override {
    // If the node is virtual, the output is always a virtual input value,
    // regardless of the actual build value.
    //
    // This is a special case for phony commands, to avoid them incorrectly
    // propagating failed/cancelled states onwards to downstream commands when
    // they are being used only for ordering purposes.
    auto buildNode = static_cast<BuildNode*>(node);
    if (buildNode->isVirtual() && !buildNode->isCommandTimestamp()) {
      return BuildValue::makeVirtualInput();
    }

    // Otherwise, delegate to the inherited implementation.
    return ExternalCommand::getResultForOutput(node, value);
  }
};

class PhonyTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported configuration attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported configuration attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<PhonyCommand>(name);
  }
};

#pragma mark - ShellTool implementation

class ShellTool : public Tool {
private:
  bool controlEnabled = true;

public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "control-enabled") {
      if (value != "true" && value != "false") {
        ctx.error("invalid value: '" + value + "' for attribute '" +
                  name + "'");
        return false;
      }
      controlEnabled = (value == "true");
    } else {
      ctx.error("unexpected attribute: '" + name + "'");
      return false;
    }
    return true;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<ShellCommand>(name, controlEnabled);
  }
};

#pragma mark - ClangTool implementation

class ClangShellCommand : public ExternalCommand {
  /// The compiler command to invoke.
  std::vector<StringRef> args;
  
  /// The path to the dependency output file, if used.
  std::string depsPath;
  
  virtual CommandSignature getSignature() const override {
    return ExternalCommand::getSignature()
        .combine(args);
  }

  bool processDiscoveredDependencies(TaskInterface ti,
                                     QueueJobContext* context) {
    // Read the dependencies file.
    auto input = getBuildSystem(ti).getFileSystem().getFileContents(depsPath);
    if (!input) {
      getBuildSystem(ti).getDelegate().commandHadError(this,
                     "unable to open dependencies file (" + depsPath + ")");
      return false;
    }

    // Parse the output.
    //
    // We just ignore the rule, and add any dependency that we encounter in the
    // file.
    struct DepsActions : public core::MakefileDepsParser::ParseActions {
      TaskInterface ti;
      ClangShellCommand* command;
      unsigned numErrors{0};

      DepsActions(TaskInterface ti,
                  ClangShellCommand* command)
          : ti(ti), command(command) {}

      virtual void error(StringRef message, uint64_t position) override {
        getBuildSystem(ti).getDelegate().commandHadError(command,
                       "error reading dependency file '" + command->depsPath +
                       "': " + message.str());
        ++numErrors;
      }

      virtual void actOnRuleDependency(StringRef dependency,
                                       StringRef unescapedWord) override {
        ti.discoveredDependency(BuildKey::makeNode(unescapedWord).toData());
        getBuildSystem(ti).getDelegate().commandFoundDiscoveredDependency(command, unescapedWord,
                                                                          DiscoveredDependencyKind::Input);
      }

      virtual void actOnRuleStart(StringRef name,
                                  StringRef unescapedWord) override {}

      virtual void actOnRuleEnd() override {}
    };

    DepsActions actions(ti, this);
    core::MakefileDepsParser(input->getBuffer(), actions, false).parse();
    return actions.numErrors == 0;
  }

public:
  using ExternalCommand::ExternalCommand;

  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream(result) << getDescription();
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream os(result);
    bool first = true;
    for (const auto& arg: args) {
      if (!first) os << " ";
      first = false;
      basic::appendShellEscapedString(os, arg);
    }
  }
  
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "args") {
      // When provided as a scalar string, we default to executing using the
      // shell.
      args.clear();
      args.push_back(ctx.getDelegate().getInternedString(DefaultShellPath));
      args.push_back(ctx.getDelegate().getInternedString("-c"));
      args.push_back(ctx.getDelegate().getInternedString(value));
    } else if (name == "deps") {
      depsPath = value;
    } else {
      return ExternalCommand::configureAttribute(ctx, name, value);
    }

    return true;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    if (name == "args") {
      args.clear();
      args.reserve(values.size());
      for (auto arg: values) {
        args.emplace_back(ctx.getDelegate().getInternedString(arg));
      }
    } else {
        return ExternalCommand::configureAttribute(ctx, name, values);
    }

    return true;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    return ExternalCommand::configureAttribute(ctx, name, values);
  }

  virtual void startExternalCommand(BuildSystem&, TaskInterface) override {
    return;
  }

  virtual void provideValueExternalCommand(
      BuildSystem&,
      TaskInterface,
      uintptr_t inputID,
      const BuildValue& value) override { }

  virtual void executeExternalCommand(BuildSystem&,
                                      TaskInterface ti,
                                      QueueJobContext* context,
                                      llvm::Optional<ProcessCompletionFn> completionFn) override {
    // Execute the command.
    ti.spawn(context, args, {}, {true}, {[this, ti, completionFn](ProcessResult result) mutable {

      if (result.status != ProcessStatus::Succeeded) {
        // If the command failed, there is no need to gather dependencies.
        if (completionFn.hasValue())
          completionFn.getValue()(result);
        return;
      }

      // Otherwise, collect the discovered dependencies, if used.
      if (!depsPath.empty()) {
        ti.spawn({ this, [this, ti, completionFn, result](QueueJobContext* context) mutable {
          if (!processDiscoveredDependencies(ti, context)) {
            // If we were unable to process the dependencies output, report a
            // failure.
            if (completionFn.hasValue())
              completionFn.getValue()(ProcessStatus::Failed);
            return;
          }
          if (completionFn.hasValue())
            completionFn.getValue()(result);
        }}, QueueJobPriority::High);
        return;
      }

      if (completionFn.hasValue())
        completionFn.getValue()(result);
    }});
  }
};

class ClangTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<ClangShellCommand>(name);
  }
};

#pragma mark - SwiftCompilerTool implementation

class SwiftGetVersionCommand : public Command {
  std::string executable;
  
public:
  SwiftGetVersionCommand(const BuildKey& key)
      : Command("swift-get-version"), executable(key.getCustomTaskData()) {
  }

  // FIXME: Should create a CustomCommand class, to avoid all the boilerplate
  // required implementations.

  bool shouldShowStatus() override { return false; }
    
  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream(result) << "Checking Swift Compiler Version";
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream(result) << '"' << executable << '"'
                                      << " --version";
  }

  virtual void configureDescription(const ConfigureContext&,
                                    StringRef value) override { }
  virtual void configureInputs(const ConfigureContext&,
                               const std::vector<Node*>& value) override { }
  virtual void configureOutputs(const ConfigureContext&,
                                const std::vector<Node*>& value) override { }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual BuildValue getResultForOutput(Node* node,
                                        const BuildValue& value) override {
    // This method should never be called on a custom command.
    llvm_unreachable("unexpected");
    return BuildValue::makeInvalid();
  }
  
  virtual bool isResultValid(BuildSystem&, const BuildValue& value) override {
    // Always rebuild this task.
    return false;
  }

  virtual void start(BuildSystem&, TaskInterface ti) override { }
  virtual void providePriorValue(BuildSystem&, TaskInterface ti,
                                 const BuildValue&) override { }
  virtual void provideValue(BuildSystem&,
                            TaskInterface ti,
                            uintptr_t inputID,
                            const KeyType& key,
                            const BuildValue& value) override { }

  virtual void execute(BuildSystem&,
                       TaskInterface ti,
                       QueueJobContext* context,
                       ResultFn resultFn) override {
    // Construct the command line used to query the swift compiler version.
    //
    // FIXME: Need a decent subprocess interface.
    SmallString<256> command;
    llvm::raw_svector_ostream commandOS(command);
    commandOS << basic::shellEscaped(executable);
    commandOS << " " << "--version";
#if defined(_WIN32)
    // FIXME:  cmd.exe uses different syntax for I/O redirection to null.
    commandOS << " 2>NUL";
#else
    commandOS << " " << "2>/dev/null";
#endif

    // Read the result.
    FILE *fp = basic::sys::popen(commandOS.str().str().c_str(), "r");
    SmallString<4096> result;
    if (fp) {
      char buf[4096];
      for (;;) {
        ssize_t numRead = fread(buf, 1, sizeof(buf), fp);
        if (numRead == 0) {
          // FIXME: Error handling.
          break;
        }
        result.append(StringRef(buf, numRead));
      }
      basic::sys::pclose(fp);
    }

    // For now, we can get away with just encoding this as a successful
    // command and relying on the signature to detect changes.
    //
    // FIXME: We should support BuildValues with arbitrary payloads.
    resultFn(BuildValue::makeSuccessfulCommandWithOutputSignature(
       basic::FileInfo{}, CommandSignature(result)));
  }
};

class SwiftCompilerShellCommand : public ExternalCommand {
  /// The compiler command to invoke.
  std::string executable = "swiftc";
  
  /// The name of the module.
  std::string moduleName;
  
  /// Module aliases used to build this module. For example, if
  /// `-module-alias Foo=Bar` was passed, and source files in
  /// this module references `Foo`, e.g.  `import Foo`, the `Bar`
  /// module will loaded and used to compile this module.
  std::vector<std::string> moduleAliases;

  /// The path of the output module.
  std::string moduleOutputPath;

  /// The list of sources (combined).
  std::vector<std::string> sourcesList;

  /// The list of objects (combined).
  std::vector<std::string> objectsList;

  /// The list of import paths (combined).
  std::vector<std::string> importPaths;

  /// The directory in which to store temporary files.
  std::string tempsPath;

  /// Additional arguments, as a string.
  std::vector<std::string> otherArgs;

  /// Whether the sources are part of a library or not.
  bool isLibrary = false;
  
  /// Whether to enable -whole-module-optimization.
  bool enableWholeModuleOptimization = false;

  /// Enables multi-threading with the thread count if > 0.
  ///
  /// Note: This is only used when whole module optimization is enabled.
  std::string numThreads = "0";

  virtual CommandSignature getSignature() const override {
    return ExternalCommand::getSignature()
        .combine(executable)
        .combine(moduleName)
        .combine(moduleAliases)
        .combine(moduleOutputPath)
        .combine(sourcesList)
        .combine(objectsList)
        .combine(importPaths)
        .combine(tempsPath)
        .combine(otherArgs)
        .combine(isLibrary);
  }

  /// Get the path to use for the output file map.
  void getOutputFileMapPath(SmallVectorImpl<char>& result) const {
    llvm::sys::path::append(result, tempsPath, "output-file-map.json");
  }
    
  /// Compute the complete set of command line arguments to invoke swift with.
  void constructCommandLineArgs(StringRef outputFileMapPath,
                                std::vector<StringRef>& result) const {
    result.push_back(executable);
    result.push_back("-module-name");
    result.push_back(moduleName);

    for (const auto& nameAndAlias: moduleAliases) {
      // E.g. `-module-alias Foo=Bar`
      result.push_back("-module-alias");
      result.push_back(nameAndAlias);
    }

    result.push_back("-emit-dependencies");
    if (!moduleOutputPath.empty()) {
      result.push_back("-emit-module");
      result.push_back("-emit-module-path");
      result.push_back(moduleOutputPath);
    }
    result.push_back("-output-file-map");
    result.push_back(outputFileMapPath);
    if (isLibrary) {
      result.push_back("-parse-as-library");
    }
    if (enableWholeModuleOptimization) {
      result.push_back("-whole-module-optimization");
      result.push_back("-num-threads");
      result.push_back(numThreads);
    } else {
      result.push_back("-incremental");
    }
    result.push_back("-c");
    for (const auto& source: sourcesList) {
      result.push_back(source);
    }
    for (const auto& import: importPaths) {
      result.push_back("-I");
      result.push_back(import);
    }
    for (const auto& arg: otherArgs) {
      result.push_back(arg);
    }
  }
  
public:
  using ExternalCommand::ExternalCommand;

  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
      llvm::raw_svector_ostream(result)
        << "Compiling Swift Module '" << moduleName
        << "' (" << sourcesList.size() << " sources)";
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    SmallString<64> outputFileMapPath;
    getOutputFileMapPath(outputFileMapPath);
    
    std::vector<StringRef> commandLine;
    constructCommandLineArgs(outputFileMapPath, commandLine);
    
    llvm::raw_svector_ostream os(result);
    bool first = true;
    for (const auto& arg: commandLine) {
      if (!first) os << " ";
      first = false;
      // FIXME: This isn't correct, we need utilities for doing shell quoting.
      if (arg.find(' ') != StringRef::npos) {
        os << '"' << arg << '"';
      } else {
        os << arg;
      }
    }
  }
  
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "executable") {
      executable = value;
    } else if (name == "module-name") {
      moduleName = value;
    } else if (name == "module-output-path") {
      moduleOutputPath = value;
    } else if (name == "sources") {
      SmallVector<StringRef, 32> sources;
      StringRef(value).split(sources, " ", /*MaxSplit=*/-1,
                             /*KeepEmpty=*/false);
      sourcesList = std::vector<std::string>(sources.begin(), sources.end());
    } else if (name == "objects") {
      SmallVector<StringRef, 32> objects;
      StringRef(value).split(objects, " ", /*MaxSplit=*/-1,
                             /*KeepEmpty=*/false);
      objectsList = std::vector<std::string>(objects.begin(), objects.end());
    } else if (name == "import-paths") {
      SmallVector<StringRef, 32> imports;
      StringRef(value).split(imports, " ", /*MaxSplit=*/-1,
                             /*KeepEmpty=*/false);
      importPaths = std::vector<std::string>(imports.begin(), imports.end());
    } else if (name == "temps-path") {
      tempsPath = value;
    } else if (name == "is-library") {
      if (!configureBool(ctx, isLibrary, name, value))
        return false;
    } else if (name == "enable-whole-module-optimization") {
      if (!configureBool(ctx, enableWholeModuleOptimization, name, value))
        return false;
    } else if (name == "num-threads") {
      int numThreadsInt = 0;
      if (value.getAsInteger(10, numThreadsInt)) {
        ctx.error("'" + name + "' should be an int.");
        return false;
      }
      if (numThreadsInt < 0) {
        ctx.error("'" + name + "' should be greater than or equal to zero.");
        return false;
      }
      numThreads = value;
    } else if (name == "other-args") {
      SmallVector<StringRef, 32> args;
      StringRef(value).split(args, " ", /*MaxSplit=*/-1,
                             /*KeepEmpty=*/false);
      otherArgs = std::vector<std::string>(args.begin(), args.end());
    } else {
      return ExternalCommand::configureAttribute(ctx, name, value);
    }

    return true;
  }

  // Extracts and stores the bool value of an attribute inside "to" variable.
  // Returns true on success and false on error.
  bool configureBool(const ConfigureContext& ctx, bool& to, StringRef name, StringRef value) {
    if (value != "true" && value != "false") {
      ctx.error("invalid value: '" + value + "' for attribute '" +
          name + "'");
      return false;
    }
    to = value == "true";
    return true;
  }

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    if (name == "sources") {
      sourcesList = std::vector<std::string>(values.begin(), values.end());
    } else if (name == "objects") {
      objectsList = std::vector<std::string>(values.begin(), values.end());
    } else if (name == "import-paths") {
      importPaths = std::vector<std::string>(values.begin(), values.end());
    } else if (name == "other-args") {
      otherArgs = std::vector<std::string>(values.begin(), values.end());
    } else if (name == "module-aliases") {
      moduleAliases = std::vector<std::string>(values.begin(), values.end());
    } else {
      return ExternalCommand::configureAttribute(ctx, name, values);
    }
    
    return true;
  }

  bool writeOutputFileMap(TaskInterface ti,
                          StringRef outputFileMapPath,
                          std::vector<std::string>& depsFiles_out) const {
    assert(sourcesList.size() == objectsList.size());

    SmallString<16> data;
    std::error_code ec;
    llvm::raw_fd_ostream os(outputFileMapPath, ec,
                            llvm::sys::fs::OpenFlags::F_Text);
    if (ec) {
      getBuildSystem(ti).getDelegate().commandHadError((Command*)this,
                      "unable to create output file map: '" + outputFileMapPath.str() + "'");
      return false;
    }

    os << "{\n";

    // Write the master file dependencies entry.
    SmallString<16> masterDepsPath;
    llvm::sys::path::append(masterDepsPath, tempsPath, "master.swiftdeps");
    os << "  \"\": {\n";
    if (enableWholeModuleOptimization) {
      SmallString<16> depsPath;
      llvm::sys::path::append(depsPath, tempsPath, moduleName + ".d");
      depsFiles_out.push_back(depsPath.str());
      SmallString<16> object;
      llvm::sys::path::append(object, tempsPath, moduleName + ".o");
      os << "    \"dependencies\": \"" << escapeForJSON(depsPath) << "\",\n";
      os << "    \"object\": \"" << escapeForJSON(object) << "\",\n";
    }
    os << "    \"swift-dependencies\": \"" << escapeForJSON(masterDepsPath) << "\"\n";
    os << "  },\n";

    // Write out the entries for each source file.
    for (unsigned i = 0; i != sourcesList.size(); ++i) {
      auto source = sourcesList[i];
      auto object = objectsList[i];
      auto objectDir = llvm::sys::path::parent_path(object);
      auto sourceStem = llvm::sys::path::stem(source);
      SmallString<16> partialModulePath;
      llvm::sys::path::append(partialModulePath, objectDir,
                              sourceStem + "~partial.swiftmodule");
      SmallString<16> swiftDepsPath;
      llvm::sys::path::append(swiftDepsPath, objectDir,
                              sourceStem + ".swiftdeps");

      os << "  \"" << escapeForJSON(source) << "\": {\n";
      if (!enableWholeModuleOptimization) {
        SmallString<16> depsPath;
        llvm::sys::path::append(depsPath, objectDir, sourceStem + ".d");
        os << "    \"dependencies\": \"" << escapeForJSON(depsPath) << "\",\n";
        depsFiles_out.push_back(depsPath.str());
      }
      os << "    \"object\": \"" << escapeForJSON(object) << "\",\n";
      os << "    \"swiftmodule\": \"" << escapeForJSON(partialModulePath) << "\",\n";
      os << "    \"swift-dependencies\": \"" << escapeForJSON(swiftDepsPath) << "\"\n";
      os << "  }" << ((i + 1) < sourcesList.size() ? "," : "") << "\n";
    }

    os << "}\n";

    os.close();

    return true;
  }

  bool processDiscoveredDependencies(TaskInterface ti, StringRef depsPath) {
    // Read the dependencies file.
    auto input = getBuildSystem(ti).getFileSystem().getFileContents(depsPath);
    if (!input) {
      getBuildSystem(ti).getDelegate().commandHadError(this,
                     "unable to open dependencies file (" + depsPath.str() + ")");
      return false;
    }

    // Parse the output.
    //
    // We just ignore the rule, and add any dependency that we encounter in the
    // file.
    struct DepsActions : public core::MakefileDepsParser::ParseActions {
      TaskInterface ti;
      StringRef depsPath;
      Command* command;
      unsigned numErrors{0};
      unsigned ruleNumber{0};
      
      DepsActions(TaskInterface ti,
                  StringRef depsPath, Command* command)
          : ti(ti), depsPath(depsPath) {}

      virtual void error(StringRef message, uint64_t position) override {
        getBuildSystem(ti).getDelegate().commandHadError(command,
                       "error reading dependency file '" + depsPath.str() +
                       "': " + message.str());
        ++numErrors;
      }

      virtual void actOnRuleDependency(StringRef dependency,
                                       StringRef unescapedWord) override {
        // Only process dependencies for the first rule (the output file), the
        // rest are identical.
        if (ruleNumber == 0) {
          ti.discoveredDependency(BuildKey::makeNode(unescapedWord).toData());
          getBuildSystem(ti).getDelegate().commandFoundDiscoveredDependency(command, unescapedWord,
                                                                            DiscoveredDependencyKind::Input);
        }
      }

      virtual void actOnRuleStart(StringRef name,
                                  StringRef unescapedWord) override {}

      virtual void actOnRuleEnd() override {
        ++ruleNumber;
      }
    };

    DepsActions actions(ti, depsPath, this);
    core::MakefileDepsParser(input->getBuffer(), actions, false).parse();
    return actions.numErrors == 0;
  }

  /// Overridden start to also introduce a dependency on the Swift compiler
  /// version.
  virtual void start(BuildSystem& system, TaskInterface ti) override {
    ExternalCommand::start(system, ti);
    
    // The Swift compiler version is also an input.
    //
    // FIXME: We need to fix the input ID situation, this is not extensible. We
    // either have to build a registration of the custom tasks so they can divy
    // up the input ID namespace, or we should just use the keys. Probably move
    // to just using the keys, unless there is a place where that is really not
    // cheap.
    auto getVersionKey = BuildKey::makeCustomTask(
        "swift-get-version", executable);
    ti.request(getVersionKey.toData(), core::BuildEngine::kMaximumInputID - 1);
  }

  /// Overridden to access the Swift compiler version.
  virtual void provideValue(BuildSystem& system,
                            TaskInterface ti,
                            uintptr_t inputID,
                            const KeyType& key,
                            const BuildValue& value) override {
    // We can ignore the 'swift-get-version' input, it is just used to detect
    // that we need to rebuild.
    if (inputID == core::BuildEngine::kMaximumInputID - 1) {
      return;
    }
    
    ExternalCommand::provideValue(system, ti, inputID, key, value);
  }

  virtual void startExternalCommand(BuildSystem&, TaskInterface) override {
    return;
  }

  virtual void provideValueExternalCommand(
      BuildSystem&,
      TaskInterface ti,
      uintptr_t inputID,
      const BuildValue& value) override { }

  virtual void executeExternalCommand(
      BuildSystem& system,
      TaskInterface ti,
      QueueJobContext* context,
      llvm::Optional<ProcessCompletionFn> completionFn) override {
    // FIXME: Need to add support for required parameters.
    if (sourcesList.empty()) {
      system.getDelegate().error("", {}, "no configured 'sources'");
      if (completionFn.hasValue())
        completionFn.getValue()(ProcessStatus::Failed);
      return;
    }
    if (objectsList.empty()) {
      system.getDelegate().error("", {}, "no configured 'objects'");
      if (completionFn.hasValue())
        completionFn.getValue()(ProcessStatus::Failed);
      return;
    }
    if (moduleName.empty()) {
      system.getDelegate().error("", {}, "no configured 'module-name'");
      if (completionFn.hasValue())
        completionFn.getValue()(ProcessStatus::Failed);
      return;
    }
    if (tempsPath.empty()) {
      system.getDelegate().error("", {}, "no configured 'temps-path'");
      if (completionFn.hasValue())
        completionFn.getValue()(ProcessStatus::Failed);
      return;
    }

    if (sourcesList.size() != objectsList.size()) {
      system.getDelegate().error(
          "", {}, "'sources' and 'objects' are not the same size");
      if (completionFn.hasValue())
        completionFn.getValue()(ProcessStatus::Failed);
      return;
    }

    // Ensure the temporary directory exists.
    //
    // We ignore failures here, and just let things that depend on this fail.
    //
    // FIXME: This should really be done using an additional implicit input, so
    // it only happens once per build.
    (void) system.getFileSystem().createDirectories(tempsPath);
 
    SmallString<64> outputFileMapPath;
    getOutputFileMapPath(outputFileMapPath);
    
    // Form the complete command.
    std::vector<StringRef> commandLine;
    constructCommandLineArgs(outputFileMapPath, commandLine);

    // Write the output file map.
    std::vector<std::string> depsFiles;
    if (!writeOutputFileMap(ti, outputFileMapPath, depsFiles)) {
      if (completionFn.hasValue())
        completionFn.getValue()(ProcessStatus::Failed);
      return;
    }

    // Execute the command.
    auto result = ti.spawn(context, commandLine);

    if (result != ProcessStatus::Succeeded) {
      // If the command failed, there is no need to gather dependencies.
      if (completionFn.hasValue())
        completionFn.getValue()(result);
      return;
    }

    // Load all of the discovered dependencies.
    ti.spawn({ this, [this, ti, completionFn, result, depsFiles](QueueJobContext* context) mutable {
      for (const auto& depsPath: depsFiles) {
        if (!processDiscoveredDependencies(ti, depsPath)) {
          if (completionFn.hasValue())
            completionFn.getValue()(ProcessStatus::Failed);
          return;
        }
      }
      if (completionFn.hasValue())
        completionFn.getValue()(result);
    }}, basic::QueueJobPriority::High);
  }
};

class SwiftCompilerTool : public Tool {
public:
  SwiftCompilerTool(StringRef name) : Tool(name) {}

  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name, StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<SwiftCompilerShellCommand>(name);
  }

  virtual std::unique_ptr<Command> createCustomCommand(
      const BuildKey& key) override {
    if (key.getCustomTaskName() == "swift-get-version" ) {
      return llvm::make_unique<SwiftGetVersionCommand>(key);
    }

    return nullptr;
  }
};


#pragma mark - MkdirTool implementation

class MkdirCommand : public ExternalCommand {
  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream(result) << getDescription();
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream os(result);
    os << "mkdir -p ";
    // FIXME: This isn't correct, we need utilities for doing shell quoting.
    if (StringRef(getOutputs()[0]->getName()).find(' ') != StringRef::npos) {
      os << '"' << getOutputs()[0]->getName() << '"';
    } else {
      os << getOutputs()[0]->getName();
    }
  }

  virtual bool isResultValid(BuildSystem& system,
                             const BuildValue& value) override {
    // If the prior value wasn't for a successful command, recompute.
    if (!value.isSuccessfulCommand())
      return false;

    // Otherwise, the result is valid if the directory still exists.
    auto info = getOutputs()[0]->getFileInfo(
        system.getFileSystem());
    if (info.isMissing())
      return false;

    // If the item is not a directory, it needs to be recreated.
    if (!info.isDirectory())
      return false;

    // FIXME: We should strictly enforce the integrity of this validity routine
    // by ensuring that the build result for this command does not fully encode
    // the file info, but rather just encodes its success. As is, we are leaking
    // out the details of the file info (like the timestamp), but not rerunning
    // when they change. This is by design for this command, but it would still
    // be nice to be strict about it.
    
    return true;
  }
  
  virtual void startExternalCommand(BuildSystem&, TaskInterface ti) override {
    return;
  }

  virtual void provideValueExternalCommand(
      BuildSystem&,
      TaskInterface ti,
      uintptr_t inputID,
      const BuildValue& value) override { }

  virtual void executeExternalCommand(
      BuildSystem& system,
      TaskInterface ti,
      QueueJobContext* context,
      llvm::Optional<ProcessCompletionFn> completionFn) override {
    auto output = getOutputs()[0];
    if (!system.getFileSystem().createDirectories(
            output->getName())) {
      getBuildSystem(ti).getDelegate().commandHadError(this,
                     "unable to create directory '" + output->getName().str() + "'");
      if (completionFn.hasValue())
        completionFn.getValue()(ProcessStatus::Failed);
      return;
    }
    if (completionFn.hasValue())
      completionFn.getValue()(ProcessStatus::Succeeded);
  }
  
public:
  using ExternalCommand::ExternalCommand;
};

class MkdirTool : public Tool {

public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    auto res = llvm::make_unique<MkdirCommand>(name);
    return res;
  }
};

#pragma mark - SymlinkTool implementation

class SymlinkCommand : public Command {
  /// The path of the actual symbolic link to create, if different from the
  /// output node.
  std::string linkOutputPath;
  
  /// The command description.
  std::string description;

  /// The contents to write at the output path.
  std::string contents;

  /// Get the destination path.
  StringRef getActualOutputPath() const {
    return linkOutputPath.empty() ? outputs[0]->getName() :
      StringRef(linkOutputPath);
  }
  
  virtual CommandSignature getSignature() const override {
    CommandSignature code(outputs[0]->getName());
    code = code.combine(contents);
    for (const auto* input: inputs) {
      code = code.combine(input->getName());
    }
    return code;
  }

  virtual void configureDescription(const ConfigureContext&,
                                    StringRef value) override {
    description = value;
  }

  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream(result) << description;
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream os(result);
    os << "ln -sfh ";
    StringRef outputPath = getActualOutputPath();
    if (outputs.empty() || !outputPath.empty()) {
      // FIXME: This isn't correct, we need utilities for doing shell quoting.
      if (outputPath.find(' ') != StringRef::npos) {
        os << '"' << outputPath << '"';
      } else {
        os << outputPath;
      }
    } else {
      os << "<<<missing output>>>";
    }
    os << ' ';
    // FIXME: This isn't correct, we need utilities for doing shell quoting.
    if (StringRef(contents).find(' ') != StringRef::npos) {
      os << '"' << contents << '"';
    } else {
      os << contents;
    }
  }
  
  virtual void configureInputs(const ConfigureContext& ctx,
                               const std::vector<Node*>& value) override {
    inputs.reserve(value.size());
    for (auto* node: value) {
      inputs.emplace_back(static_cast<BuildNode*>(node));
    }
  }

  virtual void configureOutputs(const ConfigureContext& ctx,
                                const std::vector<Node*>& value) override {
    if (value.size() == 1) {
      outputs.push_back(static_cast<BuildNode*>(value[0]));
    } else if (value.empty()) {
      ctx.error("missing declared output");
    } else {
      ctx.error("unexpected explicit output: '" + value[1]->getName() + "'");
    }
  }
  
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "contents") {
      contents = value;
      return true;
    } else if (name == "link-output-path") {
      linkOutputPath = value;
      return true;
    } else if (name == "repair-via-ownership-analysis") {
      if (value == "true") {
        repairViaOwnershipAnalysis = true;
        return true;
      } else if (value == "false") {
        repairViaOwnershipAnalysis = false;
        return true;
      } else {
        ctx.error("invalid value for attribute: '" + name + "'");
        return false;
      }
    } else {
      ctx.error("unexpected attribute: '" + name + "'");
      return false;
    }
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual BuildValue getResultForOutput(Node* node,
                                        const BuildValue& value) override {
    // If the value was a failed command, propagate the failure.
    if (value.isFailedCommand() || value.isPropagatedFailureCommand() ||
        value.isCancelledCommand())
      return BuildValue::makeFailedInput();
    if (value.isSkippedCommand())
      return BuildValue::makeSkippedCommand();

    // Otherwise, we should have a successful command -- return the actual
    // result for the output.
    assert(value.isSuccessfulCommand());

    auto info = value.getOutputInfo();
    if (info.isMissing())
        return BuildValue::makeMissingOutput();
    return BuildValue::makeExistingInput(info);
  }

  virtual bool isResultValid(BuildSystem& system,
                             const BuildValue& value) override {
    // It is an error if this command isn't configured properly.
    StringRef outputPath = getActualOutputPath();
    if (outputs.empty() || outputPath.empty())
      return false;

    // If the prior value wasn't for a successful command, recompute.
    if (!value.isSuccessfulCommand())
      return false;

    // If the prior command doesn't look like one for a link, recompute.
    if (value.getNumOutputs() != 1)
      return false;

    // Otherwise, assume the result is valid if its link status matches the
    // previous one.
    auto info = system.getFileSystem().getLinkInfo(outputPath);
    if (info.isMissing())
      return false;

    return info == value.getOutputInfo();
  }
  
  virtual void start(BuildSystem&, TaskInterface ti) override {
    // The command itself takes no inputs, so just treat any declared inputs as
    // "must follow" directives.
    //
    // FIXME: We should make this explicit once we have actual support for must
    // follow inputs.
    for (auto it = inputs.begin(), ie = inputs.end(); it != ie; ++it) {
      ti.mustFollow(BuildKey::makeNode(*it).toData());
    }
  }

  virtual void providePriorValue(BuildSystem&, TaskInterface ti,
                                 const BuildValue& value) override {
    // Ignored.
  }

  virtual void provideValue(BuildSystem&, TaskInterface ti,
                            uintptr_t inputID,
                            const KeyType& key, const BuildValue& value) override {
    assert(0 && "unexpected API call");
  }

  virtual void execute(BuildSystem& system,
                       TaskInterface ti,
                       QueueJobContext* context,
                       ResultFn resultFn) override {
    // It is an error if this command isn't configured properly.
    StringRef outputPath = getActualOutputPath();
    if (outputs.empty() || outputPath.empty()) {
      resultFn(BuildValue::makeFailedCommand());
      return;
    }

    auto& fs = system.getFileSystem();

    // Create the directory containing the symlink, if necessary.
    //
    // FIXME: Shared behavior with ExternalCommand.
    {
      auto parent = llvm::sys::path::parent_path(outputPath);
      if (!parent.empty()) {
        (void) fs.createDirectories(parent);
      }
    }

    // Create the symbolic link (note that despite the poorly chosen LLVM
    // name, this is a symlink).
    system.getDelegate().commandStarted(this);
    auto success = true;
    if (!fs.createSymlink(contents, outputPath.str())) {
      // On failure, we attempt to unlink the file and retry.
      fs.remove(outputPath.str());

      if (!fs.createSymlink(contents, outputPath.str())) {
        getBuildSystem(ti).getDelegate().commandHadError(this,
                       "unable to create symlink at '" + outputPath.str() + "'");
        success = false;
      }
    }
    system.getDelegate().commandFinished(this, success ? ProcessStatus::Succeeded : ProcessStatus::Failed);
    
    // Process the result.
    if (!success) {
      resultFn(BuildValue::makeFailedCommand());
      return;
    }

    // Capture the *link* information of the output.
    FileInfo outputInfo = fs.getLinkInfo(outputPath);
      
    // Complete with a successful result.
    resultFn(BuildValue::makeSuccessfulCommand(outputInfo));
  }

public:
  using Command::Command;
};

class SymlinkTool : public Tool {
  bool repairViaOwnershipAnalysis = false;
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "repair-via-ownership-analysis") {
      if (value == "true") {
        repairViaOwnershipAnalysis = true;
        return true;
      } else if(value == "false") {
        repairViaOwnershipAnalysis = false;
        return true;
      } else {
        ctx.error("invalid value for attribute: '" + name + "'");
        return false;
      }
    } else {
      ctx.error("unexpected attribute: '" + name + "'");
      return false;
    }
  }

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    auto res = llvm::make_unique<SymlinkCommand>(name);
    res->repairViaOwnershipAnalysis = repairViaOwnershipAnalysis;
    return res;
  }
};

#pragma mark - ArchiveTool implementation

class ArchiveShellCommand : public ExternalCommand {

  std::string archiveName;
  std::vector<std::string> archiveInputs;

  virtual void startExternalCommand(BuildSystem&, TaskInterface ti) override {
    return;
  }

  virtual void provideValueExternalCommand(
      BuildSystem&,
      TaskInterface ti,
      uintptr_t inputID,
      const BuildValue& value) override { }

  virtual void executeExternalCommand(
      BuildSystem&,
      TaskInterface ti,
      QueueJobContext* context,
      llvm::Optional<ProcessCompletionFn> completionFn) override {
    // First delete the current archive
    // TODO instead insert, update and remove files from the archive
    if (llvm::sys::fs::remove(archiveName, /*IgnoreNonExisting*/ true)) {
      if (completionFn.hasValue())
        completionFn.getValue()(ProcessStatus::Failed);
      return;
    }

    // Create archive
    auto args = getArgs();
    ti.spawn(context,
             std::vector<StringRef>(args.begin(), args.end()),
             {}, {true},
             {[completionFn](ProcessResult result) {
      if (completionFn.hasValue())
        completionFn.getValue()(result);
    }});
  }

  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
    if (getDescription().empty()) {
      llvm::raw_svector_ostream(result) << "Archiving " + archiveName;
    } else {
      llvm::raw_svector_ostream(result) << getDescription();
    }
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream stream(result);
    bool first = true;
    for (const auto& arg: getArgs()) {
      if (!first) {
        stream << " ";
      }
      first = false;
      stream << arg;
    }
  }
  
  virtual void configureInputs(const ConfigureContext& ctx,
                                const std::vector<Node*>& value) override {
    ExternalCommand::configureInputs(ctx, value);

    for (const auto& input: getInputs()) {
      if (!input->isVirtual()) {
        archiveInputs.push_back(input->getName());
      }
    }
    if (archiveInputs.empty()) {
      ctx.error("missing expected input");
    }
  }
  
  virtual void configureOutputs(const ConfigureContext& ctx,
                                const std::vector<Node*>& value) override {
    ExternalCommand::configureOutputs(ctx, value);

    for (const auto& output: getOutputs()) {
      if (!output->isVirtual()) {
        if (archiveName.empty()) {
          archiveName = output->getName();
        } else {
          ctx.error("unexpected explicit output: " + output->getName());
        }
      }
    }
    if (archiveName.empty()) {
      ctx.error("missing expected output");
    }
  }

  std::vector<std::string> getArgs() const {
    std::vector<std::string> args;
    if (const char *ar = std::getenv("AR"))
      args.push_back(std::string(ar));
    else
      args.push_back("ar");
    args.push_back("cr");
    args.push_back(archiveName);
    args.insert(args.end(), archiveInputs.begin(), archiveInputs.end());
    return args;
  }

public:
  using ExternalCommand::ExternalCommand;
};

class ArchiveTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<ArchiveShellCommand>(name);
  }
};

#pragma mark - SharedLibraryTool implementation

class SharedLibraryShellCommand : public ExternalCommand {

  // Defaults compilers. Can be overwritten on the build config is required.
  std::string executable = "";
  std::string sharedLibName;
  std::string compilerStyle;
  std::vector<std::string> sharedLibInputs;
  /// Additional arguments, as a string.
  std::vector<std::string> otherArgs;

  virtual void startExternalCommand(BuildSystem&, TaskInterface ti) override {
    return;
  }

  virtual void provideValueExternalCommand(
      BuildSystem&,
      TaskInterface ti,
      uintptr_t inputID,
      const BuildValue& value) override { }

  virtual void executeExternalCommand(
      BuildSystem&, TaskInterface ti, QueueJobContext* context,
      llvm::Optional<ProcessCompletionFn> completionFn) override {

    auto args = getArgs();
    ti.spawn(
        context, std::vector<StringRef>(args.begin(), args.end()), {},
        {true}, {[completionFn](ProcessResult result) {
          if (completionFn.hasValue())
            completionFn.getValue()(result);
        }});
  }

  virtual void
  getShortDescription(SmallVectorImpl<char>& result) const override {
    if (getDescription().empty()) {
      llvm::raw_svector_ostream(result)
          << "Creating Shared library: " + sharedLibName;
    } else {
      llvm::raw_svector_ostream(result) << getDescription();
    }
  }

  virtual void
  getVerboseDescription(SmallVectorImpl<char>& result) const override {
    llvm::raw_svector_ostream stream(result);
    bool first = true;
    for (const auto& arg : getArgs()) {
      if (first) {
        first = false;
      } else {
        stream << " ";
      }
      stream << arg;
    }
  }

  virtual void configureInputs(const ConfigureContext& ctx,
                               const std::vector<Node*>& value) override {
    ExternalCommand::configureInputs(ctx, value);

    for (const auto& input : getInputs()) {
      if (!input->isVirtual()) {
        sharedLibInputs.push_back(input->getName());
      }
    }
    if (sharedLibInputs.empty()) {
      ctx.error("SharedLibraryTool requires inputs to be specified");
    }
  }

  virtual void configureOutputs(const ConfigureContext& ctx,
                                const std::vector<Node*>& value) override {
    ExternalCommand::configureOutputs(ctx, value);

    for (const auto& output : getOutputs()) {
      if (!output->isVirtual()) {
        if (sharedLibName.empty()) {
          sharedLibName = output->getName();
        } else {
          ctx.error("unexpected explicit output: " + output->getName());
        }
      }
    }
    if (sharedLibName.empty()) {
      ctx.error("SharedLibraryTool requires the resulting shared library name "
                "to be specified");
    }
  }

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "executable") {
      executable = value;
    } else if (name == "other-args") {
      SmallVector<StringRef, 32> args;
      StringRef(value).split(args, " ", /*MaxSplit=*/-1,
                             /*KeepEmpty=*/false);
      otherArgs = std::vector<std::string>(args.begin(), args.end());
    } else if (name == "compiler-style") {
      if (value != "cl" && value != "clang" && value != "swiftc") {
        ctx.error("Unsupported : compiler-style'" + value +
                  "' for shared libraries'.  Supported styles are cl, clang, "
                  "and swiftc");
      }
      compilerStyle = value;
    }
    return true;
  }

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    if (name == "other-args") {
      otherArgs = std::vector<std::string>(values.begin(), values.end());
    } else {
      return ExternalCommand::configureAttribute(ctx, name, values);
    }

    return true;
  }

  std::vector<std::string> getArgs() const {
    std::vector<std::string> args;

    if (compilerStyle == "swiftc") {
      args.push_back(executable);
      args.push_back("-emit-library");
      args.insert(args.end(), sharedLibInputs.begin(), sharedLibInputs.end());
      args.push_back("-o");
      args.push_back(sharedLibName);
      args.insert(args.end(), otherArgs.begin(), otherArgs.end());
    } else if (compilerStyle == "clang") {
      args.push_back(executable);
      args.insert(args.end(), sharedLibInputs.begin(), sharedLibInputs.end());
      args.push_back("-o");
      args.push_back(sharedLibName);
      args.insert(args.end(), otherArgs.begin(), otherArgs.end());
      args.push_back("-shared");
    } else if (compilerStyle == "cl") {
      args.push_back(executable);
      args.insert(args.end(), sharedLibInputs.begin(), sharedLibInputs.end());
      args.push_back("/o");
      args.push_back(sharedLibName);
      args.push_back("/LD");
      args.push_back("/MD");
      args.push_back("/link");
      args.push_back("MSVCRT.lib");
    }

    return args;
  }

public:
  using ExternalCommand::ExternalCommand;
};

class SharedLibraryTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<SharedLibraryShellCommand>(name);
  }
};

#pragma mark - StaleFileRemovalTool implementation

class StaleFileRemovalCommand : public Command {
  std::string description;

  std::vector<std::string> expectedOutputs;
  mutable std::vector<std::string> filesToDelete;
  std::vector<std::string> roots;
  mutable bool computedFilesToDelete = false;

  BuildValue priorValue;
  bool hasPriorResult = false;

  std::string pathSeparators = llbuild::basic::sys::getPathSeparators();

  virtual void configureDescription(const ConfigureContext&, StringRef value) override {
    description = value;
  }

  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
    llvm::raw_svector_ostream(result) << (description.empty() ? "Stale file removal" : description);
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    computeFilesToDelete();

    getShortDescription(result);
    llvm::raw_svector_ostream(result) << ", stale files: [";
    for (auto fileToDelete : filesToDelete) {
      llvm::raw_svector_ostream(result) << fileToDelete;
      if (fileToDelete != *(--filesToDelete.end())) {
        llvm::raw_svector_ostream(result) << ", ";
      }
    }
    llvm::raw_svector_ostream(result) << "], roots: [";
    for (auto root : roots) {
      llvm::raw_svector_ostream(result) << root;
      if (root != *(--roots.end())) {
        llvm::raw_svector_ostream(result) << ", ";
      }
    }
    llvm::raw_svector_ostream(result) << "]";
  }

  virtual void configureInputs(const ConfigureContext& ctx,
                               const std::vector<Node*>& value) override {}

  virtual void configureOutputs(const ConfigureContext& ctx,
                                const std::vector<Node*>& value) override {}

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    if (name == "expectedOutputs") {
      expectedOutputs.reserve(values.size());
      for (auto value : values) {
        expectedOutputs.emplace_back(value.str());
      }
      return true;
    } else if (name == "roots") {
      roots.reserve(values.size());
      for (auto value : values) {
        roots.emplace_back(value.str());
      }
      return true;
    }

    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual BuildValue getResultForOutput(Node* node,
                                        const BuildValue& value) override {
    // If the value was a failed command, propagate the failure.
    if (value.isFailedCommand() || value.isPropagatedFailureCommand() ||
        value.isCancelledCommand())
      return BuildValue::makeFailedInput();
    if (value.isSkippedCommand())
      return BuildValue::makeSkippedCommand();

    // Otherwise, this was successful, return the value as-is.
    return BuildValue::fromData(value.toData());;
  }

  virtual bool isResultValid(BuildSystem& system,
                             const BuildValue& value) override {
    // Always re-run stale file removal.
    return false;
  }

  virtual void start(BuildSystem&, TaskInterface) override {}

  virtual void providePriorValue(BuildSystem&, TaskInterface,
                                 const BuildValue& value) override {
    hasPriorResult = true;
    priorValue = BuildValue::fromData(value.toData());
  }

  virtual void provideValue(BuildSystem&,
                            TaskInterface,
                            uintptr_t inputID,
                            const KeyType& key,
                            const BuildValue& value) override {
    assert(0 && "unexpected API call");
  }

  void computeFilesToDelete() const {
    if (computedFilesToDelete) {
      return;
    }

    std::vector<StringRef> priorValueList = priorValue.getStaleFileList();
    std::set<std::string> priorNodes(priorValueList.begin(), priorValueList.end());
    std::set<std::string> expectedNodes(expectedOutputs.begin(), expectedOutputs.end());

    std::set_difference(priorNodes.begin(), priorNodes.end(),
                        expectedNodes.begin(), expectedNodes.end(),
                        std::back_inserter(filesToDelete));

    computedFilesToDelete = true;
  }

  virtual void execute(BuildSystem& system,
                       TaskInterface ti,
                       QueueJobContext* context,
                       ResultFn resultFn) override {
    // Nothing to do if we do not have a prior result.
    if (!hasPriorResult || !priorValue.isStaleFileRemoval()) {
      system.getDelegate().commandStarted(this);
      system.getDelegate().commandFinished(this, ProcessStatus::Succeeded);
      resultFn(BuildValue::makeStaleFileRemoval(expectedOutputs));
      return;
    }

    computeFilesToDelete();

    system.getDelegate().commandStarted(this);

    for (auto fileToDelete : filesToDelete) {
      // If no root paths are specified, any path is valid.
      bool isLocatedUnderRootPath = roots.size() == 0 ? true : false;

      // If root paths are defined, stale file paths should be absolute.
      if (roots.size() > 0 &&
          pathSeparators.find(fileToDelete[0]) == std::string::npos) {
        system.getDelegate().commandHadWarning(this, "Stale file '" + fileToDelete + "' has a relative path. This is invalid in combination with the root path attribute.\n");
        continue;
      }

      // Check if the file is located under one of the allowed root paths.
      for (auto root : roots) {
        if (pathIsPrefixedByPath(fileToDelete, root)) {
          isLocatedUnderRootPath = true;
        }
      }

      if (!isLocatedUnderRootPath) {
        system.getDelegate().commandHadWarning(this, "Stale file '" + fileToDelete + "' is located outside of the allowed root paths.\n");
        continue;
      }

      if (getBuildSystem(ti).getFileSystem().remove(fileToDelete)) {
        system.getDelegate().commandHadNote(this, "Removed stale file '" + fileToDelete + "'\n");
      } else {
        // Do not warn if the file has already been deleted.
        if (errno != ENOENT) {
          system.getDelegate().commandHadWarning(this, "cannot remove stale file '" + fileToDelete + "': " + strerror(errno) + "\n");
        }
      }
    }

    system.getDelegate().commandFinished(this, ProcessStatus::Succeeded);

    // Complete with a successful result.
    resultFn(BuildValue::makeStaleFileRemoval(expectedOutputs));
  }

public:
  StaleFileRemovalCommand(const StringRef name)
  : Command(name), priorValue(BuildValue::makeInvalid()) {}
};

class StaleFileRemovalTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
                                  const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<StaleFileRemovalCommand>(name);
  }
};

#pragma mark - BuildSystemFileDelegate

BuildSystemDelegate& BuildSystemFileDelegate::getSystemDelegate() {
  return system.getDelegate();
}

void BuildSystemFileDelegate::setFileContentsBeingParsed(StringRef buffer) {
  getSystemDelegate().setFileContentsBeingParsed(buffer);
}

void BuildSystemFileDelegate::error(StringRef filename,
                                    const BuildFileToken& at,
                                    const Twine& message) {
  // Delegate to the system delegate.
  auto atSystemToken = BuildSystemDelegate::Token{at.start, at.length};
  system.error(filename, atSystemToken, message);
}

void
BuildSystemFileDelegate::cannotLoadDueToMultipleProducers(Node *output,
                                                          std::vector<Command*> commands) {
  getSystemDelegate().cannotBuildNodeDueToMultipleProducers(output, commands);
}

bool
BuildSystemFileDelegate::configureClient(const ConfigureContext& ctx,
                                         StringRef name,
                                         uint32_t version,
                                         const property_list_type& properties) {
  // The client must match the configured name of the build system.
  if (name != getSystemDelegate().getName())
    return false;

  // The client version must match the configured version.
  //
  // FIXME: We should give the client the opportunity to support a previous
  // schema version (auto-upgrade).
  if (version != getSystemDelegate().getVersion())
    return false;

  for (auto prop : properties) {
    if (prop.first == "file-system") {
      if (prop.second == "device-agnostic") {
        system.configureFileSystem(FileSystemMode::DeviceAgnostic);
      } else if (prop.second == "checksum-only") {
        system.configureFileSystem(FileSystemMode::ChecksumOnly);
      } else if (prop.second != "default") {
        ctx.error("unsupported client file-system: '" + prop.second + "'");
        return false;
      }
    }
  }

  return true;
}

std::unique_ptr<Tool>
BuildSystemFileDelegate::lookupTool(StringRef name) {
  // First, give the client an opportunity to create the tool.
  if (auto tool = getSystemDelegate().lookupTool(name)) {
    return tool;
  }

  // Otherwise, look for one of the builtin tool definitions.
  if (name == "shell") {
    return llvm::make_unique<ShellTool>(name);
  } else if (name == "phony") {
    return llvm::make_unique<PhonyTool>(name);
  } else if (name == "clang") {
    return llvm::make_unique<ClangTool>(name);
  } else if (name == "mkdir") {
    return llvm::make_unique<MkdirTool>(name);
  } else if (name == "symlink") {
    return llvm::make_unique<SymlinkTool>(name);
  } else if (name == "archive") {
    return llvm::make_unique<ArchiveTool>(name);
  } else if (name == "shared-library") {
    return llvm::make_unique<SharedLibraryTool>(name);
  } else if (name == "stale-file-removal") {
    return llvm::make_unique<StaleFileRemovalTool>(name);
  } else if (name == "swift-compiler") {
    return llvm::make_unique<SwiftCompilerTool>(name);
  }

  return nullptr;
}

void BuildSystemFileDelegate::loadedTarget(StringRef name,
                                           const Target& target) {
}

void BuildSystemFileDelegate::loadedDefaultTarget(StringRef target) {
}

void BuildSystemFileDelegate::loadedCommand(StringRef name,
                                            const Command& command) {
}

std::unique_ptr<Node>
BuildSystemFileDelegate::createNode(StringRef name,
                                    bool isImplicit) {
  return system.createNode(name, isImplicit);
}

}

#pragma mark - BuildSystem

BuildSystem::BuildSystem(BuildSystemDelegate& delegate, std::unique_ptr<basic::FileSystem> fileSystem)
  : impl(new BuildSystemImpl(*this, delegate, std::move(fileSystem)))
{
}

BuildSystem::~BuildSystem() {
  delete static_cast<BuildSystemImpl*>(impl);
}

BuildSystemDelegate& BuildSystem::getDelegate() {
  return static_cast<BuildSystemImpl*>(impl)->getDelegate();
}

basic::FileSystem& BuildSystem::getFileSystem() {
  return static_cast<BuildSystemImpl*>(impl)->getFileSystem();
}

bool BuildSystem::loadDescription(StringRef mainFilename) {
  return static_cast<BuildSystemImpl*>(impl)->loadDescription(mainFilename);
}

void BuildSystem::loadDescription(
    std::unique_ptr<BuildDescription> description) {
  return static_cast<BuildSystemImpl*>(impl)->loadDescription(
      std::move(description));
}

bool BuildSystem::loadDescription(
    llvm::function_ref<bool(BuildDescriptionBuilder&)> populate,
    StringRef originName) {
  return static_cast<BuildSystemImpl*>(impl)->loadDescription(populate,
                                                             originName);
}

bool BuildSystem::attachDB(StringRef path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->attachDB(path, error_out);
}

bool BuildSystem::enableTracing(StringRef path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->enableTracing(path, error_out);
}

llvm::Optional<BuildValue> BuildSystem::build(BuildKey key) {
  return static_cast<BuildSystemImpl*>(impl)->build(key);
}

bool BuildSystem::build(StringRef name) {
  return static_cast<BuildSystemImpl*>(impl)->build(name);
}

void BuildSystem::cancel() {
  if (impl) {
    static_cast<BuildSystemImpl*>(impl)->cancel();
  }
}

void BuildSystem::addCancellationDelegate(CancellationDelegate* del) {
  if (impl) {
    static_cast<BuildSystemImpl*>(impl)->addCancellationDelegate(del);
  }
}

void BuildSystem::removeCancellationDelegate(CancellationDelegate* del) {
  if (impl) {
    static_cast<BuildSystemImpl*>(impl)->removeCancellationDelegate(del);
  }
}

void BuildSystem::resetForBuild() {
  static_cast<BuildSystemImpl*>(impl)->resetForBuild();
}

uint32_t BuildSystem::getSchemaVersion() {
  return BuildSystemImpl::internalSchemaVersion;
}

ShellCommandHandler*
BuildSystem::resolveShellCommandHandler(ShellCommand* command) {
  return static_cast<BuildSystemImpl*>(impl)->resolveShellCommandHandler(command);
}

BuildNode* BuildSystem::lookupNode(StringRef name) {
  return static_cast<BuildSystemImpl*>(impl)->lookupNode(name);
}

// This function checks if the given path is prefixed by another path.
bool llbuild::buildsystem::pathIsPrefixedByPath(std::string path,
                                                std::string prefixPath) {
  std::string pathSeparators = llbuild::basic::sys::getPathSeparators();
  // Note: GCC 4.8 doesn't support the mismatch(first1, last1, first2, last2)
  // overload, just mismatch(first1, last1, first2), so we have to handle the
  // case where prefixPath is longer than path.
  if (prefixPath.length() > path.length()) {
    // The only case where the prefix can be longer and still be a valid prefix
    // is "/foo/" is a prefix of "/foo"
    return prefixPath.substr(0, prefixPath.length() - 1) == path &&
           pathSeparators.find(prefixPath[prefixPath.length() - 1]) !=
               std::string::npos;
  }
  auto res = std::mismatch(prefixPath.begin(), prefixPath.end(), path.begin());
  // Check if `prefixPath` has been exhausted or just a separator remains.
  bool isPrefix = res.first == prefixPath.end() ||
                  (pathSeparators.find(*(res.first++)) != std::string::npos);
  // Check if `path` has been exhausted or just a separator remains.
  return isPrefix &&
         (res.second == path.end() ||
          (pathSeparators.find(*(res.second++)) != std::string::npos));
}
